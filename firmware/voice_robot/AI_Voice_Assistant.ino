#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <FluxGarage_RoboEyes.h>
#include <math.h>
#include "driver/i2s.h"

// Fill these in before uploading. The PC relay and ESP32 must be on the same Wi-Fi.
const char *WIFI_SSID = "YOUR_WIFI_SSID";
const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char *RELAY_URL = "http://YOUR_PC_IP:8765/ask";

// Wiring used by this sketch.
constexpr int OLED_SDA = 8;
constexpr int OLED_SCL = 9;
constexpr int MIC_BCLK = 4;
constexpr int MIC_LRCLK = 5;
constexpr int MIC_DATA = 6;
// Keep the amplifier on a separate I2S clock/data group, as in the reference project.
constexpr int AMP_BCLK = 16;
constexpr int AMP_LRCLK = 17;
constexpr int AMP_DATA = 18;
// MAX98357A SD/EN: LOW mutes/shuts down the amplifier, HIGH enables it.
constexpr int AMP_ENABLE = 7;
// TC1508 board: IN1/IN2 control motor A, IN3/IN4 control motor B.
constexpr int MOTOR_A_IN1 = 10;
constexpr int MOTOR_A_IN2 = 11;
constexpr int MOTOR_B_IN1 = 12;
constexpr int MOTOR_B_IN2 = 13;
constexpr bool MOTOR_A_INVERT = false;
constexpr bool MOTOR_B_INVERT = false;
constexpr i2s_port_t I2S_PORT = I2S_NUM_0;

constexpr uint32_t SAMPLE_RATE = 16000;
constexpr uint32_t FIXED_RECORD_SECONDS = 4;
constexpr uint32_t MAX_RECORD_SECONDS = 15;
constexpr size_t FIXED_RECORD_BYTES = SAMPLE_RATE * FIXED_RECORD_SECONDS * sizeof(int16_t);
constexpr size_t MAX_RECORD_BYTES = SAMPLE_RATE * MAX_RECORD_SECONDS * sizeof(int16_t);
constexpr size_t MAX_WAV_BYTES = 44 + MAX_RECORD_BYTES;
constexpr uint32_t MIN_HOLD_RECORD_MS = 300;
constexpr size_t MAX_PACKET_BYTES = 900000;

Adafruit_SSD1306 display(128, 64, &Wire, -1);
RoboEyes<Adafruit_SSD1306> roboEyes(display);
WebServer webServer(80);

unsigned long lastMotorCommand = 0;
constexpr unsigned long MOTOR_COMMAND_TIMEOUT_MS = 600;
bool voiceRequested = false;
bool voiceBusy = false;
bool voiceHoldMode = false;
volatile bool voiceStopRequested = false;
volatile bool voiceRecording = false;

enum FaceState {
  FACE_IDLE,
  FACE_LISTENING,
  FACE_THINKING,
  FACE_SPEAKING,
  FACE_ERROR
};

FaceState currentFace = FACE_ERROR;
bool roboEyesReady = false;

void setFace(FaceState state) {
  if (!roboEyesReady || currentFace == state) return;
  currentFace = state;

  // Reset temporary effects before applying the new state.
  roboEyes.setMood(DEFAULT);
  roboEyes.setPosition(DEFAULT);
  roboEyes.setAutoblinker(OFF);
  roboEyes.setIdleMode(OFF);
  roboEyes.setCuriosity(OFF);
  roboEyes.setSweat(OFF);
  roboEyes.setHFlicker(OFF, 0);
  roboEyes.setVFlicker(OFF, 0);

  switch (state) {
    case FACE_IDLE:
      roboEyes.setMood(DEFAULT);
      roboEyes.setAutoblinker(ON, 3, 2);
      roboEyes.setIdleMode(ON, 2, 2);
      break;
    case FACE_LISTENING:
      roboEyes.setMood(DEFAULT);
      roboEyes.setPosition(N);
      roboEyes.setCuriosity(ON);
      roboEyes.setAutoblinker(ON, 2, 1);
      roboEyes.setVFlicker(ON, 1);
      break;
    case FACE_THINKING:
      roboEyes.setMood(TIRED);
      roboEyes.setPosition(E);
      roboEyes.setAutoblinker(ON, 1, 1);
      roboEyes.setIdleMode(ON, 1, 1);
      break;
    case FACE_SPEAKING:
      roboEyes.setMood(HAPPY);
      roboEyes.setAutoblinker(ON, 1, 1);
      roboEyes.setIdleMode(ON, 1, 1);
      roboEyes.setVFlicker(ON, 2);
      break;
    case FACE_ERROR:
      roboEyes.setMood(ANGRY);
      roboEyes.setPosition(S);
      roboEyes.setSweat(ON);
      roboEyes.setAutoblinker(ON, 1, 0);
      break;
  }
}

void serviceEyes() {
  if (roboEyesReady) roboEyes.update();
}

void setAmplifierEnabled(bool enabled) {
  digitalWrite(AMP_ENABLE, enabled ? HIGH : LOW);
}

void setMotor(int in1, int in2, int speed, bool invert) {
  if (invert) speed = -speed;
  speed = constrain(speed, -255, 255);

  if (speed > 0) {
    analogWrite(in1, speed);
    analogWrite(in2, 0);
  } else if (speed < 0) {
    analogWrite(in1, 0);
    analogWrite(in2, -speed);
  } else {
    analogWrite(in1, 0);
    analogWrite(in2, 0);
  }
}

void stopMotors() {
  setMotor(MOTOR_A_IN1, MOTOR_A_IN2, 0, MOTOR_A_INVERT);
  setMotor(MOTOR_B_IN1, MOTOR_B_IN2, 0, MOTOR_B_INVERT);
  lastMotorCommand = millis();
}

void driveMotors(int leftSpeed, int rightSpeed) {
  setMotor(MOTOR_A_IN1, MOTOR_A_IN2, leftSpeed, MOTOR_A_INVERT);
  setMotor(MOTOR_B_IN1, MOTOR_B_IN2, rightSpeed, MOTOR_B_INVERT);
  lastMotorCommand = millis();
}

const char MOTOR_PAGE[] PROGMEM = R"rawliteral(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Mo-Chan</title><style>
body{font-family:system-ui,sans-serif;text-align:center;background:#10141c;color:#f5f7fb;margin:0;padding:18px}
h1{font-size:24px;margin:8px}.speed{width:240px}.pad{display:grid;grid-template-columns:repeat(3,88px);gap:12px;justify-content:center;margin:22px auto}
button{height:64px;border:0;border-radius:14px;background:#2b78ff;color:white;font-size:18px;font-weight:600;touch-action:none}
button.stop{background:#e04b4b}button.turn{background:#516074}button.talk{background:#19a974;width:280px;margin:8px auto}button:disabled{opacity:.55}.empty{visibility:hidden}
</style></head><body><h1>Mo-Chan</h1>
<label>Speed <input class="speed" id="speed" type="range" min="80" max="255" value="180"></label>
<div class="pad"><button class="empty">.</button><button id="forward">Forward</button><button class="empty">.</button>
<button id="left" class="turn">Left</button><button id="stop" class="stop">Stop</button><button id="right" class="turn">Right</button>
<button class="empty">.</button><button id="back">Backward</button><button class="empty">.</button></div>
<div><button id="spinL" class="turn">Spin L</button> <button id="spinR" class="turn">Spin R</button></div>
<button id="talk" class="talk">Voice Chat</button>
<script>
const speed=()=>Number(document.getElementById('speed').value), active={};
function move(l,r){fetch('/api/move?l='+l+'&r='+r).catch(()=>{});}
function stop(){fetch('/api/stop').catch(()=>{});}
function bind(id,fn){const b=document.getElementById(id);let timer;
 const start=e=>{e.preventDefault();fn();timer=setInterval(fn,150)};
 const end=e=>{e.preventDefault();clearInterval(timer);stop()};
 b.addEventListener('pointerdown',start);b.addEventListener('pointerup',end);b.addEventListener('pointercancel',end);b.addEventListener('pointerleave',end);
}
bind('forward',()=>move(speed(),speed()));bind('back',()=>move(-speed(),-speed()));
bind('left',()=>move(Math.round(speed()*.35),speed()));bind('right',()=>move(speed(),Math.round(speed()*.35)));
bind('spinL',()=>move(-speed(),speed()));bind('spinR',()=>move(speed(),-speed()));
document.getElementById('stop').addEventListener('click',stop);window.addEventListener('blur',stop);
const talkButton=document.getElementById('talk');
let talkPressed=false;
function resetTalkButton(){talkButton.disabled=false;talkButton.textContent='Voice Chat';}
async function waitForVoiceDone(){
  const timer=setInterval(async()=>{
    try{
      const status=await fetch('/api/status').then(r=>r.json());
      if(!status.busy && !status.pending){clearInterval(timer);resetTalkButton();}
    }catch(e){}
  },1000);
}
async function finishTalk(e){
  if(e)e.preventDefault();
  if(!talkPressed)return;
  talkPressed=false;
  talkButton.disabled=true;
  talkButton.textContent='Processing...';
  try{await fetch('/api/talk/stop');}catch(e){}
  waitForVoiceDone();
}
talkButton.addEventListener('pointerdown',async e=>{
  e.preventDefault();
  if(talkButton.disabled || talkPressed)return;
  talkPressed=true;
  if(talkButton.setPointerCapture)talkButton.setPointerCapture(e.pointerId);
  stop();talkButton.textContent='Release to finish';
  try{
    const response=await fetch('/api/talk');
    if(!response.ok)throw new Error('voice start failed');
  }catch(e){talkPressed=false;resetTalkButton();}
});
talkButton.addEventListener('pointerup',finishTalk);
talkButton.addEventListener('pointercancel',finishTalk);
window.addEventListener('blur',()=>finishTalk());
</script></body></html>
)rawliteral";

void processVoice(bool holdToTalk);

void handleMotorPage() {
  webServer.send_P(200, "text/html", MOTOR_PAGE);
}

void handleMotorMove() {
  if (!webServer.hasArg("l") || !webServer.hasArg("r")) {
    webServer.send(400, "application/json", "{\"error\":\"missing l/r\"}");
    return;
  }
  int leftSpeed = constrain(webServer.arg("l").toInt(), -255, 255);
  int rightSpeed = constrain(webServer.arg("r").toInt(), -255, 255);
  driveMotors(leftSpeed, rightSpeed);
  webServer.send(200, "application/json", "{\"ok\":true}");
}

void handleMotorStop() {
  stopMotors();
  webServer.send(200, "application/json", "{\"ok\":true}");
}

void handleVoiceTalk() {
  if (voiceBusy || voiceRequested) {
    webServer.send(409, "application/json", "{\"ok\":false,\"error\":\"voice busy\"}");
    return;
  }
  voiceRequested = true;
  voiceHoldMode = true;
  voiceStopRequested = false;
  webServer.send(202, "application/json", "{\"ok\":true,\"pending\":true}");
}

void handleVoiceStop() {
  if (!voiceBusy && !voiceRequested) {
    webServer.send(409, "application/json", "{\"ok\":false,\"error\":\"no voice request\"}");
    return;
  }
  voiceStopRequested = true;
  webServer.send(200, "application/json", "{\"ok\":true,\"stopping\":true}");
}

void handleVoiceStatus() {
  String response = String("{\"busy\":") + (voiceBusy ? "true" : "false") +
                    ",\"pending\":" + (voiceRequested ? "true" : "false") +
                    ",\"recording\":" + (voiceRecording ? "true" : "false") + "}";
  webServer.send(200, "application/json", response);
}

void startWebServer() {
  pinMode(MOTOR_A_IN1, OUTPUT);
  pinMode(MOTOR_A_IN2, OUTPUT);
  pinMode(MOTOR_B_IN1, OUTPUT);
  pinMode(MOTOR_B_IN2, OUTPUT);
  stopMotors();
  webServer.on("/", HTTP_GET, handleMotorPage);
  webServer.on("/api/move", HTTP_GET, handleMotorMove);
  webServer.on("/api/stop", HTTP_GET, handleMotorStop);
  webServer.on("/api/talk", HTTP_GET, handleVoiceTalk);
  webServer.on("/api/talk/stop", HTTP_GET, handleVoiceStop);
  webServer.on("/api/status", HTTP_GET, handleVoiceStatus);
  webServer.onNotFound([]() { webServer.send(404, "text/plain", "Not found"); });
  webServer.begin();
  Serial.print("Motor control: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/");
}

void serviceBackground() {
  webServer.handleClient();
  if (millis() - lastMotorCommand > MOTOR_COMMAND_TIMEOUT_MS) {
    stopMotors();
  }
  serviceEyes();
}

void showStatus(const char *line1, const char *line2 = "") {
  (void)line2;
  if (strcmp(line1, "Listening...") == 0) {
    setFace(FACE_LISTENING);
  } else if (strcmp(line1, "Thinking...") == 0 ||
             strcmp(line1, "WiFi connecting...") == 0) {
    setFace(FACE_THINKING);
  } else if (strcmp(line1, "Speaking...") == 0) {
    setFace(FACE_SPEAKING);
  } else if (strcmp(line1, "Record failed") == 0 ||
             strcmp(line1, "Request failed") == 0 ||
             strcmp(line1, "WiFi failed") == 0) {
    setFace(FACE_ERROR);
  } else {
    setFace(FACE_IDLE);
  }
}

void writeLe16(uint8_t *p, uint16_t value) {
  p[0] = value & 0xff;
  p[1] = (value >> 8) & 0xff;
}

void writeLe32(uint8_t *p, uint32_t value) {
  p[0] = value & 0xff;
  p[1] = (value >> 8) & 0xff;
  p[2] = (value >> 16) & 0xff;
  p[3] = (value >> 24) & 0xff;
}

uint16_t readLe16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readLe32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

void makeWavHeader(uint8_t *wav, uint32_t dataBytes) {
  memcpy(wav, "RIFF", 4);
  writeLe32(wav + 4, 36 + dataBytes);
  memcpy(wav + 8, "WAVEfmt ", 8);
  writeLe32(wav + 16, 16);
  writeLe16(wav + 20, 1);             // PCM
  writeLe16(wav + 22, 1);             // mono
  writeLe32(wav + 24, SAMPLE_RATE);
  writeLe32(wav + 28, SAMPLE_RATE * 2);
  writeLe16(wav + 32, 2);
  writeLe16(wav + 34, 16);
  memcpy(wav + 36, "data", 4);
  writeLe32(wav + 40, dataBytes);
}

bool installI2sRx() {
  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = SAMPLE_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  config.communication_format = I2S_COMM_FORMAT_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 8;
  config.dma_buf_len = 256;
  config.use_apll = false;
  config.tx_desc_auto_clear = false;
  config.fixed_mclk = 0;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = MIC_BCLK;
  pins.ws_io_num = MIC_LRCLK;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = MIC_DATA;

  if (i2s_driver_install(I2S_PORT, &config, 0, nullptr) != ESP_OK) return false;
  if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) {
    i2s_driver_uninstall(I2S_PORT);
    return false;
  }
  i2s_zero_dma_buffer(I2S_PORT);
  return true;
}

size_t recordWav(uint8_t *wav, size_t maxDataBytes, bool stopOnRequest) {
  setAmplifierEnabled(false);
  delay(30);
  makeWavHeader(wav, maxDataBytes);
  if (!installI2sRx()) return 0;

  size_t written = 0;
  uint32_t sampleCount = 0;
  int32_t peak = 0;
  uint64_t sumAbs = 0;
  const uint32_t startedAt = millis();
  voiceRecording = true;
  int32_t raw[256];
  while (written < maxDataBytes) {
    size_t bytesRead = 0;
    if (i2s_read(I2S_PORT, raw, sizeof(raw), &bytesRead, portMAX_DELAY) != ESP_OK) {
      voiceRecording = false;
      i2s_driver_uninstall(I2S_PORT);
      return 0;
    }
    size_t samples = bytesRead / sizeof(int32_t);
    for (size_t i = 0; i < samples && written < maxDataBytes; ++i) {
      int32_t value = raw[i] >> 14;
      value = constrain(value, -32768, 32767);
      int32_t magnitude = value < 0 ? -value : value;
      if (magnitude > peak) peak = magnitude;
      sumAbs += static_cast<uint32_t>(magnitude);
      ++sampleCount;
      int16_t sample = static_cast<int16_t>(value);
      wav[44 + written++] = sample & 0xff;
      wav[44 + written++] = (sample >> 8) & 0xff;
    }
    serviceBackground();
    if (stopOnRequest && voiceStopRequested && millis() - startedAt >= MIN_HOLD_RECORD_MS) {
      break;
    }
  }
  voiceRecording = false;
  voiceStopRequested = false;
  makeWavHeader(wav, written);
  i2s_driver_uninstall(I2S_PORT);
  uint32_t average = sampleCount == 0 ? 0 : static_cast<uint32_t>(sumAbs / sampleCount);
  Serial.printf("Mic level: peak=%ld, average=%lu, samples=%lu\n",
                static_cast<long>(peak),
                static_cast<unsigned long>(average),
                static_cast<unsigned long>(sampleCount));
  return written;
}

bool installI2sTx(uint32_t sampleRate) {
  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  config.sample_rate = sampleRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  config.communication_format = I2S_COMM_FORMAT_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 8;
  config.dma_buf_len = 256;
  config.use_apll = false;
  config.tx_desc_auto_clear = true;
  config.fixed_mclk = 0;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = AMP_BCLK;
  pins.ws_io_num = AMP_LRCLK;
  pins.data_out_num = AMP_DATA;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  if (i2s_driver_install(I2S_PORT, &config, 0, nullptr) != ESP_OK) return false;
  if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) {
    i2s_driver_uninstall(I2S_PORT);
    return false;
  }
  i2s_zero_dma_buffer(I2S_PORT);
  return true;
}

bool findWavData(const uint8_t *wav, size_t length, uint32_t &sampleRate,
                size_t &dataOffset, size_t &dataLength) {
  if (length < 44 || memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0) return false;
  sampleRate = readLe32(wav + 24);
  size_t offset = 12;
  while (offset + 8 <= length) {
    uint32_t chunkLength = readLe32(wav + offset + 4);
    if (memcmp(wav + offset, "data", 4) == 0) {
      dataOffset = offset + 8;
      dataLength = min<size_t>(chunkLength, length - dataOffset);
      return true;
    }
    offset += 8 + chunkLength + (chunkLength & 1);
  }
  return false;
}

bool playWav(const uint8_t *wav, size_t length) {
  uint32_t sampleRate = 0;
  size_t dataOffset = 0;
  size_t dataLength = 0;
  if (!findWavData(wav, length, sampleRate, dataOffset, dataLength)) {
    Serial.println("Playback error: invalid WAV");
    return false;
  }
  Serial.printf("Playback WAV: rate=%lu, pcm_bytes=%u\n",
                static_cast<unsigned long>(sampleRate),
                static_cast<unsigned int>(dataLength));
  int16_t peak = 0;
  for (size_t i = dataOffset; i + 1 < dataOffset + dataLength; i += 2) {
    int16_t sample = static_cast<int16_t>(
        static_cast<uint16_t>(wav[i]) | (static_cast<uint16_t>(wav[i + 1]) << 8));
    int16_t magnitude = sample < 0 ? static_cast<int16_t>(-sample) : sample;
    if (magnitude > peak) peak = magnitude;
  }
  Serial.printf("PCM peak=%d\n", peak);
  if (!installI2sTx(sampleRate)) {
    Serial.println("Playback error: I2S TX install failed");
    return false;
  }
  setAmplifierEnabled(true);
  delay(20);

  size_t sent = 0;
  while (sent < dataLength) {
    size_t written = 0;
    size_t chunk = min<size_t>(4096, dataLength - sent);
    if (i2s_write(I2S_PORT, wav + dataOffset + sent, chunk, &written, portMAX_DELAY) != ESP_OK) {
      Serial.println("Playback error: I2S write failed");
      setAmplifierEnabled(false);
      i2s_driver_uninstall(I2S_PORT);
      return false;
    }
    sent += written;
    serviceBackground();
  }
  i2s_zero_dma_buffer(I2S_PORT);
  setAmplifierEnabled(false);
  delay(20);
  i2s_driver_uninstall(I2S_PORT);
  Serial.println("Playback finished");
  return true;
}

void playTestTone() {
  constexpr uint32_t TEST_RATE = 16000;
  constexpr uint32_t TEST_SECONDS = 2;
  constexpr uint32_t TEST_FREQUENCY = 1000;
  constexpr int16_t TEST_AMPLITUDE = 6000;

  if (!installI2sTx(TEST_RATE)) {
    Serial.println("Test tone error: I2S TX install failed");
    return;
  }
  setAmplifierEnabled(true);
  delay(20);
  Serial.println("Test tone: 1 kHz for 2 seconds");
  int16_t buffer[256];
  const uint32_t totalSamples = TEST_RATE * TEST_SECONDS;
  uint32_t generated = 0;
  while (generated < totalSamples) {
    size_t count = min<size_t>(256, totalSamples - generated);
    for (size_t i = 0; i < count; ++i) {
      float phase = 6.28318530718f * TEST_FREQUENCY * (generated + i) / TEST_RATE;
      buffer[i] = static_cast<int16_t>(TEST_AMPLITUDE * sinf(phase));
    }
    size_t written = 0;
    if (i2s_write(I2S_PORT, buffer, count * sizeof(int16_t), &written, portMAX_DELAY) != ESP_OK) {
      Serial.println("Test tone error: I2S write failed");
      break;
    }
    generated += written / sizeof(int16_t);
    serviceBackground();
  }
  i2s_zero_dma_buffer(I2S_PORT);
  setAmplifierEnabled(false);
  delay(20);
  i2s_driver_uninstall(I2S_PORT);
  Serial.println("Test tone finished");
}

bool connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  showStatus("WiFi connecting...");
  for (int attempt = 0; attempt < 40 && WiFi.status() != WL_CONNECTED; ++attempt) {
    serviceEyes();
    delay(500);
  }
  if (WiFi.status() != WL_CONNECTED) {
    showStatus("WiFi failed", "check settings");
    return false;
  }
  Serial.print("WiFi IP: ");
  Serial.println(WiFi.localIP());
  showStatus("Ready", "send r in Serial");
  return true;
}

bool askRelay(const uint8_t *wav, size_t wavLength) {
  HTTPClient http;
  http.setTimeout(90000);
  if (!http.begin(RELAY_URL)) return false;
  http.addHeader("Content-Type", "audio/wav");
  int code = http.POST(const_cast<uint8_t *>(wav), wavLength);
  if (code != HTTP_CODE_OK) {
    Serial.printf("Relay HTTP error: %d\n", code);
    http.end();
    return false;
  }

  uint8_t *packet = static_cast<uint8_t *>(ps_malloc(MAX_PACKET_BYTES));
  if (!packet) {
    http.end();
    return false;
  }
  WiFiClient *stream = http.getStreamPtr();
  int expected = http.getSize();
  size_t received = 0;
  uint32_t lastData = millis();
  while (http.connected() && (expected < 0 || received < static_cast<size_t>(expected))) {
    int available = stream->available();
    if (available > 0) {
      size_t toRead = min<size_t>(available, MAX_PACKET_BYTES - received);
      if (toRead == 0) break;
      size_t got = stream->readBytes(packet + received, toRead);
      received += got;
      lastData = millis();
    } else if (millis() - lastData > 10000) {
      break;
    } else {
      serviceBackground();
      delay(2);
    }
  }
  http.end();

  if (received < 12 || memcmp(packet, "MOCH", 4) != 0) {
    Serial.println("Invalid relay packet");
    free(packet);
    return false;
  }
  uint32_t transcriptLength = readLe32(packet + 4);
  size_t transcriptOffset = 8;
  size_t replyLengthOffset = transcriptOffset + transcriptLength;
  if (replyLengthOffset + 4 > received) {
    free(packet);
    return false;
  }
  uint32_t replyLength = readLe32(packet + replyLengthOffset);
  size_t audioOffset = replyLengthOffset + 4 + replyLength;
  if (audioOffset > received) {
    free(packet);
    return false;
  }
  Serial.print("You: ");
  Serial.write(packet + transcriptOffset, transcriptLength);
  Serial.println();
  Serial.print("DeepSeek: ");
  Serial.write(packet + replyLengthOffset + 4, replyLength);
  Serial.println();
  showStatus("Speaking...");
  bool played = playWav(packet + audioOffset, received - audioOffset);
  free(packet);
  return played;
}

void processVoice(bool holdToTalk) {
  stopMotors();
  if (WiFi.status() != WL_CONNECTED) {
    connectWifi();
    if (WiFi.status() != WL_CONNECTED) return;
  }
  uint8_t *wav = static_cast<uint8_t *>(ps_malloc(MAX_WAV_BYTES));
  if (!wav) {
    showStatus("PSRAM error");
    return;
  }
  showStatus("Listening...", holdToTalk ? "hold button" : "4 seconds");
  Serial.println(holdToTalk
                   ? "Speak now: hold the phone button, release to finish."
                   : "Speak now: recording for 4 seconds.");
  size_t wavLength = recordWav(
      wav,
      holdToTalk ? MAX_RECORD_BYTES : FIXED_RECORD_BYTES,
      holdToTalk);
  if (wavLength == 0) {
    showStatus("Record failed");
    free(wav);
    return;
  }
  showStatus("Thinking...");
  bool ok = askRelay(wav, wavLength + 44);
  free(wav);
  if (!ok) showStatus("Request failed", "check relay");
  else showStatus("Ready", "send r in Serial");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(AMP_ENABLE, OUTPUT);
  setAmplifierEnabled(false);
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found; continuing without display");
  } else {
    roboEyes.begin(128, 64, 60);
    roboEyesReady = true;
    setFace(FACE_IDLE);
    showStatus("Booting...");
  }
  Serial.printf("Free heap: %u, PSRAM: %u\n", ESP.getFreeHeap(), ESP.getFreePsram());
  if (connectWifi() && WiFi.status() == WL_CONNECTED) {
    startWebServer();
  }
  Serial.println("Type r + Enter to record one question, or t + Enter for a speaker test.");
  Serial.println("Serial input diagnostic is enabled.");
}

void loop() {
  while (Serial.available() > 0) {
    int incoming = Serial.read();
    if (incoming < 0) break;

    // Echo every received byte so a serial-port/input-mode problem is visible.
    Serial.printf("RX: 0x%02X", static_cast<unsigned int>(incoming));
    if (incoming >= 32 && incoming <= 126) {
      Serial.printf(" ('%c')", static_cast<char>(incoming));
    }
    Serial.println();

    if (incoming == 'r' || incoming == 'R') {
      if (voiceBusy || voiceRequested) {
        Serial.println("Voice command ignored: voice request already pending.");
      } else {
        Serial.println("Command accepted: starting recording.");
        voiceHoldMode = false;
        voiceStopRequested = false;
        voiceRequested = true;
      }
    } else if (incoming == 't' || incoming == 'T') {
      playTestTone();
    }
  }
  serviceBackground();
  if (voiceRequested && !voiceBusy) {
    const bool holdToTalk = voiceHoldMode;
    voiceRequested = false;
    voiceBusy = true;
    Serial.println("Starting voice conversation.");
    processVoice(holdToTalk);
    voiceBusy = false;
  }
  delay(10);
}
