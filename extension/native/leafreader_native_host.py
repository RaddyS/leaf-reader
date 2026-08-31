#!/usr/bin/env python3
"""Chrome native messaging host for Leaf Reader's local Piper voices."""

import json
import os
import signal
import struct
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

DATA_ROOT = Path.home() / ".local" / "share" / "leafreader"
PYTHON = DATA_ROOT / "piper-venv" / "bin" / "python"
VOICES_ROOT = DATA_ROOT / "voices"
write_lock = threading.Lock()
process_lock = threading.Lock()
current_process: subprocess.Popen | None = None
generation = 0


def send(message: dict) -> None:
    payload = json.dumps(message, ensure_ascii=False).encode("utf-8")
    with write_lock:
        sys.stdout.buffer.write(struct.pack("=I", len(payload)))
        sys.stdout.buffer.write(payload)
        sys.stdout.buffer.flush()


def voices() -> dict[str, Path]:
    return {
        model.stem: model
        for model in sorted(VOICES_ROOT.rglob("*.onnx"))
        if Path(f"{model}.json").is_file()
    }


def label(voice_id: str) -> str:
    parts = voice_id.split("-")
    if len(parts) < 3:
        return f"{voice_id} (Piper)"
    locale, quality = parts[0].replace("_", "-"), parts[-1].title()
    name = " ".join(parts[1:-1]).replace("_", " ").title()
    return f"{name} — {locale}, {quality}"


def stop_current() -> None:
    global current_process, generation
    generation += 1
    with process_lock:
        process = current_process
        current_process = None
    if process and process.poll() is None:
        try:
            os.killpg(process.pid, signal.SIGTERM)
            process.wait(timeout=2)
        except (ProcessLookupError, subprocess.TimeoutExpired):
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)


def synthesize(request_id: int, voice_id: str, text: str, speed: float, token: int) -> None:
    global current_process
    try:
        model = voices()[voice_id]
        length_scale = max(0.55, min(1.6, 1.0 / speed))
        with tempfile.TemporaryDirectory(prefix="leafreader-extension-") as temp_dir:
            output = Path(temp_dir) / "speech.wav"
            command = [str(PYTHON), "-m", "piper", "-m", str(model), "-f", str(output),
                       "--length-scale", str(length_scale), "--", text]
            process = subprocess.Popen(command, start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            with process_lock:
                current_process = process
            stderr = process.communicate()[1].decode("utf-8", "replace").strip()
            if token != generation:
                return
            if process.returncode:
                raise RuntimeError(stderr or "Piper synthesis failed")
            send({"id": request_id, "event": "playing"})
            process = subprocess.Popen(["pw-play", str(output)], start_new_session=True,
                                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            with process_lock:
                current_process = process
            stderr = process.communicate()[1].decode("utf-8", "replace").strip()
            if token == generation and process.returncode:
                raise RuntimeError(stderr or "Audio playback failed")
            if token == generation:
                send({"id": request_id, "event": "finished"})
    except Exception as error:
        if token == generation:
            send({"id": request_id, "event": "error", "error": str(error)})
    finally:
        with process_lock:
            current_process = None


def handle(message: dict) -> None:
    global generation
    request_id = message.get("id")
    command = message.get("command")
    if command == "list_voices":
        available = voices()
        send({"id": request_id, "ok": True,
              "voices": [{"id": key, "label": label(key)} for key in available]})
        return
    if command == "stop":
        stop_current()
        send({"id": request_id, "ok": True})
        return
    if command == "speak":
        available = voices()
        voice_id = message.get("voice", "")
        text = message.get("text", "")
        if voice_id not in available:
            send({"id": request_id, "ok": False, "error": "Unknown voice"})
            return
        if not isinstance(text, str) or not text.strip():
            send({"id": request_id, "ok": False, "error": "No readable text supplied"})
            return
        stop_current()
        speed = max(0.65, min(1.55, float(message.get("speed", 1.0))))
        token = generation
        threading.Thread(target=synthesize, args=(request_id, voice_id, text[:100000], speed, token), daemon=True).start()
        send({"id": request_id, "ok": True, "status": "generating"})
        return
    send({"id": request_id, "ok": False, "error": "Unsupported command"})


def main() -> None:
    while True:
        raw_length = sys.stdin.buffer.read(4)
        if len(raw_length) != 4:
            break
        length = struct.unpack("=I", raw_length)[0]
        if length > 64 * 1024 * 1024:
            break
        payload = sys.stdin.buffer.read(length)
        try:
            message = json.loads(payload.decode("utf-8"))
            if isinstance(message, dict):
                handle(message)
        except Exception as error:
            send({"ok": False, "error": str(error)})
    stop_current()


if __name__ == "__main__":
    main()
