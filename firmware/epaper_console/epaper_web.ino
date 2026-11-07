#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <GxEPD2_BW.h>

// E-paper wiring used by the working sketch.
#define EPD_SCK   12
#define EPD_MOSI  11
#define EPD_CS    10
#define EPD_DC     9
#define EPD_RST    8
#define EPD_BUSY   7

constexpr uint16_t EPD_WIDTH = 296;
constexpr uint16_t EPD_HEIGHT = 128;
constexpr size_t FRAME_BYTES = (EPD_WIDTH * EPD_HEIGHT) / 8;

// The ESP32 creates its own Wi-Fi network.
const char *AP_SSID = "EPD-ESP32";
const char *AP_PASSWORD = "change-me-123";

GxEPD2_BW<GxEPD2_290_T5D, GxEPD2_290_T5D::HEIGHT> display(
  GxEPD2_290_T5D(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

WebServer server(80);
uint8_t frameBuffer[FRAME_BYTES];

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>电子纸控制台</title>
  <style>
    :root { color-scheme: light; font-family: system-ui, "Microsoft YaHei", sans-serif; }
    * { box-sizing: border-box; }
    body { margin: 0; background: #eef1f4; color: #17212b; }
    main { width: min(760px, 100%); margin: 0 auto; padding: 20px 14px 28px; }
    h1 { margin: 0 0 16px; font-size: 22px; font-weight: 650; }
    section { background: #fff; border: 1px solid #d8dee5; border-radius: 8px; padding: 16px; margin-top: 12px; }
    label { display: block; margin: 10px 0 6px; font-size: 14px; color: #44515d; }
    textarea, select, input[type=file], input[type=range] { width: 100%; }
    textarea { min-height: 100px; resize: vertical; border: 1px solid #bbc5cf; border-radius: 5px; padding: 9px; font: 16px system-ui, "Microsoft YaHei", sans-serif; }
    select, input[type=file] { border: 1px solid #bbc5cf; border-radius: 5px; padding: 8px; background: #fff; }
    .row { display: flex; gap: 12px; align-items: end; }
    .row > * { flex: 1; min-width: 0; }
    .modes { display: flex; gap: 18px; margin-bottom: 12px; }
    .modes label { margin: 0; color: #17212b; }
    canvas { display: block; width: min(100%, 592px); height: auto; image-rendering: pixelated; background: white; border: 1px solid #8793a0; margin: 0 auto; }
    button { border: 0; border-radius: 5px; padding: 10px 15px; font-size: 15px; cursor: pointer; }
    #send { background: #1769aa; color: white; }
    #clear { background: #e5e9ed; color: #17212b; }
    button:disabled { opacity: .55; cursor: wait; }
    .actions { display: flex; gap: 10px; margin-top: 14px; }
    #status { min-height: 20px; margin-top: 12px; color: #44515d; font-size: 14px; }
    .hint { color: #667582; font-size: 13px; margin: 7px 0 0; }
  </style>
</head>
<body>
<main>
  <h1>电子纸控制台</h1>
  <section>
    <div class="modes">
      <label><input type="radio" name="mode" value="text" checked> 文字</label>
      <label><input type="radio" name="mode" value="image"> 图片</label>
    </div>

    <div id="textPanel">
      <label for="text">显示内容</label>
      <textarea id="text" placeholder="输入中文或其他文字"></textarea>
      <div class="row">
        <div>
          <label for="fontSize">字号</label>
          <select id="fontSize">
            <option value="16">16 px</option>
            <option value="20" selected>20 px</option>
            <option value="24">24 px</option>
            <option value="28">28 px</option>
            <option value="32">32 px</option>
          </select>
        </div>
        <div>
          <label for="align">对齐</label>
          <select id="align">
            <option value="left" selected>左对齐</option>
            <option value="center">居中</option>
            <option value="right">右对齐</option>
          </select>
        </div>
      </div>
    </div>

    <div id="imagePanel" hidden>
      <label for="image">选择图片</label>
      <input id="image" type="file" accept="image/*">
      <p class="hint">图片会自动缩放到电子纸的 296×128 像素范围内。</p>
    </div>

    <label for="threshold">黑白阈值：<span id="thresholdValue">160</span></label>
    <input id="threshold" type="range" min="1" max="254" value="160">
    <canvas id="preview" width="296" height="128"></canvas>

    <div class="actions">
      <button id="send">发送到电子纸</button>
      <button id="clear">清空屏幕</button>
    </div>
    <div id="status"></div>
  </section>
</main>

<script>
const W = 296;
const H = 128;
const canvas = document.getElementById('preview');
const ctx = canvas.getContext('2d', { willReadFrequently: true });
const text = document.getElementById('text');
const fontSize = document.getElementById('fontSize');
const align = document.getElementById('align');
const imageInput = document.getElementById('image');
const threshold = document.getElementById('threshold');
const thresholdValue = document.getElementById('thresholdValue');
const textPanel = document.getElementById('textPanel');
const imagePanel = document.getElementById('imagePanel');
const status = document.getElementById('status');
let sourceImage = null;

function currentMode() {
  return document.querySelector('input[name="mode"]:checked').value;
}

function drawText() {
  ctx.fillStyle = '#fff';
  ctx.fillRect(0, 0, W, H);
  ctx.fillStyle = '#000';
  const size = Number(fontSize.value);
  ctx.font = `${size}px system-ui, "Microsoft YaHei", sans-serif`;
  ctx.textBaseline = 'top';
  ctx.textAlign = align.value;
  const margin = 5;
  const maxWidth = W - margin * 2;
  const lines = [];
  for (const paragraph of text.value.split('\n')) {
    let line = '';
    for (const ch of paragraph) {
      const next = line + ch;
      if (line && ctx.measureText(next).width > maxWidth) {
        lines.push(line);
        line = ch;
      } else {
        line = next;
      }
    }
    lines.push(line);
  }
  const lineHeight = Math.ceil(size * 1.25);
  const x = align.value === 'left' ? margin : align.value === 'right' ? W - margin : W / 2;
  lines.slice(0, Math.floor(H / lineHeight)).forEach((line, i) => {
    ctx.fillText(line, x, i * lineHeight + 2);
  });
}

function drawImage() {
  ctx.fillStyle = '#fff';
  ctx.fillRect(0, 0, W, H);
  if (!sourceImage) return;
  const scale = Math.min(W / sourceImage.width, H / sourceImage.height);
  const dw = Math.round(sourceImage.width * scale);
  const dh = Math.round(sourceImage.height * scale);
  ctx.imageSmoothingEnabled = true;
  ctx.drawImage(sourceImage, Math.round((W - dw) / 2), Math.round((H - dh) / 2), dw, dh);
}

function redraw() {
  if (currentMode() === 'text') drawText(); else drawImage();
}

function packBitmap() {
  const pixels = ctx.getImageData(0, 0, W, H).data;
  const out = new Uint8Array(W * H / 8);
  const limit = Number(threshold.value);
  for (let y = 0; y < H; y++) {
    for (let x = 0; x < W; x++) {
      const p = (y * W + x) * 4;
      const luminance = 0.299 * pixels[p] + 0.587 * pixels[p + 1] + 0.114 * pixels[p + 2];
      if (pixels[p + 3] > 20 && luminance < limit) {
        const index = y * (W / 8) + (x >> 3);
        out[index] |= 0x80 >> (x & 7);
      }
    }
  }
  return out;
}

async function sendBitmap(bytes) {
  document.getElementById('send').disabled = true;
  status.textContent = '正在发送...';
  try {
    let binary = '';
    const chunkSize = 0x8000;
    for (let i = 0; i < bytes.length; i += chunkSize) {
      binary += String.fromCharCode(...bytes.subarray(i, i + chunkSize));
    }
    const encoded = btoa(binary);
    const response = await fetch('/display', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: encoded
    });
    status.textContent = response.ok ? '已发送，电子纸正在刷新。' : await response.text();
  } catch (error) {
    status.textContent = '发送失败，请确认仍连接着 EPD-ESP32 Wi-Fi。';
  } finally {
    document.getElementById('send').disabled = false;
  }
}

document.querySelectorAll('input[name="mode"]').forEach(input => input.addEventListener('change', () => {
  const isText = currentMode() === 'text';
  textPanel.hidden = !isText;
  imagePanel.hidden = isText;
  redraw();
}));
text.addEventListener('input', redraw);
fontSize.addEventListener('change', redraw);
align.addEventListener('change', redraw);
threshold.addEventListener('input', () => {
  thresholdValue.textContent = threshold.value;
  redraw();
});
imageInput.addEventListener('change', () => {
  const file = imageInput.files[0];
  if (!file) return;
  const url = URL.createObjectURL(file);
  sourceImage = new Image();
  sourceImage.onload = () => { URL.revokeObjectURL(url); redraw(); };
  sourceImage.src = url;
});
document.getElementById('send').addEventListener('click', () => sendBitmap(packBitmap()));
document.getElementById('clear').addEventListener('click', () => sendBitmap(new Uint8Array(W * H / 8)));
redraw();
</script>
</body>
</html>
)rawliteral";

void updateDisplay() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    // drawBitmap uses the Adafruit_GFX rotation and page buffer correctly.
    display.drawBitmap(0, 0, frameBuffer, EPD_WIDTH, EPD_HEIGHT, GxEPD_BLACK);
  } while (display.nextPage());
}

int base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

size_t decodeBase64(const String &input, uint8_t *output, size_t outputCapacity) {
  int value = 0;
  int bits = -8;
  size_t outputLength = 0;

  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];
    if (c == '=') break;

    int decoded = base64Value(c);
    if (decoded < 0) continue;

    value = (value << 6) | decoded;
    bits += 6;
    if (bits >= 0) {
      if (outputLength >= outputCapacity) return 0;
      output[outputLength++] = (value >> bits) & 0xff;
      bits -= 8;
    }
  }

  return outputLength;
}

void handleDisplay() {
  String body = server.arg("plain");
  size_t decodedLength = decodeBase64(body, frameBuffer, FRAME_BYTES);
  if (decodedLength != FRAME_BYTES) {
    String message = "Invalid bitmap data: decoded " + String(decodedLength) + ", expected " + String(FRAME_BYTES);
    server.send(400, "text/plain; charset=utf-8", message);
    return;
  }

  updateDisplay();
  server.send(200, "text/plain; charset=utf-8", "OK");
}

void handleClear() {
  memset(frameBuffer, 0, FRAME_BYTES);
  updateDisplay();
  server.send(200, "text/plain; charset=utf-8", "OK");
}

void setup() {
  Serial.begin(115200);
  delay(200);

  memset(frameBuffer, 0, FRAME_BYTES);

  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(115200, true, 2, false);
  // The panel is physically 296x128; GxEPD2's native driver size is 128x296.
  display.setRotation(1);
  display.setFullWindow();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
  });
  server.on("/display", HTTP_POST, handleDisplay);
  server.on("/clear", HTTP_POST, handleClear);
  server.begin();

  Serial.println();
  Serial.println("E-paper web server started");
  Serial.print("Wi-Fi: ");
  Serial.println(AP_SSID);
  Serial.print("Password: ");
  Serial.println(AP_PASSWORD);
  Serial.print("Open: http://");
  Serial.println(WiFi.softAPIP());
}

void loop() {
  server.handleClient();
  delay(2);
}
