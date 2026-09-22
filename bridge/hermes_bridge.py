#!/usr/bin/env python3
"""
Vibe Watch <-> Hermes Agent bridge.

Replaces the macOS/Codex host of the original vibewatch project. The watch
connects over Wi-Fi (WebSocket) and this service:

  * receives push-to-talk audio (16 kHz PCM16 mono) from the watch mic
  * transcribes it locally with faster-whisper
  * sends the text to Hermes Agent's API server (/v1/responses), one named
    conversation per agent slot (the six dots on the watch face)
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
APPROVE_TEXT = os.getenv("APPROVE_TEXT", "Yes, approved. Go ahead.")
REJECT_TEXT = os.getenv("REJECT_TEXT", "No, don't do that. Stop and wait for my next instruction.")

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
# Hermes Agent client
# ---------------------------------------------------------------------------


class Hermes:
    def __init__(self) -> None:
        self.http = httpx.AsyncClient(
            base_url=HERMES_URL,
            timeout=httpx.Timeout(HERMES_TIMEOUT, connect=10.0),
            headers={"Authorization": f"Bearer {HERMES_KEY}"},
        )

    async def ask(self, text: str, conversation: str, session_key: str, plan: bool) -> str:
        instructions = VOICE_INSTRUCTIONS + ("\n\n" + PLAN_INSTRUCTIONS if plan else "")
        resp = await self.http.post(
            "/v1/responses",
            headers={"X-Hermes-Session-Key": session_key},
            json={
                "model": HERMES_MODEL,
                "input": text,
                "instructions": instructions,
                "conversation": conversation,
                "store": True,
            },
        )
        resp.raise_for_status()
        data = resp.json()
        if isinstance(data.get("output_text"), str) and data["output_text"].strip():
            return data["output_text"]
        parts: list[str] = []
        for item in data.get("output", []):
            if item.get("type") == "message":
                for c in item.get("content", []):
                    if c.get("type") in ("output_text", "text"):
                        parts.append(c.get("text", ""))
        return "\n".join(parts).strip() or "I finished, but had nothing to say."

    async def health(self) -> bool:
        try:
            r = await self.http.get("/health")
            return r.status_code == 200
        except httpx.HTTPError:
            return False


# ---------------------------------------------------------------------------
# Per-watch session
# ---------------------------------------------------------------------------


@dataclass
class Slot:
    state: str = "idle"
    generation: int = 0  # bumped by the AI button to start a fresh conversation
    task: asyncio.Task | None = None
    last_reply: str = ""
    unheard: bool = False


@dataclass
class Watch:
    ws: ServerConnection
    device: str
    speech: Speech
    hermes: Hermes
    slots: list[Slot] = field(default_factory=lambda: [Slot() for _ in range(AGENT_COUNT)])
    selected: int = 0
    plan_mode: bool = False
    recording: bool = False
    rec_slot: int = 0
    rec_buf: bytearray = field(default_factory=bytearray)
    send_lock: asyncio.Lock = field(default_factory=asyncio.Lock)

    # -- outbound --------------------------------------------------------

    async def send_json(self, obj: dict) -> None:
        async with self.send_lock:
            await self.ws.send(json.dumps(obj, separators=(",", ":")))

    async def push_status(self) -> None:
        params = []
        for i, slot in enumerate(self.slots):
            c, b, e, s = STATE_STYLE[slot.state]
            params.append({"id": i, "c": c, "b": b, "e": e, "s": s})
        await self.send_json({"method": "v.oai.thstatus", "params": params})

    async def set_state(self, idx: int, state: str) -> None:
        self.slots[idx].state = state
        await self.push_status()

    async def show_label(self, text: str) -> None:
        await self.send_json({"method": "host.focused_app", "params": {"appName": text}})

    async def speak(self, idx: int, text: str) -> None:
        text = spoken(text)
        if not text:
            return
        pcm, rate = await self.speech.synthesize(text)
        if self.recording:
            # User started talking while we synthesized; keep it for later.
            self.slots[idx].unheard = True
            await self.set_state(idx, "unheard")
            return
        await self.set_state(idx, "speaking")
        async with self.send_lock:
            await self.ws.send(json.dumps({"m": "tts.begin", "p": {"rate": rate, "bytes": len(pcm)}}))
            for off in range(0, len(pcm), 4096):
                await self.ws.send(pcm[off : off + 4096])
            await self.ws.send(json.dumps({"m": "tts.end"}))
        self.slots[idx].unheard = False
        # Approximate playback time, then settle the LED.
        await asyncio.sleep(len(pcm) / 2 / rate)
        if self.slots[idx].state == "speaking":
            await self.set_state(idx, "done")

    # -- agent turns -----------------------------------------------------

    def conversation_name(self, idx: int) -> str:
        return f"{self.device}-agent{idx + 1}-g{self.slots[idx].generation}"

    def submit(self, idx: int, text: str) -> None:
        slot = self.slots[idx]
        if slot.task and not slot.task.done():
            log.info("[%s] slot %d busy; queuing is not supported, ignoring: %s", self.device, idx, text)
            return
        slot.task = asyncio.create_task(self._turn(idx, text))

    async def _turn(self, idx: int, text: str) -> None:
        slot = self.slots[idx]
        try:
            await self.set_state(idx, "thinking")
            log.info("[%s] agent%d <- %r", self.device, idx + 1, text)
            reply = await self.hermes.ask(
                text, self.conversation_name(idx), f"vibewatch:{self.device}", self.plan_mode
            )
            log.info("[%s] agent%d -> %r", self.device, idx + 1, reply[:200])
            slot.last_reply = reply
            if idx == self.selected and not self.recording:
                await self.speak(idx, reply)
            else:
                slot.unheard = True
                await self.set_state(idx, "unheard")
        except asyncio.CancelledError:
            await self.set_state(idx, "idle")
            raise
        except Exception as exc:  # noqa: BLE001
            log.exception("[%s] agent%d failed", self.device, idx + 1)
            await self.set_state(idx, "error")
            try:
                await self.speak_error(idx, exc)
            except Exception:  # noqa: BLE001
                pass

    async def speak_error(self, idx: int, exc: Exception) -> None:
        if isinstance(exc, httpx.ConnectError):
            msg = "I can't reach Hermes. Check that the gateway is running."
        elif isinstance(exc, httpx.HTTPStatusError) and exc.response.status_code == 401:
            msg = "Hermes rejected the API key."
        else:
            msg = "Something went wrong talking to Hermes."
        pcm, rate = await self.speech.synthesize(msg)
        async with self.send_lock:
            await self.ws.send(json.dumps({"m": "tts.begin", "p": {"rate": rate, "bytes": len(pcm)}}))
            await self.ws.send(pcm)
            await self.ws.send(json.dumps({"m": "tts.end"}))

    # -- inbound ---------------------------------------------------------

    async def on_text(self, raw: str) -> None:
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            return
        m = msg.get("m") or msg.get("method") or ""
        p = msg.get("p") or msg.get("params") or {}

        if m == "hello":
            log.info("[%s] hello %s", self.device, p)
            await self.push_status()
            ok = await self.hermes.health()
            await self.show_label("Hermes" if ok else "Hermes offline")
        elif m == "voice.start":
            self.recording = True
            self.rec_slot = int(p.get("slot", self.selected))
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
        elif m == "v.oai.hid":
            await self.on_key(str(p.get("k", "")), bool(p.get("act")), int(p.get("slot", self.selected)))
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
            self.selected = int(key[2:])
            s = self.slots[self.selected]
            if s.unheard and s.last_reply:
                asyncio.create_task(self.speak(self.selected, s.last_reply))
            return
        self.selected = slot
        s = self.slots[slot]
        busy = s.task is not None and not s.task.done()
        if key == KEY_OK:
            self.submit(slot, APPROVE_TEXT)
        elif key == KEY_NG:
            if busy:
                # Stops waiting on this turn. Hermes may finish its current
                # step in the background; see README for the Runs API upgrade.
                s.task.cancel()
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
            if busy:
                s.task.cancel()
            s.generation += 1
            s.last_reply = ""
            s.unheard = False
            await self.set_state(slot, "idle")
            await self.show_label(f"New chat {slot + 1}")

    async def on_audio(self, data: bytes) -> None:
        if self.recording:
            self.rec_buf.extend(data)


# ---------------------------------------------------------------------------
# Server
# ---------------------------------------------------------------------------


async def main() -> None:
    logging.basicConfig(level=os.getenv("LOG_LEVEL", "INFO"), format="%(asctime)s %(levelname)s %(message)s")
    if not WATCH_TOKEN or not HERMES_KEY:
        raise SystemExit("Set WATCH_TOKEN and HERMES_API_KEY in bridge/.env")

    speech = Speech()
    hermes = Hermes()
    if not await hermes.health():
        log.warning("Hermes API not reachable at %s yet (will keep trying per request)", HERMES_URL)

    async def handler(ws: ServerConnection) -> None:
        headers = ws.request.headers
        if ws.request.path != "/watch" or headers.get("Authorization") != f"Bearer {WATCH_TOKEN}":
            log.warning("Rejected connection from %s", ws.remote_address)
            await ws.close(code=4401, reason="unauthorized")
            return
        device = re.sub(r"[^a-zA-Z0-9_-]", "", headers.get("X-Vibe-Device", "vibe-watch")) or "vibe-watch"
        watch = Watch(ws=ws, device=device, speech=speech, hermes=hermes)
        log.info("Watch %s connected from %s", device, ws.remote_address)
        try:
            async for frame in ws:
                if isinstance(frame, bytes):
                    await watch.on_audio(frame)
                else:
                    await watch.on_text(frame)
        except websockets.ConnectionClosed:
            pass
        finally:
            for s in watch.slots:
                if s.task and not s.task.done():
                    s.task.cancel()
            log.info("Watch %s disconnected", device)

    async with serve(handler, LISTEN_HOST, LISTEN_PORT, max_size=2**20, ping_interval=20):
        log.info("Bridge listening on ws://%s:%d/watch -> Hermes %s", LISTEN_HOST, LISTEN_PORT, HERMES_URL)
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
