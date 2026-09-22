# Vibe Watch → Hermes Agent over Wi-Fi, with voice

This fork swaps the original Bluetooth-HID / macOS-Codex host for Wi-Fi and a
small bridge service that sits next to Hermes Agent on your Proxmox container.

```
 StopWatch (ESP32-S3)                 Proxmox LXC 192.168.0.197
 ┌──────────────────────┐   Wi-Fi    ┌──────────────────────────────────────┐
 │ touch / buttons / UI │ ─────────► │ hermes_bridge.py  :8765  (/watch)     │
 │ MEMS mic  16 kHz PCM │ WebSocket  │   faster-whisper  (speech → text)     │
 │ speaker   ◄── PCM    │ ◄───────── │   Piper           (text → speech)     │
 └──────────────────────┘            │        │ HTTP 127.0.0.1:8642           │
                                     │        ▼                               │
                                     │ hermes gateway  (API server)           │
                                     └──────────────────────────────────────┘
```

## What changed vs. upstream

| Area | Upstream | This fork |
| --- | --- | --- |
| Transport | NimBLE HID vendor report, 61-byte chunks | Wi-Fi STA + WebSocket client (`src/net_link.cpp`) |
| Host | macOS app speaking the Codex Micro protocol | `bridge/hermes_bridge.py` → Hermes `/v1/runs` |
| Approvals | OK/NG keypresses | Full-screen pop-up tied to Hermes' approval `request_id` |
| Mic button | Sent ACT10/ACT11; the Mac did dictation | Watch records its own mic and streams PCM (`src/voice.cpp`) |
| Replies | Shown on the Mac | Spoken on the watch speaker |
| Settings “PAIR” | BLE pairing slot | “CONNECT”: picks watch id `vibe-watch-1..3` and reconnects |
| Status bar | ON / PAIR | ON / HOST (Wi-Fi ok, bridge down) / WIFI / REC / SAY |

The JSON message shapes (`v.oai.hid`, `v.oai.thstatus`, `host.focused_app`,
`device.status`) are unchanged, so the UI code is almost untouched.

## Controls

| Control | Action |
| --- | --- |
| Agent dots 1–6 | Six independent Hermes conversations. Selecting a dot with a pulsing-green (unheard) reply plays it. |
| Center mic / hold right button | Push-to-talk to the selected agent. Talking over a reply cuts it off. |
| **Approval pop-up** | **Right button / OK = run it once. Left button / NG = deny.** No answer before the countdown = deny. |
| OK | Answers that agent's pending approval if there is one; otherwise says “Yes, go ahead.” |
| NG | Denies that agent's pending approval; else **stops** the running task; else says “No, don’t do that.” |
| FAST | Replay the last reply |
| PLAN | Toggle plan mode (Hermes is told to propose before acting) |
| AI | Start a fresh conversation in that slot |

LED colors: grey idle · blue listening · purple pulsing thinking · amber pulsing waiting for approval · cyan speaking · green pulsing unheard · green done · red error.

## Approvals

When Hermes wants to run something its safety rules flag (deleting files,
`sudo`, force-pushing, …) it pauses the run and the watch shows a full-screen
card: the agent number, Hermes' description ("recursive delete"), the redacted
command, a countdown, and NG / OK buttons. The watch buzzes, chimes, and (with
`APPROVAL_SPEAK=1`) says "Agent 3 needs approval: recursive delete."

- Answers are tied to Hermes' `request_id`, so a late or duplicate press can never approve a different command.
- The buttons ignore presses for the first 0.7 s, and a press that was already held when the card appeared doesn't count.
- The card waits until you finish a push-to-talk sentence before appearing.
- Silence is not consent: when `APPROVAL_TTL_S` runs out the watch reports `expired` and the bridge denies it.
- If Wi-Fi drops, the bridge keeps the request and re-shows it when the watch reconnects.
- If you answer from somewhere else (Hermes CLI, Telegram, dashboard), the card disappears.
- Several pending approvals queue up and are shown one at a time.

Hermes settings that matter (`~/.hermes/config.yaml`):

```yaml
approvals:
  mode: manual     # smart lets a guardian model auto-approve low-risk commands;
                   # manual sends every flagged command to the watch
  timeout: 300     # keep this above APPROVAL_TTL_S in bridge/.env
```

Only runs started through the Runs API get interactive approvals. Hermes'
`/v1/responses` and chat endpoints treat API clients as unattended and deny
flagged commands outright, which is why the bridge now uses `/v1/runs`.

## 1. Hermes (inside the container)

Add to `~/.hermes/.env` and restart `hermes gateway`:

```
API_SERVER_ENABLED=true
API_SERVER_KEY=<long random string>
# Leave API_SERVER_HOST at its 127.0.0.1 default: only the bridge talks to it.
```

Check: `curl http://127.0.0.1:8642/health` → `{"status": "ok"}`

## 2. Bridge (same container)

```bash
apt install -y python3-venv
mkdir -p /opt/vibewatch-bridge && cp bridge/* bridge/.env.example /opt/vibewatch-bridge/
cd /opt/vibewatch-bridge
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
mkdir -p models && .venv/bin/python -m piper.download_voices en_US-lessac-medium --data-dir models
cp .env.example .env && nano .env        # WATCH_TOKEN, HERMES_API_KEY
.venv/bin/python hermes_bridge.py        # first run downloads the Whisper model
```

Then install the service: `cp vibewatch-bridge.service /etc/systemd/system/ && systemctl enable --now vibewatch-bridge`.

Proxmox: if the container or node firewall is on, allow TCP 8765 in to the container from your LAN.

Test without the watch from any PC: `python fake_watch.py question.wav --token <WATCH_TOKEN>` → writes `reply.wav`.
Approval pop-ups show up in the terminal as a y/n prompt. A good test question is
"delete the folder /tmp/vibewatch-test", which Hermes should flag.

## 3. Firmware

```bash
cp include/secrets.example.h include/secrets.h   # Wi-Fi SSID/password, bridge IP, token
python3 -m platformio run -e m5stack-stopwatch --target upload
python3 -m platformio device monitor              # watch the log
```

The ESP32-S3 only does 2.4 GHz Wi-Fi — make sure your SSID has a 2.4 GHz band.

## Notes and limits

- The mic and speaker share the codec’s I2S clock pins, so the watch is half-duplex:
  it stops the speaker to listen and restarts it afterwards.
- Replies are synthesized in full, then sent (~44 kB/s of PCM). Long answers are
  trimmed to `MAX_SPOKEN_CHARS`; Hermes is asked to answer in short spoken sentences.
- NG while busy calls Hermes' `/v1/runs/{id}/stop`; Hermes stops at its next safe point.
- Each watch slot is a Hermes session named `vibewatch-<watch>-agentN-gG`, so you can
  find and continue them from the Hermes dashboard or CLI. The AI button starts a new one.
- Wi-Fi uses more battery than BLE. Expect noticeably shorter runtime on the 450 mAh cell.
- Traffic is plain `ws://` on your LAN, authenticated by the shared token.
  Don’t port-forward 8765 to the internet.

## Credits

The approval state machine (`lib/vibe_approval`) and the pop-up layout are adapted
from [neilshare/vibewatch](https://github.com/neilshare/vibewatch) (MIT), itself a
fork of [GOROman/vibewatch](https://github.com/GOROman/vibewatch).

## Tests

```bash
python3 -m platformio test -e native   # approval state machine, runs on your PC
```
