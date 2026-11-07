# Mo-Chan Wheeled Voice Robot

Mo-Chan 是一个基于 ESP32-S3 的轮式互动小机器人示例。主固件把网页遥控、语音输入、云端语音识别、LLM 对话、语音合成、电机控制和 OLED 表情整合到同一套流程中；仓库同时保留一个独立的电子纸网页控制固件。

## 功能概览

- 手机网页控制前进、后退、左右转向和原地旋转，松开按键自动停车。
- 通过 INMP441 录音，发送到电脑端中继服务完成 ASR、对话和 TTS，再由 MAX98357A 播放回答。
- OLED 使用 RoboEyes 显示待机、聆听、思考、说话和错误状态表情。
- 电子纸固件可创建独立 Wi-Fi 热点，通过网页发送文字或图片到 296×128 黑白电子纸。

## 仓库结构

```text
firmware/
  voice_robot/       ESP32-S3 主固件：电机、OLED、网页遥控、语音交互
  epaper_console/    电子纸独立网页控制固件
server/
  voice-relay/       电脑端 ASR -> DeepSeek -> TTS 中继服务
libraries/
  RoboEyes/          主固件需要的本地 Arduino 库
docs/
  wiring.png         项目接线图
```

压缩包、编译缓存、旧版测试草稿和与当前功能无关的示例没有上传。

## 主固件使用方法

### 1. 准备 Arduino 环境

在 Arduino IDE 中安装：

- Espressif ESP32 board package，并选择 ESP32-S3 对应开发板
- Adafruit GFX Library
- Adafruit SSD1306
- GxEPD2 仅用于电子纸固件

将 `libraries/RoboEyes` 复制到 Arduino 的 `libraries` 目录，或通过 Arduino IDE 选择 `Sketch > Include Library > Add .ZIP Library` 后使用同名库。

主固件默认使用以下硬件：ESP32-S3、SSD1315/SSD1306 OLED、INMP441 麦克风、MAX98357A 功放、TC1508/MX1508 电机驱动和两个 N20 减速电机。具体接线见 [`docs/wiring.png`](docs/wiring.png)。

### 2. 配置并上传主固件

打开 `firmware/voice_robot/AI_Voice_Assistant.ino`，修改以下配置：

```cpp
const char *WIFI_SSID = "你的 Wi-Fi 名称";
const char *WIFI_PASSWORD = "你的 Wi-Fi 密码";
const char *RELAY_URL = "http://电脑局域网IPv4:8765/ask";
```

上传前确认电机电池已断开，并将串口监视器设置为 `115200`。固件启动后会打印 ESP32 的网页地址，例如：

```text
Motor control: http://192.168.x.x/
```

手机连接到同一个 Wi-Fi 后访问该地址。按住方向键移动，松开后停车；`Voice Chat` 按钮支持按住说话、松开提交。也可以在串口监视器发送 `r`，启动一次固定 4 秒录音。

### 3. 启动电脑端语音中继

电脑和 ESP32 必须连接到同一个局域网。在 `server/voice-relay` 目录安装依赖：

```powershell
python -m pip install -r requirements.txt
```

当前中继链路为：

```text
ESP32 WAV -> 火山引擎 ASR -> DeepSeek 对话 -> 火山引擎 TTS -> ESP32 WAV 播放
```

在启动中继的同一个 PowerShell 窗口中配置密钥，不要把真实密钥写进代码或提交到 GitHub：

```powershell
$env:VOLC_ASR_API_KEY = "你的火山引擎 API Key"
$env:VOLC_TTS_API_KEY = $env:VOLC_ASR_API_KEY
$env:VOLC_ASR_RESOURCE_ID = "volc.seedasr.sauc.duration"
$env:VOLC_TTS_RESOURCE_ID = "seed-tts-2.0"
$env:VOLC_TTS_SPEAKER = "zh_female_shuangkuaisisi_moon_bigtts"
$env:DEEPSEEK_API_KEY = "你的 DeepSeek API Key"
$env:RELAY_PORT = "8765"
python relay_server.py
```

如果端口被占用，可改成其他端口，并同步修改固件中的 `RELAY_URL`。启动成功后，中继服务会监听 `http://电脑IP:8765/ask`。若 ASR 或 TTS 报错，优先检查服务是否开通、资源 ID、音色名称和 API Key 是否匹配。

## 电子纸控制台

`firmware/epaper_console/epaper_web.ino` 是独立固件，不需要和主固件同时上传。安装 GxEPD2 后上传，电脑或手机连接它创建的 Wi-Fi：

```text
Wi-Fi: EPD-ESP32
密码: change-me-123
地址: http://192.168.4.1
```

网页支持输入文字或选择图片，预览黑白阈值后发送到电子纸，也可以清空屏幕。若要公开使用或长期部署，请在代码中修改热点名称和密码。

## 注意事项

- 电机电池不能接到 ESP32 的 3V3、5V 或 VBUS；电机驱动电源与 ESP32 必须共地。
- 首次调试时先让车轮离开桌面，并将功放音量调低。
- 语音功能依赖电脑端中继服务和第三方云服务；没有配置 API Key 时，网页遥控和 OLED 表情仍可单独使用。
- 如果某个电机转向相反，在主固件中调整 `MOTOR_A_INVERT` 或 `MOTOR_B_INVERT`，也可以交换对应电机的两根线。

