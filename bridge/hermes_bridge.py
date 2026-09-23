#!/usr/bin/env python3
"""
Vibe Watch <-> Hermes Agent bridge.

Replaces the macOS/Codex host of the original vibewatch project. The watch
connects over Wi-Fi (WebSocket) and this service:

  * receives push-to-talk audio (16 kHz PCM16 mono) from the watch mic
  * transcribes it locally with faster-whisper
  * starts a Hermes Agent run (/v1/runs), one Hermes session per agent slot
    (the six dots on the watch face), and follows its event stream
  * forwards Hermes' dangerous-command approvals to the watch as a pop-up and
    answers them with the button the user pressed (/v1/runs/{id}/approval)
  * speaks the reply with Piper TTS and streams PCM back to the watch
  * drives the six agent LEDs with the same v.oai.thstatus messages the
    original firmware already understands

Run it in the same Proxmox container as Hermes so Hermes' API can stay bound
to 127.0.0.1 and only this bridge is exposed on the LAN.
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import re
import time
from collections import deque
from datetime import datetime, timedelta
from dataclasses import dataclass, field
from pathlib import Path

import httpx
import numpy as np
import websockets
from websockets.asyncio.server import ServerConnection, serve

try:
    from dotenv import load_dotenv

    load_dotenv(Path(__file__).with_name(".env"))
except ImportError:
    pass

# ---------------------------------------------------------------------------
# Configuration (see .env.example)
# ---------------------------------------------------------------------------

LISTEN_HOST = os.getenv("BRIDGE_HOST", "0.0.0.0")
LISTEN_PORT = int(os.getenv("BRIDGE_PORT", "8765"))
WATCH_TOKEN = os.environ.get("WATCH_TOKEN", "")

HERMES_URL = os.getenv("HERMES_URL", "http://127.0.0.1:8642").rstrip("/")
HERMES_KEY = os.environ.get("HERMES_API_KEY", "")
HERMES_MODEL = os.getenv("HERMES_MODEL", "hermes-agent")
HERMES_TIMEOUT = float(os.getenv("HERMES_TIMEOUT", "900"))

WHISPER_MODEL = os.getenv("WHISPER_MODEL", "base.en")
WHISPER_DEVICE = os.getenv("WHISPER_DEVICE", "cpu")
WHISPER_COMPUTE = os.getenv("WHISPER_COMPUTE", "int8")
WHISPER_LANGUAGE = os.getenv("WHISPER_LANGUAGE", "en") or None

PIPER_VOICE = os.getenv("PIPER_VOICE", str(Path(__file__).with_name("models") / "en_US-lessac-medium.onnx"))
TTS_GAIN = float(os.getenv("TTS_GAIN", "0.6"))  # StopWatch amp clips easily
MAX_SPOKEN_CHARS = int(os.getenv("MAX_SPOKEN_CHARS", "900"))

VOICE_INSTRUCTIONS = os.getenv(
    "VOICE_INSTRUCTIONS",
    "The user is talking to you through a small wrist-worn device with a speaker "
    "and no screen for text. Reply in short, natural spoken sentences. Do not use "
    "markdown, bullet points, tables, URLs, or code blocks in your reply; if you "
    "produce code or files, save them and briefly say where.",
)
PLAN_INSTRUCTIONS = (
    "PLAN MODE is on: describe what you intend to do and wait for approval "
    "before running tools that change anything."
)
APPROVE_TEXT = os.getenv("APPROVE_TEXT", "Yes, go ahead.")
REJECT_TEXT = os.getenv("REJECT_TEXT", "No, don't do that. Stop and wait for my next instruction.")

# Must stay below Hermes' approvals.timeout (default 300 s) so the watch's
# "expired" (= deny) arrives before Hermes gives up on its own.
APPROVAL_TTL_S = int(os.getenv("APPROVAL_TTL_S", "240"))
APPROVAL_SPEAK = os.getenv("APPROVAL_SPEAK", "1") not in ("0", "false", "no")

# Hermes profiles (swipe between them on the watch). Format:
#   HERMES_PROFILES=home=http://127.0.0.1:8642,coding=http://127.0.0.1:8643
# Keys: HERMES_API_KEY_HOME, HERMES_API_KEY_CODING (fallback HERMES_API_KEY).
# Colours (optional): HERMES_COLOR_HOME=9D74FF
# Unset = one profile using HERMES_URL / HERMES_API_KEY, as before.
HERMES_PROFILES = os.getenv("HERMES_PROFILES", "").strip()
MAX_PROFILES = 4
PROFILE_PALETTE = [0x9D74FF, 0x33C4E8, 0xFFAC28, 0x42E88B]

# Status ring / page on the watch
STATUS_INTERVAL_S = int(os.getenv("STATUS_INTERVAL_S", "30"))
DAILY_TOKEN_BUDGET = int(os.getenv("DAILY_TOKEN_BUDGET", "0"))  # 0 = no budget, ring stays full
USAGE_FILE = Path(os.getenv("USAGE_FILE", str(Path(__file__).with_name("usage.json"))))

AGENT_COUNT = 6
MIC_RATE = 16000

log = logging.getLogger("vibewatch-bridge")

# Watch LED states: color, brightness, effect (1 solid, 4 strong pulse, 6 soft pulse), speed
STATE_STYLE = {
    "idle": (0x3A4050, 0.35, 1, 0.0),
    "listening": (0x2D8CFF, 1.0, 1, 0.0),
    "thinking": (0x9D74FF, 1.0, 4, 0.6),
    "speaking": (0x33C4E8, 1.0, 6, 0.8),
    "unheard": (0x42E88B, 1.0, 6, 0.4),
    "done": (0x42E88B, 0.7, 1, 0.0),
    "error": (0xF55367, 1.0, 1, 0.0),
    "approval": (0xFFAC28, 1.0, 4, 0.9),
}

# Key names sent by the unchanged watch UI (see sendOuterActionEvent in main.cpp)
KEY_FAST, KEY_OK, KEY_NG, KEY_PLAN, KEY_AI = "ACT06", "ACT07", "ACT08", "ACT09", "ACT12"


# ---------------------------------------------------------------------------
# Speech engines (loaded once, used from worker threads)
# ---------------------------------------------------------------------------


class Speech:
    def __init__(self) -> None:
        from faster_whisper import WhisperModel
        from piper import PiperVoice

        log.info("Loading Whisper model %s (%s/%s)", WHISPER_MODEL, WHISPER_DEVICE, WHISPER_COMPUTE)
        self.whisper = WhisperModel(WHISPER_MODEL, device=WHISPER_DEVICE, compute_type=WHISPER_COMPUTE)
        log.info("Loading Piper voice %s", PIPER_VOICE)
        self.piper = PiperVoice.load(PIPER_VOICE)
        self._stt_lock = asyncio.Lock()
        self._tts_lock = asyncio.Lock()

    def _transcribe(self, pcm: bytes) -> str:
        audio = np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768.0
        segments, _ = self.whisper.transcribe(
            audio, language=WHISPER_LANGUAGE, beam_size=1, vad_filter=True
        )
        return " ".join(s.text.strip() for s in segments).strip()

    def _synthesize(self, text: str) -> tuple[bytes, int]:
        chunks, rate = [], 22050
        for chunk in self.piper.synthesize(text):
            rate = chunk.sample_rate
            chunks.append(chunk.audio_int16_bytes)
        pcm = np.frombuffer(b"".join(chunks), dtype=np.int16).astype(np.float32) * TTS_GAIN
        return np.clip(pcm, -32768, 32767).astype("<i2").tobytes(), rate

    async def transcribe(self, pcm: bytes) -> str:
        async with self._stt_lock:
            return await asyncio.to_thread(self._transcribe, pcm)

    async def synthesize(self, text: str) -> tuple[bytes, int]:
        async with self._tts_lock:
            return await asyncio.to_thread(self._synthesize, text)


def spoken(text: str) -> str:
    """Strip markdown the model may still emit and cap the length for the speaker."""
    text = re.sub(r"```.*?```", " (code omitted) ", text, flags=re.S)
    text = re.sub(r"`([^`]*)`", r"\1", text)
    text = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", text)
    text = re.sub(r"https?://\S+", "a link", text)
    text = re.sub(r"^[#>\-\*\+\s]+", "", text, flags=re.M)
    text = re.sub(r"[*_~|]", "", text)
    text = re.sub(r"\s+", " ", text).strip()
    if len(text) > MAX_SPOKEN_CHARS:
        cut = text[:MAX_SPOKEN_CHARS]
        text = cut[: cut.rfind(". ") + 1 or len(cut)] + " That's the short version."
    return text


# ---------------------------------------------------------------------------
# Hermes Agent client (Runs API)
# ---------------------------------------------------------------------------

TERMINAL_EVENTS = {"run.completed", "run.failed", "run.cancelled", "run.interrupted"}
TERMINAL_STATUSES = {"completed", "failed", "cancelled", "interrupted"}


class HermesRunError(RuntimeError):
    pass


class Hermes:
    def __init__(self, url: str = HERMES_URL, key: str = HERMES_KEY) -> None:
        self.url = url
        self.http = httpx.AsyncClient(
            base_url=url.rstrip("/"),
            timeout=httpx.Timeout(60.0, connect=10.0),
            headers={"Authorization": f"Bearer {key}"},
        )

    async def start_run(self, text: str, session_id: str, session_key: str, plan: bool) -> str:
        instructions = VOICE_INSTRUCTIONS + ("\n\n" + PLAN_INSTRUCTIONS if plan else "")
        resp = await self.http.post(
            "/v1/runs",
            headers={"X-Hermes-Session-Key": session_key},
            json={
                "model": HERMES_MODEL,
                "input": text,
                "instructions": instructions,
                "session_id": session_id,
            },
        )
        resp.raise_for_status()
        return resp.json()["run_id"]

    async def events(self, run_id: str):
        """Yield decoded events from GET /v1/runs/{id}/events (SSE, data-only frames)."""
        timeout = httpx.Timeout(HERMES_TIMEOUT, connect=10.0)
        async with self.http.stream("GET", f"/v1/runs/{run_id}/events", timeout=timeout) as resp:
            resp.raise_for_status()
            async for line in resp.aiter_lines():
                if line.startswith("data: "):
                    try:
                        yield json.loads(line[6:])
                    except json.JSONDecodeError:
                        continue

    async def status(self, run_id: str) -> dict:
        resp = await self.http.get(f"/v1/runs/{run_id}")
        resp.raise_for_status()
        return resp.json()

    async def answer_approval(self, run_id: str, request_id: str, approve: bool) -> bool:
        resp = await self.http.post(
            f"/v1/runs/{run_id}/approval",
            json={"choice": "once" if approve else "deny", "request_id": request_id},
        )
        if resp.status_code == 409:
            # Already resolved elsewhere, withdrawn, or the run ended.
            log.info("approval %s no longer pending: %s", request_id[:8], resp.text[:200])
            return False
        resp.raise_for_status()
        return True

    async def stop(self, run_id: str) -> None:
        try:
            await self.http.post(f"/v1/runs/{run_id}/stop")
        except httpx.HTTPError as exc:
            log.warning("stop %s failed: %s", run_id, exc)

    async def health(self) -> bool:
        try:
            r = await self.http.get("/health")
            return r.status_code == 200
        except httpx.HTTPError:
            return False

    async def snapshot(self) -> dict:
        """Health, active runs and enabled jobs. Never raises; 'offline' on failure."""
        snap = {"health": "offline", "runs": None, "jobs": 0}
        try:
            r = await self.http.get("/health/detailed")
            if r.status_code == 200:
                data = r.json()
                snap["health"] = "ok" if data.get("status") == "ok" else "degraded"
                runs = find_key(data, "active_api_runs")
                if isinstance(runs, int):
                    snap["runs"] = runs
            elif await self.health():
                snap["health"] = "ok"  # older Hermes without /health/detailed
        except (httpx.HTTPError, ValueError):
            return snap
        try:
            r = await self.http.get("/api/jobs")
            if r.status_code == 200:
                jobs = r.json().get("jobs", [])
                snap["jobs"] = sum(1 for j in jobs if isinstance(j, dict)
                                   and j.get("enabled", True) and not j.get("paused_at")
                                   and j.get("state") != "paused")
        except (httpx.HTTPError, ValueError, AttributeError):
            pass
        return snap

    async def supports_approvals(self) -> bool:
        try:
            r = await self.http.get("/v1/capabilities")
            feats = r.json().get("features", {})
            return bool(feats.get("run_approval_response") or feats.get("run_approval")
                        or feats.get("approval_events"))
        except (httpx.HTTPError, ValueError):
            return False


# ---------------------------------------------------------------------------
# Status: token ledger + Hermes health, pushed to watches as "host.status"
# ---------------------------------------------------------------------------


def seconds_to_local_midnight() -> int:
    now = datetime.now().astimezone()
    midnight = (now + timedelta(days=1)).replace(hour=0, minute=0, second=0, microsecond=0)
    return max(0, int((midnight - now).total_seconds()))


class UsageLedger:
    """Tokens used today (local time) in total and per Hermes session; survives restarts."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.day = datetime.now().astimezone().date().isoformat()
        self.totals: dict[str, int] = {}
        self.sessions: dict[str, int] = {}
        try:
            data = json.loads(path.read_text())
            if data.get("day") == self.day:
                self.totals = {k: int(v) for k, v in data.get("totals", {}).items()}
                if not self.totals and "total" in data:  # file from the single-profile version
                    self.totals = {"hermes": int(data["total"])}
                self.sessions = {k: int(v) for k, v in data.get("sessions", {}).items()}
        except (OSError, ValueError):
            pass

    def _roll(self) -> None:
        today = datetime.now().astimezone().date().isoformat()
        if today != self.day:
            self.day, self.totals, self.sessions = today, {}, {}

    def add(self, profile: str, session_id: str, usage: dict | None) -> None:
        if not isinstance(usage, dict):
            return
        tokens = usage.get("total_tokens")
        if not isinstance(tokens, (int, float)) or tokens <= 0:
            tokens = sum(v for k, v in usage.items()
                         if k in ("input_tokens", "output_tokens") and isinstance(v, (int, float)))
        tokens = int(tokens)
        if tokens <= 0:
            return
        self._roll()
        self.totals[profile] = self.totals.get(profile, 0) + tokens
        self.sessions[session_id] = self.sessions.get(session_id, 0) + tokens
        try:
            self.path.write_text(json.dumps({"day": self.day, "totals": self.totals, "sessions": self.sessions}))
        except OSError as exc:
            log.warning("could not save %s: %s", self.path, exc)

    def today(self, profile: str) -> int:
        self._roll()
        return self.totals.get(profile, 0)

    def session(self, session_id: str) -> int:
        self._roll()
        return self.sessions.get(session_id, 0)


def find_key(obj, key: str):
    """First value for `key` anywhere in nested dicts/lists (Hermes' readiness shape varies)."""
    if isinstance(obj, dict):
        if key in obj:
            return obj[key]
        for v in obj.values():
            found = find_key(v, key)
            if found is not None:
                return found
    elif isinstance(obj, list):
        for v in obj:
            found = find_key(v, key)
            if found is not None:
                return found
    return None


LEDGER = UsageLedger(USAGE_FILE)


@dataclass
class Profile:
    name: str
    url: str
    key: str
    color: int
    hermes: "Hermes | None" = None
    snapshot: dict = field(default_factory=lambda: {"health": None, "runs": None, "jobs": 0})


def load_profiles() -> list[Profile]:
    if not HERMES_PROFILES:
        return [Profile("hermes", HERMES_URL, HERMES_KEY, PROFILE_PALETTE[0])]
    profiles: list[Profile] = []
    for i, entry in enumerate(e for e in HERMES_PROFILES.split(",") if e.strip()):
        name, _, url = entry.strip().partition("=")
        name = re.sub(r"[^a-z0-9_-]", "", name.strip().lower())
        if not name or not url or len(profiles) >= MAX_PROFILES:
            raise SystemExit(f"Bad HERMES_PROFILES entry {entry!r} (max {MAX_PROFILES}, name=url)")
        key = os.getenv(f"HERMES_API_KEY_{name.upper()}", HERMES_KEY)
        raw_color = os.getenv(f"HERMES_COLOR_{name.upper()}", "")
        color = int(raw_color, 16) if raw_color else PROFILE_PALETTE[i % 4]
        profiles.append(Profile(name, url.strip(), key, color))
    return profiles


# ---------------------------------------------------------------------------
# Approvals: one pop-up on the watch at a time, the rest queue here
# ---------------------------------------------------------------------------


@dataclass
class PendingApproval:
    request_id: str
    run_id: str
    slot: int
    title: str
    detail: str
    created: float = field(default_factory=time.monotonic)
    future: asyncio.Future = field(default_factory=lambda: asyncio.get_running_loop().create_future())

    def ttl_ms(self) -> int:
        left = APPROVAL_TTL_S - (time.monotonic() - self.created)
        return max(5000, int(left * 1000))


def approval_texts(event: dict) -> tuple[str, str]:
    title = (event.get("description") or "").strip() or "Run a flagged command"
    detail = re.sub(r"\s+", " ", str(event.get("command") or "")).strip()
    return title[:60], detail[:150]


# ---------------------------------------------------------------------------
# Per-watch state (survives Wi-Fi reconnects)
# ---------------------------------------------------------------------------


@dataclass
class Slot:
    state: str = "idle"
    generation: int = 0  # bumped by the AI button to start a fresh session
    task: asyncio.Task | None = None
    run_id: str | None = None
    last_reply: str = ""
    unheard: bool = False


@dataclass
class Device:
    device: str
    speech: Speech
    profiles: list[Profile]
    ws: ServerConnection | None = None
    slots: list[Slot] = field(default_factory=list)  # AGENT_COUNT per profile, indexed by gid
    active: int = 0      # profile shown on the watch
    selected: int = 0    # gid = profile * AGENT_COUNT + slot
    plan_mode: bool = False
    recording: bool = False
    rec_slot: int = 0
    rec_buf: bytearray = field(default_factory=bytearray)
    send_lock: asyncio.Lock = field(default_factory=asyncio.Lock)
    approval_queue: deque = field(default_factory=deque)
    approval_current: PendingApproval | None = None
    last_attention: tuple = ()

    def __post_init__(self) -> None:
        self.slots = [Slot() for _ in range(len(self.profiles) * AGENT_COUNT)]

    # -- profile / slot addressing -------------------------------------------

    @property
    def multi(self) -> bool:
        return len(self.profiles) > 1

    def gid(self, slot: int, profile: int | None = None) -> int:
        p = self.active if profile is None else profile
        p = min(max(p, 0), len(self.profiles) - 1)
        return p * AGENT_COUNT + min(max(slot, 0), AGENT_COUNT - 1)

    def prof(self, gid: int) -> Profile:
        return self.profiles[gid // AGENT_COUNT]

    def h(self, gid: int) -> "Hermes":
        return self.prof(gid).hermes

    def label(self, gid: int) -> str:
        n = gid % AGENT_COUNT + 1
        return f"{self.prof(gid).name} agent {n}" if self.multi else f"Agent {n}"

    def attention(self) -> list[int]:
        """Per profile: 2 = approval waiting, 1 = unheard reply, 0 = nothing."""
        out = []
        for p in range(len(self.profiles)):
            gids = range(p * AGENT_COUNT, (p + 1) * AGENT_COUNT)
            if any(self.approval_waiting_for(g) for g in gids):
                out.append(2)
            elif any(self.slots[g].unheard for g in gids):
                out.append(1)
            else:
                out.append(0)
        return out

    async def push_profiles(self, force: bool = False) -> None:
        att = tuple(self.attention())
        if not force and att == self.last_attention:
            return
        self.last_attention = att
        await self.send_json({"method": "profile.list", "params": {
            "names": [p.name for p in self.profiles],
            "colors": [p.color for p in self.profiles],
            "active": self.active,
            "attention": list(att),
        }})

    async def select_profile(self, index: int) -> None:
        index = min(max(index, 0), len(self.profiles) - 1)
        self.active = index
        self.selected = self.gid(self.selected % AGENT_COUNT)
        log.info("[%s] profile -> %s", self.device, self.profiles[index].name)
        await self.push_profiles(force=True)
        await self.push_status()
        await self.push_host_status()
        await self.show_label(self.profiles[index].name.capitalize())

    # -- outbound --------------------------------------------------------

    async def send_json(self, obj: dict) -> None:
        if self.ws is None:
            return
        try:
            async with self.send_lock:
                await self.ws.send(json.dumps(obj, separators=(",", ":")))
        except websockets.ConnectionClosed:
            pass

    async def send_audio(self, pcm: bytes, rate: int) -> bool:
        if self.ws is None:
            return False
        try:
            async with self.send_lock:
                await self.ws.send(json.dumps({"m": "tts.begin", "p": {"rate": rate, "bytes": len(pcm)}}))
                for off in range(0, len(pcm), 4096):
                    await self.ws.send(pcm[off : off + 4096])
                await self.ws.send(json.dumps({"m": "tts.end"}))
            return True
        except websockets.ConnectionClosed:
            return False

    async def push_status(self) -> None:
        params = []
        for i in range(AGENT_COUNT):
            g = self.gid(i)
            state = self.slots[g].state
            if self.approval_waiting_for(g):
                state = "approval"
            c, b, e, s = STATE_STYLE[state]
            params.append({"id": i, "c": c, "b": b, "e": e, "s": s})
        await self.send_json({"method": "v.oai.thstatus", "params": params})
        await self.push_profiles()

    async def set_state(self, idx: int, state: str) -> None:
        self.slots[idx].state = state
        await self.push_status()

    async def push_host_status(self) -> None:
        profile = self.profiles[self.active]
        snap = profile.snapshot
        if snap["health"] is None:
            return  # nothing measured yet: the watch keeps showing "no data"
        today = LEDGER.today(profile.name)
        remaining = 100.0
        if DAILY_TOKEN_BUDGET > 0:
            remaining = max(0.0, 100.0 * (1 - today / DAILY_TOKEN_BUDGET))
        runs = snap["runs"]
        if runs is None:
            runs = sum(1 for i in range(AGENT_COUNT) if self.busy(self.gid(i)))
        await self.send_json({"method": "host.status", "params": {
            "health": snap["health"],
            "runs": runs,
            "jobs": snap["jobs"],
            "tokens_today": today,
            "budget": DAILY_TOKEN_BUDGET,
            "remaining_pct": round(remaining, 1),
            "reset_s": seconds_to_local_midnight(),
            "slot_tokens": [LEDGER.session(self.session_id(self.gid(i))) for i in range(AGENT_COUNT)],
            "ttl_s": max(30, STATUS_INTERVAL_S * 3),
        }})

    async def show_label(self, text: str) -> None:
        await self.send_json({"method": "host.focused_app", "params": {"appName": text[:18]}})

    async def announce(self, text: str) -> None:
        pcm, rate = await self.speech.synthesize(text)
        if not self.recording:
            await self.send_audio(pcm, rate)

    async def speak(self, idx: int, text: str) -> None:
        text = spoken(text)
        if not text:
            return
        pcm, rate = await self.speech.synthesize(text)
        if self.recording or self.ws is None:
            self.slots[idx].unheard = True
            await self.set_state(idx, "unheard")
            return
        await self.set_state(idx, "speaking")
        if not await self.send_audio(pcm, rate):
            self.slots[idx].unheard = True
            await self.set_state(idx, "unheard")
            return
        self.slots[idx].unheard = False
        await asyncio.sleep(len(pcm) / 2 / rate)  # approximate playback time
        if self.slots[idx].state == "speaking":
            await self.set_state(idx, "done")

    # -- connection lifecycle --------------------------------------------

    async def attach(self, ws: ServerConnection) -> None:
        if self.ws is not None and self.ws is not ws:
            await self.ws.close(code=4000, reason="replaced by new connection")
        self.ws = ws
        self.recording = False

    async def detach(self, ws: ServerConnection) -> None:
        if self.ws is ws:
            self.ws = None
            self.recording = False
        # Runs and approvals keep going; the pop-up is re-sent on reconnect and
        # the watch-side TTL still fails closed if nobody answers.

    # -- approvals ---------------------------------------------------------

    def approval_waiting_for(self, slot: int) -> bool:
        if self.approval_current and self.approval_current.slot == slot:
            return True
        return any(p.slot == slot for p in self.approval_queue)

    async def request_approval(self, slot: int, run_id: str, event: dict) -> bool:
        """Queue an approval, show it on the watch, and wait for the answer."""
        rid = str(event.get("request_id") or "")
        if not rid:
            log.warning("[%s] approval without request_id; denying", self.device)
            return False
        title, detail = approval_texts(event)
        pa = PendingApproval(request_id=rid, run_id=run_id, slot=slot, title=title, detail=detail)
        self.approval_queue.append(pa)
        log.info("[%s] %s approval %s: %s | %s", self.device, self.label(slot), rid[:8], title, detail)
        await self.pump_approvals()
        try:
            choice = await asyncio.wait_for(asyncio.shield(pa.future), timeout=APPROVAL_TTL_S + 30)
        except asyncio.TimeoutError:
            choice = "expired"
        finally:
            await self.drop_approval(rid, notify_watch=True)
        log.info("[%s] approval %s -> %s", self.device, rid[:8], choice)
        return choice == "approve"

    async def pump_approvals(self) -> None:
        if self.approval_current is None and self.approval_queue:
            self.approval_current = self.approval_queue.popleft()
            await self.show_current_approval(announce=True)
        await self.push_status()

    async def show_current_approval(self, announce: bool) -> None:
        pa = self.approval_current
        if pa is None:
            return
        await self.send_json({"m": "approval.request", "p": {
            "id": pa.request_id, "slot": pa.slot % AGENT_COUNT, "profile": pa.slot // AGENT_COUNT,
            "profile_name": self.prof(pa.slot).name if self.multi else "", "kind": "EXEC",
            "title": pa.title, "detail": pa.detail, "ttl_ms": pa.ttl_ms(),
        }})
        if announce and APPROVAL_SPEAK:
            asyncio.create_task(self.announce(f"{self.label(pa.slot)} needs approval: {pa.title}."))

    async def on_approval_decision(self, rid: str, choice: str) -> None:
        pa = self.approval_current
        if pa is None or pa.request_id != rid:
            log.info("[%s] stale approval decision %s for %s", self.device, choice, rid[:8])
            return
        if choice == "busy":
            # Watch still had another pop-up; re-send shortly.
            await asyncio.sleep(1.0)
            await self.show_current_approval(announce=False)
            return
        if choice == "invalid":
            choice = "reject"
        if not pa.future.done():
            pa.future.set_result(choice)

    async def drop_approval(self, rid: str, notify_watch: bool) -> None:
        """Remove an approval (answered, withdrawn, or its run ended)."""
        for pa in list(self.approval_queue):
            if pa.request_id == rid:
                self.approval_queue.remove(pa)
                if not pa.future.done():
                    pa.future.set_result("cancelled")
        if self.approval_current and self.approval_current.request_id == rid:
            pa = self.approval_current
            self.approval_current = None
            if not pa.future.done():
                pa.future.set_result("cancelled")
                if notify_watch:
                    await self.send_json({"m": "approval.cancel", "p": {"id": rid}})
        await self.pump_approvals()

    async def decide_for_slot(self, slot: int, approve: bool) -> bool:
        """OK/NG from the normal layer while that agent's approval is pending."""
        pa = self.approval_current
        if pa and pa.slot == slot and not pa.future.done():
            await self.send_json({"m": "approval.cancel", "p": {"id": pa.request_id}})
            pa.future.set_result("approve" if approve else "reject")
            return True
        return False

    # -- agent turns -------------------------------------------------------

    def session_id(self, idx: int) -> str:
        n, gen = idx % AGENT_COUNT + 1, self.slots[idx].generation
        if not self.multi:  # unchanged from the single-profile version
            return f"vibewatch-{self.device}-agent{n}-g{gen}"
        return f"vibewatch-{self.device}-{self.prof(idx).name}-agent{n}-g{gen}"

    def busy(self, idx: int) -> bool:
        t = self.slots[idx].task
        return t is not None and not t.done()

    def submit(self, idx: int, text: str) -> None:
        if self.busy(idx):
            log.info("[%s] agent%d busy; ignoring: %s", self.device, idx + 1, text)
            asyncio.create_task(self.announce(f"{self.label(idx)} is still working."))
            return
        self.slots[idx].task = asyncio.create_task(self._turn(idx, text))

    async def _turn(self, idx: int, text: str) -> None:
        slot = self.slots[idx]
        try:
            await self.set_state(idx, "thinking")
            log.info("[%s] %s <- %r", self.device, self.label(idx), text)
            slot.run_id = await self.h(idx).start_run(
                text, self.session_id(idx), f"vibewatch:{self.device}", self.plan_mode)
            reply = await self.follow_run(idx, slot.run_id)
            if reply is None:
                await self.set_state(idx, "idle")
                return
            log.info("[%s] %s -> %r", self.device, self.label(idx), reply[:200])
            slot.last_reply = reply
            if idx == self.selected and not self.recording and self.ws is not None:
                # Speak outside this task so the slot accepts a new question
                # (or barge-in) while the reply is still playing.
                asyncio.create_task(self.speak(idx, reply))
            else:
                slot.unheard = True
                await self.set_state(idx, "unheard")
        except asyncio.CancelledError:
            if slot.run_id:
                await self.h(idx).stop(slot.run_id)
            await self.set_state(idx, "idle")
            raise
        except Exception as exc:  # noqa: BLE001
            log.exception("[%s] agent%d failed", self.device, idx + 1)
            await self.set_state(idx, "error")
            await self.speak_error(exc)
        finally:
            slot.run_id = None

    async def follow_run(self, idx: int, run_id: str) -> str | None:
        """Consume the run's events until it ends. Returns the reply, or None if cancelled."""
        approval_tasks: dict[str, asyncio.Task] = {}
        terminal: dict | None = None

        def handle_approval(event: dict) -> None:
            rid = str(event.get("request_id") or "")
            if rid and rid not in approval_tasks:
                approval_tasks[rid] = asyncio.create_task(self._approval_flow(idx, run_id, event))

        try:
            try:
                async for ev in self.h(idx).events(run_id):
                    name = ev.get("event", "")
                    if name == "approval.request":
                        handle_approval(ev)
                    elif name == "approval.responded":
                        # Answered from another client (CLI, Telegram, dashboard).
                        rid = str(ev.get("request_id") or "")
                        if rid:
                            await self.drop_approval(rid, notify_watch=True)
                    elif name == "tool.started" and ev.get("tool"):
                        await self.show_label(str(ev["tool"]))
                    elif name in TERMINAL_EVENTS:
                        terminal = ev
                        break
            except httpx.HTTPError as exc:
                log.warning("[%s] event stream for %s broke: %s; polling", self.device, run_id, exc)

            # Stream ended without a terminal event: poll the run status instead.
            while terminal is None:
                st = await self.h(idx).status(run_id)
                status = st.get("status")
                if status in TERMINAL_STATUSES:
                    terminal = {"event": f"run.{status}", **st}
                    break
                if status == "waiting_for_approval" and isinstance(st.get("approval"), dict):
                    handle_approval(st["approval"])
                await asyncio.sleep(2.0)
        finally:
            for rid, task in approval_tasks.items():
                if not task.done():
                    task.cancel()
                await self.drop_approval(rid, notify_watch=True)

        name = terminal.get("event")
        LEDGER.add(self.prof(idx).name, self.session_id(idx), terminal.get("usage"))
        await self.push_host_status()
        await self.show_label("Hermes")
        if name == "run.completed":
            return str(terminal.get("output") or "").strip() or "Done."
        if name == "run.failed":
            raise HermesRunError(str(terminal.get("error") or "run failed"))
        return None  # cancelled / interrupted

    async def _approval_flow(self, idx: int, run_id: str, event: dict) -> None:
        rid = str(event["request_id"])
        approve = await self.request_approval(idx, run_id, event)
        try:
            await self.h(idx).answer_approval(run_id, rid, approve)
        except httpx.HTTPError as exc:
            log.warning("[%s] answering approval %s failed: %s", self.device, rid[:8], exc)
        await self.set_state(idx, "thinking" if approve else self.slots[idx].state)

    async def speak_error(self, exc: Exception) -> None:
        if isinstance(exc, httpx.ConnectError):
            msg = "I can't reach Hermes. Check that the gateway is running."
        elif isinstance(exc, httpx.HTTPStatusError) and exc.response.status_code == 401:
            msg = "Hermes rejected the API key."
        elif isinstance(exc, httpx.HTTPStatusError) and exc.response.status_code == 429:
            msg = "Hermes is busy with too many runs. Try again shortly."
        else:
            msg = "Something went wrong talking to Hermes."
        try:
            await self.announce(msg)
        except Exception:  # noqa: BLE001
            pass

    # -- inbound -----------------------------------------------------------

    async def on_text(self, raw: str) -> None:
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            return
        m = msg.get("m") or msg.get("method") or ""
        p = msg.get("p") or msg.get("params") or {}

        if m == "hello":
            log.info("[%s] hello %s", self.device, p)
            await self.push_profiles(force=True)
            await self.push_status()
            hermes = self.profiles[self.active].hermes
            ok = await hermes.health()
            if ok and not await hermes.supports_approvals():
                log.warning("This Hermes build does not advertise run approvals; update Hermes.")
            await self.show_label(("Hermes" if not self.multi else self.profiles[self.active].name.capitalize())
                                  if ok else "Hermes offline")
            await self.push_host_status()
            # Re-show an approval that was open when Wi-Fi dropped.
            await self.show_current_approval(announce=False)
        elif m == "voice.start":
            self.recording = True
            self.rec_slot = self.gid(int(p.get("slot", self.selected % AGENT_COUNT)),
                                     int(p.get("profile", self.active)))
            self.selected = self.rec_slot
            self.rec_buf.clear()
            await self.set_state(self.rec_slot, "listening")
        elif m == "voice.end":
            # Snapshot now; transcription runs in the background so the
            # socket keeps servicing key presses meanwhile.
            self.recording = False
            idx, pcm = self.rec_slot, bytes(self.rec_buf)
            self.rec_buf.clear()
            asyncio.create_task(self.finish_recording(idx, pcm))
        elif m == "approval.decision":
            await self.on_approval_decision(str(p.get("id", "")), str(p.get("choice", "")))
        elif m == "v.oai.hid":
            await self.on_key(str(p.get("k", "")), bool(p.get("act")),
                              self.gid(int(p.get("slot", self.selected % AGENT_COUNT)),
                                       int(p.get("profile", self.active))))
        elif m == "profile.select":
            await self.select_profile(int(p.get("index", 0)))
        elif m == "device.battery":
            log.debug("[%s] battery %s", self.device, p)

    async def finish_recording(self, idx: int, pcm: bytes) -> None:
        seconds = len(pcm) / 2 / MIC_RATE
        if seconds < 0.35:
            await self.set_state(idx, "done" if self.slots[idx].last_reply else "idle")
            return
        await self.set_state(idx, "thinking")
        text = await self.speech.transcribe(pcm)
        log.info("[%s] heard %.1fs: %r", self.device, seconds, text)
        if not text:
            await self.set_state(idx, "idle")
            return
        self.submit(idx, text)

    async def on_key(self, key: str, pressed: bool, slot: int) -> None:
        if not pressed:
            return
        if key.startswith("AG"):
            self.selected = self.gid(int(key[2:]), slot // AGENT_COUNT)
            s = self.slots[self.selected]
            if s.unheard and s.last_reply:
                asyncio.create_task(self.speak(self.selected, s.last_reply))
            return
        self.selected = slot
        s = self.slots[slot]
        if key == KEY_OK:
            if not await self.decide_for_slot(slot, approve=True) and not self.busy(slot):
                self.submit(slot, APPROVE_TEXT)
        elif key == KEY_NG:
            if await self.decide_for_slot(slot, approve=False):
                return
            if self.busy(slot):
                # Real cancel: Hermes stops the run at the next safe point.
                if s.run_id:
                    await self.h(slot).stop(s.run_id)
                await self.send_json({"m": "tts.stop"})
            else:
                self.submit(slot, REJECT_TEXT)
        elif key == KEY_FAST:
            if s.last_reply:
                asyncio.create_task(self.speak(slot, s.last_reply))
        elif key == KEY_PLAN:
            self.plan_mode = not self.plan_mode
            await self.show_label("Plan mode" if self.plan_mode else "Hermes")
        elif key == KEY_AI:
            if self.busy(slot) and s.task:
                s.task.cancel()
            s.generation += 1
            s.last_reply = ""
            s.unheard = False
            await self.set_state(slot, "idle")
            await self.show_label(f"New chat {slot % AGENT_COUNT + 1}")

    async def on_audio(self, data: bytes) -> None:
        if self.recording:
            self.rec_buf.extend(data)


# ---------------------------------------------------------------------------
# Server
# ---------------------------------------------------------------------------


async def main() -> None:
    logging.basicConfig(level=os.getenv("LOG_LEVEL", "INFO"), format="%(asctime)s %(levelname)s %(message)s")
    if not WATCH_TOKEN or not (HERMES_KEY or HERMES_PROFILES):
        raise SystemExit("Set WATCH_TOKEN and HERMES_API_KEY (or HERMES_PROFILES + keys) in bridge/.env")

    speech = Speech()
    profiles = load_profiles()
    for prof in profiles:
        prof.hermes = Hermes(prof.url, prof.key)
        if not await prof.hermes.health():
            log.warning("Hermes '%s' not reachable at %s yet (will keep trying)", prof.name, prof.url)
        elif not await prof.hermes.supports_approvals():
            log.warning("Hermes '%s' does not advertise run approvals; pop-ups won't appear", prof.name)
    log.info("Profiles: %s", ", ".join(f"{p.name}={p.url}" for p in profiles))

    devices: dict[str, Device] = {}

    async def status_poller() -> None:
        while True:
            for prof in profiles:
                prof.snapshot.update(await prof.hermes.snapshot())
            for dev in list(devices.values()):
                await dev.push_host_status()
            await asyncio.sleep(STATUS_INTERVAL_S)

    poller = asyncio.create_task(status_poller())  # noqa: F841 (kept alive for the process)

    async def handler(ws: ServerConnection) -> None:
        headers = ws.request.headers
        if ws.request.path != "/watch" or headers.get("Authorization") != f"Bearer {WATCH_TOKEN}":
            log.warning("Rejected connection from %s", ws.remote_address)
            await ws.close(code=4401, reason="unauthorized")
            return
        name = re.sub(r"[^a-zA-Z0-9_-]", "", headers.get("X-Vibe-Device", "vibe-watch")) or "vibe-watch"
        device = devices.get(name)
        if device is None:
            device = devices[name] = Device(device=name, speech=speech, profiles=profiles)
        await device.attach(ws)
        log.info("Watch %s connected from %s", name, ws.remote_address)
        try:
            async for frame in ws:
                if isinstance(frame, bytes):
                    await device.on_audio(frame)
                else:
                    await device.on_text(frame)
        except websockets.ConnectionClosed:
            pass
        finally:
            await device.detach(ws)
            log.info("Watch %s disconnected", name)

    async with serve(handler, LISTEN_HOST, LISTEN_PORT, max_size=2**20, ping_interval=20):
        log.info("Bridge listening on ws://%s:%d/watch -> Hermes %s", LISTEN_HOST, LISTEN_PORT, HERMES_URL)
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
