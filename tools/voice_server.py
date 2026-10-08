#!/usr/bin/env python3
"""AI Voice Keyboard — PC-side WebSocket server.

Receives PCM audio from ESP32, transcribes with faster-whisper,
and types the result into the focused application.

Usage:
    python voice_server.py [--host 0.0.0.0] [--port 12345] [--model base] [--lang zh]

Dependencies:
    pip install websockets faster-whisper keyboard numpy
"""

import argparse
import asyncio
import io
import json
import struct
import sys
import wave
import numpy as np

try:
    import websockets
except ImportError:
    print("Install websockets: pip install websockets")
    sys.exit(1)

pcm_buf = bytearray()
model = None
model_name = "base"
lang = "zh"
whisper_compute = "int8"


def load_model():
    global model
    if model is None:
        try:
            from faster_whisper import WhisperModel
            print(f"Loading faster-whisper model '{model_name}'...")
            model = WhisperModel(model_name, device="cpu", compute_type=whisper_compute)
            print("Model loaded.")
        except ImportError:
            print("Install faster-whisper: pip install faster-whisper")
            sys.exit(1)
    return model


def pcm_to_wav(pcm_data, sample_rate=16000, bits=16, channels=1):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(bits // 8)
        w.setframerate(sample_rate)
        w.writeframes(pcm_data)
    return buf.getvalue()


def transcribe(wav_data):
    m = load_model()
    try:
        segments, info = m.transcribe(
            io.BytesIO(wav_data),
            language=lang if lang else None,
            beam_size=5,
        )
        text = " ".join(s.text.strip() for s in segments)
        return text.strip()
    except Exception as e:
        print(f"Transcription error: {e}")
        return ""


def keyboard_type(text):
    if not text:
        return
    try:
        import keyboard
        keyboard.write(text)
        return
    except Exception:
        pass
    try:
        import pyautogui
        pyautogui.write(text)
    except Exception as e:
        print(f"Keyboard input failed: {e}")


async def handle_audio(websocket):
    global pcm_buf
    client = websocket.remote_address
    print(f"ESP32 connected: {client}")
    pcm_buf = bytearray()

    try:
        async for message in websocket:
            if isinstance(message, str):
                try:
                    msg = json.loads(message)
                except json.JSONDecodeError:
                    continue

                msg_type = msg.get("type", "")

                if msg_type == "start":
                    pcm_buf = bytearray()
                    print("Recording started...")

                elif msg_type == "stop":
                    print(f"Recording stopped. {len(pcm_buf)} bytes PCM received.")
                    if len(pcm_buf) < 640:
                        await websocket.send(json.dumps({
                            "type": "error",
                            "msg": "Audio too short"
                        }))
                        pcm_buf = bytearray()
                        continue

                    wav_data = pcm_to_wav(pcm_buf)
                    print("Transcribing...")
                    text = transcribe(wav_data)
                    print(f"Result: {text}")

                    await websocket.send(json.dumps({
                        "type": "result",
                        "text": text
                    }))

                    keyboard_type(text)
                    pcm_buf = bytearray()

            elif isinstance(message, bytes):
                pcm_buf.extend(message)

    except websockets.exceptions.ConnectionClosed:
        print(f"ESP32 disconnected: {client}")
    except Exception as e:
        print(f"Error: {e}")


async def main():
    parser = argparse.ArgumentParser(description="AI Voice Keyboard Server")
    parser.add_argument("--host", default="0.0.0.0", help="Listen address")
    parser.add_argument("--port", type=int, default=12345, help="Listen port")
    parser.add_argument("--model", default="base", help="Whisper model size")
    parser.add_argument("--lang", default="zh", help="Language hint (zh/en)")
    parser.add_argument("--compute", default="int8", help="Compute type (int8/float32)")
    args = parser.parse_args()

    global model_name, lang, whisper_compute
    model_name = args.model
    lang = args.lang
    whisper_compute = args.compute

    print(f"AI Voice Keyboard Server")
    print(f"  Host: {args.host}:{args.port}")
    print(f"  Model: {args.model} ({args.compute})")
    print(f"  Language: {args.lang}")
    print(f"  Waiting for ESP32 connection...")

    async with websockets.serve(handle_audio, args.host, args.port):
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())