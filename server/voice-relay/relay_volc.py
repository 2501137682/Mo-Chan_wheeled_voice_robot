"""Volcengine ASR/TTS relay used by relay_server.py."""

import asyncio
import gzip
import io
import json
import os
import struct
import threading
import uuid
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib import error as urllib_error
from urllib import request as urllib_request

import websockets

HOST = "0.0.0.0"
PORT = int(os.getenv("RELAY_PORT", "8765"))
ASR_URL = os.getenv("VOLC_ASR_WS_URL", "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async")
ASR_RESOURCE_ID = os.getenv("VOLC_ASR_RESOURCE_ID", "volc.seedasr.sauc.duration")
TTS_URL = os.getenv("VOLC_TTS_WS_URL", "wss://openspeech.bytedance.com/api/v3/tts/bidirection")
TTS_RESOURCE_ID = os.getenv("VOLC_TTS_RESOURCE_ID", "seed-tts-2.0")
TTS_SPEAKER = os.getenv("VOLC_TTS_SPEAKER", "zh_female_shuangkuaisisi_moon_bigtts")
TTS_SAMPLE_RATE = int(os.getenv("VOLC_TTS_SAMPLE_RATE", "24000"))
DEEPSEEK_URL = os.getenv("DEEPSEEK_API_URL", "https://api.deepseek.com/chat/completions")
DEEPSEEK_MODEL = os.getenv("DEEPSEEK_MODEL", "deepseek-chat")

conversation = []
conversation_lock = threading.Lock()


def require_env(name):
    value = os.getenv(name)
    if not value:
        raise RuntimeError(f"Missing environment variable: {name}")
    return value


def deepseek_chat(text):
    api_key = require_env("DEEPSEEK_API_KEY")
    with conversation_lock:
        conversation.append({"role": "user", "content": text})
        messages = [{
            "role": "system",
            "content": "你的名字是柠檬。你是一个简洁、友好的聊天机器人，请用中文回答，最多三句话。",
        }, *conversation[-10:]]
    request = urllib_request.Request(
        DEEPSEEK_URL,
        data=json.dumps({
            "model": DEEPSEEK_MODEL,
            "messages": messages,
            "temperature": 0.7,
            "max_tokens": 200,
        }).encode("utf-8"),
        headers={"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib_request.urlopen(request, timeout=90) as response:
            result = json.loads(response.read().decode("utf-8"))
    except urllib_error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")
        raise RuntimeError(f"DeepSeek HTTP {exc.code}: {detail}") from exc
    try:
        answer = result["choices"][0]["message"]["content"].strip()
    except (KeyError, IndexError, TypeError) as exc:
        raise RuntimeError(f"Unexpected DeepSeek response: {result}") from exc
    with conversation_lock:
        conversation.append({"role": "assistant", "content": answer})
    return answer


# Volcengine ASR V3 binary protocol.
def asr_frame(message_type, flags, serialization, compression, payload):
    header = bytes((0x11, (message_type << 4) | flags, (serialization << 4) | compression, 0))
    return header + struct.pack(">I", len(payload)) + payload


def parse_asr_response(raw):
    if not isinstance(raw, bytes) or len(raw) < 8:
        raise RuntimeError("Volcengine ASR returned an invalid frame")
    header_size = (raw[0] & 0x0F) * 4
    message_type = raw[1] >> 4
    flags = raw[1] & 0x0F
    compression = raw[2] & 0x0F
    offset = header_size
    if message_type == 0x0F:
        if len(raw) < offset + 8:
            raise RuntimeError("Volcengine ASR returned a truncated error frame")
        code = struct.unpack_from(">I", raw, offset)[0]
        size = struct.unpack_from(">I", raw, offset + 4)[0]
        detail = raw[offset + 8:offset + 8 + size].decode("utf-8", "replace")
        raise RuntimeError(f"Volcengine ASR error {code}: {detail}")
    if message_type != 0x09:
        return False, {}
    if flags in (0x01, 0x03):
        offset += 4
    if len(raw) < offset + 4:
        raise RuntimeError("Volcengine ASR response has no payload size")
    size = struct.unpack_from(">I", raw, offset)[0]
    payload = raw[offset + 4:offset + 4 + size]
    if compression == 0x01:
        payload = gzip.decompress(payload)
    data = json.loads(payload.decode("utf-8"))
    return flags in (0x02, 0x03) or bool(data.get("is_last_package")), data


def extract_pcm(wav_bytes):
    try:
        with wave.open(io.BytesIO(wav_bytes), "rb") as wav_file:
            if (wav_file.getnchannels(), wav_file.getsampwidth(), wav_file.getframerate()) != (1, 2, 16000):
                raise RuntimeError("ESP32 WAV must be 16-bit mono, 16 kHz")
            return wav_file.readframes(wav_file.getnframes())
    except wave.Error as exc:
        raise RuntimeError(f"Invalid WAV from ESP32: {exc}") from exc


async def volc_asr(wav_bytes):
    api_key = require_env("VOLC_ASR_API_KEY")
    request_id = str(uuid.uuid4())
    headers = {
        "X-Api-Key": api_key,
        "X-Api-Resource-Id": ASR_RESOURCE_ID,
        "X-Api-Request-Id": request_id,
        "X-Api-Connect-Id": request_id,
        "X-Api-Sequence": "-1",
    }
    params = {
        "user": {"uid": "mo-chan"},
        "audio": {"format": "pcm", "codec": "raw", "rate": 16000, "bits": 16, "channel": 1},
        "request": {
            "model_name": "bigmodel",
            "enable_nonstream": True,
            "enable_itn": True,
            "enable_punc": True,
            "enable_ddc": True,
            "show_utterances": True,
            "result_type": "single",
            "end_window_size": 800,
            "force_to_speech_time": 1000,
        },
    }
    pcm = extract_pcm(wav_bytes)
    async with websockets.connect(ASR_URL, additional_headers=headers, max_size=16 * 1024 * 1024) as ws:
        payload = gzip.compress(json.dumps(params, ensure_ascii=False).encode("utf-8"))
        await ws.send(asr_frame(0x01, 0x00, 0x01, 0x01, payload))
        chunk_size = 16000 * 2 * 200 // 1000
        for start in range(0, len(pcm), chunk_size):
            chunk = gzip.compress(pcm[start:start + chunk_size])
            await ws.send(asr_frame(0x02, 0x00, 0x00, 0x01, chunk))
        await ws.send(asr_frame(0x02, 0x02, 0x00, 0x01, gzip.compress(b"")))
        text = ""
        while True:
            raw = await asyncio.wait_for(ws.recv(), 15)
            final, data = parse_asr_response(raw)
            result = data.get("result", {})
            if isinstance(result, list):
                result = result[0] if result else {}
            if result.get("text"):
                text = result["text"].strip()
            if final:
                break
    if not text:
        raise RuntimeError("Volcengine ASR returned empty text")
    return text


# Volcengine TTS V3 bidirectional WebSocket protocol.
def tts_packet(event, payload, session_id=None):
    header = bytes((0x11, 0x14, 0x10, 0x00))
    optional = struct.pack(">i", event)
    if session_id is not None:
        encoded = session_id.encode("utf-8")
        optional += struct.pack(">I", len(encoded)) + encoded
    return header + optional + struct.pack(">I", len(payload)) + payload


def tts_json(event, payload, session_id=None):
    return tts_packet(event, json.dumps(payload, ensure_ascii=False).encode("utf-8"), session_id)


def read_sized(raw, offset):
    if len(raw) < offset + 4:
        raise RuntimeError("Volcengine TTS returned a truncated frame")
    size = struct.unpack_from(">I", raw, offset)[0]
    offset += 4
    if len(raw) < offset + size:
        raise RuntimeError("Volcengine TTS payload is truncated")
    return raw[offset:offset + size], offset + size


def parse_tts_response(raw):
    if not isinstance(raw, bytes) or len(raw) < 4:
        raise RuntimeError("Volcengine TTS returned an invalid frame")
    message_type = raw[1] >> 4
    if message_type == 0x0F:
        code = struct.unpack_from(">i", raw, 4)[0]
        detail, _ = read_sized(raw, 8)
        raise RuntimeError(f"Volcengine TTS error {code}: {detail.decode('utf-8', 'replace')}")
    offset = (raw[0] & 0x0F) * 4
    if (raw[1] & 0x0F) != 0x04:
        return None, b""
    event = struct.unpack_from(">i", raw, offset)[0]
    offset += 4
    if event == 50:
        connection_id, offset = read_sized(raw, offset)
        payload, _ = read_sized(raw, offset)
        return event, payload
    if event in (51, 52):
        payload, _ = read_sized(raw, offset)
        return event, payload
    _, offset = read_sized(raw, offset)  # session id
    payload, _ = read_sized(raw, offset)
    return event, payload


def tts_payload(text, event):
    return {
        "user": {"uid": "mo-chan"},
        "event": event,
        "namespace": "BidirectionalTTS",
        "req_params": {
            "text": text,
            "speaker": TTS_SPEAKER,
            "audio_params": {"format": "pcm", "sample_rate": TTS_SAMPLE_RATE},
        },
    }


async def volc_tts(text):
    api_key = require_env("VOLC_TTS_API_KEY")
    session_id = uuid.uuid4().hex
    headers = {
        "X-Api-Key": api_key,
        "X-Api-Resource-Id": TTS_RESOURCE_ID,
        "X-Api-Connect-Id": str(uuid.uuid4()),
    }
    async with websockets.connect(TTS_URL, additional_headers=headers, max_size=16 * 1024 * 1024) as ws:
        await ws.send(tts_json(1, {}))
        event, _ = parse_tts_response(await asyncio.wait_for(ws.recv(), 15))
        if event != 50:
            raise RuntimeError(f"Volcengine TTS connection failed, event={event}")
        await ws.send(tts_json(100, tts_payload("", 100), session_id))
        event, _ = parse_tts_response(await asyncio.wait_for(ws.recv(), 15))
        if event != 150:
            raise RuntimeError(f"Volcengine TTS session failed, event={event}")
        await ws.send(tts_json(200, tts_payload(text, 200), session_id))
        await ws.send(tts_json(102, {}, session_id))
        audio = bytearray()
        while True:
            event, payload = parse_tts_response(await asyncio.wait_for(ws.recv(), 30))
            if event == 352:
                audio.extend(payload)
            elif event == 153:
                raise RuntimeError("Volcengine TTS session failed")
            elif event == 152:
                break
        try:
            await ws.send(tts_json(2, {}))
        except Exception:
            pass
    if not audio:
        raise RuntimeError("Volcengine TTS returned no PCM audio")
    return make_wav(bytes(audio), TTS_SAMPLE_RATE)


def make_wav(pcm_bytes, sample_rate):
    output = io.BytesIO()
    with wave.open(output, "wb") as wav_file:
        wav_file.setnchannels(1)
        wav_file.setsampwidth(2)
        wav_file.setframerate(sample_rate)
        wav_file.writeframes(pcm_bytes)
    return output.getvalue()


def make_packet(transcript, answer, audio):
    transcript_bytes = transcript.encode("utf-8")
    answer_bytes = answer.encode("utf-8")
    return b"MOCH" + struct.pack("<I", len(transcript_bytes)) + transcript_bytes + struct.pack("<I", len(answer_bytes)) + answer_bytes + audio


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path != "/ask":
            self.send_error(404)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            wav = self.rfile.read(length)
            transcript = asyncio.run(volc_asr(wav))
            print(f"You: {transcript}")
            answer = deepseek_chat(transcript)
            print(f"DeepSeek: {answer}")
            packet = make_packet(transcript, answer, asyncio.run(volc_tts(answer)))
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


def serve():
    print(f"Relay listening on http://0.0.0.0:{PORT}/ask")
    print(f"ASR: {ASR_URL} / {ASR_RESOURCE_ID}")
    print(f"TTS: {TTS_URL} / {TTS_RESOURCE_ID} / {TTS_SPEAKER}")
    ThreadingHTTPServer((HOST, PORT), Handler).serve_forever()
