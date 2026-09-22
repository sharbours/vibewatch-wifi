#!/usr/bin/env python3
"""
Test the bridge without flashing the watch: sends a WAV file as a
push-to-talk utterance and saves the spoken reply to reply.wav.

  python fake_watch.py question.wav --host 192.168.0.197 --token <WATCH_TOKEN>

The WAV should be 16 kHz mono 16-bit (e.g. `arecord -f S16_LE -r 16000 -c 1 q.wav`).
If Hermes asks for approval, the pop-up is printed here and you answer y/n.
"""
import argparse
import asyncio
import json
import wave

import websockets


async def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--host", default="192.168.0.197")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--token", required=True)
    ap.add_argument("--slot", type=int, default=0)
    a = ap.parse_args()

    with wave.open(a.wav, "rb") as w:
        assert w.getframerate() == 16000 and w.getnchannels() == 1 and w.getsampwidth() == 2, \
            "need 16 kHz mono 16-bit WAV"
        pcm = w.readframes(w.getnframes())

    uri = f"ws://{a.host}:{a.port}/watch"
    headers = {"Authorization": f"Bearer {a.token}", "X-Vibe-Device": "fake-watch"}
    async with websockets.connect(uri, additional_headers=headers, max_size=2**23) as ws:
        await ws.send(json.dumps({"m": "hello", "p": {"device": "fake-watch"}}))
        await ws.send(json.dumps({"m": "voice.start", "p": {"slot": a.slot, "rate": 16000}}))
        for off in range(0, len(pcm), 2048):
            await ws.send(pcm[off:off + 2048])
            await asyncio.sleep(0.064)  # real-time pacing, like the watch
        await ws.send(json.dumps({"m": "voice.end"}))

        audio, rate = bytearray(), 22050
        saw_reply_state = False  # slot LED turns cyan while the real reply plays
        async for frame in ws:
            if isinstance(frame, bytes):
                audio.extend(frame)
                continue
            msg = json.loads(frame)
            m = msg.get("m") or msg.get("method")
            if m == "v.oai.thstatus":
                states = {p["id"]: hex(p["c"]) for p in msg["params"]}
                saw_reply_state = saw_reply_state or states.get(a.slot) == hex(0x33C4E8)
                print("LEDs:", states)
            elif m == "host.focused_app":
                print("Label:", msg["params"]["appName"])
            elif m == "approval.request":
                p = msg["p"]
                print(f"\n*** APPROVAL (agent {p['slot'] + 1}) {p['title']}\n    {p['detail']}")
                answer = await asyncio.to_thread(input, "    Approve? [y/N] ")
                choice = "approve" if answer.strip().lower().startswith("y") else "reject"
                await ws.send(json.dumps({"m": "approval.decision", "p": {"id": p["id"], "choice": choice}}))
            elif m == "approval.cancel":
                print("    (approval withdrawn)")
            elif m == "tts.begin":
                rate = msg["p"]["rate"]
                audio.clear()
            elif m == "tts.end":
                if len(audio) / 2 / rate < 4 and not saw_reply_state:
                    continue  # short announcement (e.g. "needs approval"), keep waiting
                with wave.open("reply.wav", "wb") as w:
                    w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
                    w.writeframes(bytes(audio))
                print(f"Saved reply.wav ({len(audio) / 2 / rate:.1f}s)")
                return


if __name__ == "__main__":
    asyncio.run(main())
