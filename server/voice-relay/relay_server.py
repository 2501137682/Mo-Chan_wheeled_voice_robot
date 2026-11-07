"""PC relay for the ESP32 voice-assistant test.

DeepSeek handles the conversation. STT and TTS are OpenAI-compatible endpoints
because DeepSeek's chat API does not accept microphone audio or return speech.
The ESP32 sends a WAV file and receives a small binary header followed by WAV audio.
"""

import json
import os
import struct
import urllib.error
import urllib.request
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


HOST = "0.0.0.0"
PORT = 8765
DEFAULT_CHAT_URL = "https://api.deepseek.com/chat/completions"
STT_URL = os.getenv("STT_API_URL", "https://api.openai.com/v1/audio/transcriptions")
TTS_URL = os.getenv("TTS_API_URL", "https://api.openai.com/v1/audio/speech")
CHAT_URL = os.getenv("CHAT_API_URL", DEFAULT_CHAT_URL)
CHAT_MODEL = os.getenv("CHAT_MODEL", os.getenv("DEEPSEEK_MODEL", "deepseek-chat"))
STT_MODEL = os.getenv("STT_MODEL", "whisper-1")
TTS_MODEL = os.getenv("TTS_MODEL", "gpt-4o-mini-tts")
TTS_VOICE = os.getenv("TTS_VOICE", "alloy")

conversation = []


def require_env(name):
    value = os.getenv(name)
    if not value:
        raise RuntimeError(f"Missing environment variable: {name}")
    return value


def first_env(*names):
    for name in names:
        value = os.getenv(name)
        if value:
            return value
    raise RuntimeError(f"Set one of these environment variables: {', '.join(names)}")


def request_json(url, api_key, payload):
    request = urllib.request.Request(
        url,
        data=json.dumps(payload).encode("utf-8"),
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
        },
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=90) as response:
        return json.loads(response.read().decode("utf-8"))


def transcribe(wav_bytes):
    api_key = first_env("STT_API_KEY", "OPENAI_API_KEY")
    boundary = uuid.uuid4().hex
    body = bytearray()

    def field(name, value):
        body.extend(f"--{boundary}\r\n".encode())
        body.extend(f'Content-Disposition: form-data; name="{name}"\r\n\r\n'.encode())
        body.extend(value.encode())
        body.extend(b"\r\n")

    field("model", STT_MODEL)
    body.extend(f"--{boundary}\r\n".encode())
    body.extend(b'Content-Disposition: form-data; name="file"; filename="question.wav"\r\n')
    body.extend(b"Content-Type: audio/wav\r\n\r\n")
    body.extend(wav_bytes)
    body.extend(f"\r\n--{boundary}--\r\n".encode())

    request = urllib.request.Request(
        STT_URL,
        data=bytes(body),
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": f"multipart/form-data; boundary={boundary}",
        },
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=90) as response:
        result = json.loads(response.read().decode("utf-8"))
    return result["text"].strip()


def chat(text):
    global conversation
    conversation.append({"role": "user", "content": text})
    payload = {
        "model": CHAT_MODEL,
        "messages": [
            {"role": "system", "content": "你是一个简洁、友好的轮式小机器人。请用中文回答，最多三句话。"},
            *conversation[-10:],
        ],
        "temperature": 0.7,
        "max_tokens": 200,
    }
    result = request_json(CHAT_URL, first_env("CHAT_API_KEY", "DEEPSEEK_API_KEY", "OPENAI_API_KEY"), payload)
    answer = result["choices"][0]["message"]["content"].strip()
    conversation.append({"role": "assistant", "content": answer})
    return answer


def synthesize(text):
    payload = {
        "model": TTS_MODEL,
        "voice": TTS_VOICE,
        "input": text,
        "response_format": "wav",
    }
    request = urllib.request.Request(
        TTS_URL,
        data=json.dumps(payload).encode("utf-8"),
        headers={
            "Authorization": f"Bearer {first_env('TTS_API_KEY', 'OPENAI_API_KEY')}",
            "Content-Type": "application/json",
        },
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=90) as response:
        return response.read()


def make_packet(transcript, answer, audio):
    transcript_bytes = transcript.encode("utf-8")
    answer_bytes = answer.encode("utf-8")
    return (
        b"MOCH"
        + struct.pack("<I", len(transcript_bytes))
        + transcript_bytes
        + struct.pack("<I", len(answer_bytes))
        + answer_bytes
        + audio
    )


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path != "/ask":
            self.send_error(404)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            wav = self.rfile.read(length)
            transcript = transcribe(wav)
            print(f"You: {transcript}")
            answer = chat(transcript)
            print(f"DeepSeek: {answer}")
            audio = synthesize(answer)
            packet = make_packet(transcript, answer, audio)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(packet)))
            self.end_headers()
            self.wfile.write(packet)
        except Exception as exc:
            print(f"Request failed: {exc}")
            self.send_error(500, str(exc))

    def log_message(self, format_string, *args):
        print(format_string % args)


if __name__ == "__main__":
    # Keep this filename as the documented entry point; the Volcengine relay
    # lives separately so the previous OpenAI-compatible version remains easy
    # to compare or restore.
    from relay_volc import serve

    serve()
