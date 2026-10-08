# AI Voice Keyboard - 设计文档

## 概述

在现有 ESP32 用量监视器固件基础上，新增「语音键盘」模式。按下 BOOT 键开始录音，语音通过 WebSocket 实时流式发送到 PC，PC 使用 faster-whisper 本地识别后自动输入到当前光标位置。双击 BOOT 键在监视器模式和语音模式间切换。

## 系统架构

```
ESP32 (Waveshare 2.16)                PC (Python)
┌─────────────────┐                  ┌──────────────────┐
│  I2S 麦克风采集  │ ──WebSocket──→  │  WebSocket Server │
│  (16kHz/16bit)   │   PCM流式传输    │  (asyncio+websock)│
│                  │                  │         │         │
│  BOOT 按键控制    │ ←──WebSocket──  │  faster-whisper   │
│  录音开始/停止    │   识别结果文本    │  (本地模型)        │
│                  │                  │         │         │
│  AMOLED 状态显示  │                  │  keyboard 模拟输入  │
│  (待机/录音/识别) │                  │  (自动打字)        │
└─────────────────┘                  └──────────────────┘
```

## ESP32 固件设计

### 状态机

```
IDLE_MONITOR ←──双击BOOT──→ IDLE_VOICE
                               │
                        按BOOT │
                               ↓
                           RECORDING (WebSocket发送PCM)
                               │
                        再按BOOT │
                               ↓
                           PROCESSING (等待PC返回结果)
                               │
                        收到结果 │
                               ↓
                           DONE (显示识别文字2秒)
                               │
                          自动转回 │
                               ↓
                           IDLE_VOICE
```

### 模式切换逻辑

- **双击 BOOT**（300ms 内两次按下）：在 Monitor 模式和 Voice 模式间切换
- **单击 BOOT**（Voice 模式）：开始录音 / 停止录音
- **单击 BOOT**（Monitor 模式）：保持原有行为（取消思考状态）
- **Key1 / Key2**：两种模式下功能不变（Key1 切换时间页，Key2 保持原有）

### 新增文件

#### `src/voice/i2s_audio.h/.cpp` — I2S 麦克风采集

- 初始化 ES8311 编解码器（通过 I2C 配置 ADC）
- 配置 I2S 驱动：16kHz、16bit、mono
- 提供 `audioStart()` / `audioStop()` / `audioRead(buf, len)` 接口
- DMA 缓冲区使用 PSRAM
- 每次读取 640 字节（20ms @16kHz/16bit/mono），保证低延迟实时流

#### `src/voice/ws_client.h/.cpp` — WebSocket 客户端

- 使用 ESP-IDF 原生 WebSocket 客户端（`esp_websocket_client.h`）
- 连接到 `ws://{pc_host}:{pc_port}/audio`
- `wsConnect(host, port)` — 建立 WebSocket 连接
- `wsSendBin(data, len)` — 发送二进制 PCM 数据
- `wsSendText(json)` — 发送 JSON 控制消息
- `wsIsConnected()` — 检查连接状态
- `wsDisconnect()` — 断开连接
- 接收线程：解析 PC 返回的识别结果，设置全局状态

#### `src/voice/voice_ui.h/.cpp` — 语音模式 UI

- `voiceUiDrawIdle(ip)` — 显示 "VOICE" + IP + "按BOOT开始"
- `voiceUiDrawListening(elapsed)` — 显示 🎤 + "Listening..." + 录音时长
- `voiceUiDrawProcessing()` — 显示 🧠 + "Processing..."
- `voiceUiDrawResult(text)` — 显示 ✅ + 识别文本（2秒后自动切回 IDLE）
- 复用现有 display.h 的 spr 宏和 SAFE_L/SAFE_R 坐标系

### 修改文件

#### `main.cpp`

- 添加模式状态机（`enum AppMode { MODE_MONITOR, MODE_VOICE }`）
- 添加录音状态机（`enum VoiceState { VOICE_IDLE, VOICE_RECORDING, VOICE_PROCESSING, VOICE_DONE }`）
- 双击检测逻辑（检测 300ms 内两次 BOOT 按下）
- Voice 模式下的 loop 逻辑：录音 → WebSocket 发送 → 等待结果
- 保持 Monitor 模式完全不变

#### `config.h`

- AppConfig 新增 `pc_host[64]` 字段（WebSocket 服务器地址）
- 保留 `pc_port` 字段（WebSocket 端口）

#### `platformio.ini`

- 添加 WebSocket 依赖（使用 ESP-IDF 原生组件，无需额外 Arduino 库）

### I2S / ES8311 配置

```cpp
// I2S 配置
i2s_config_t i2s_cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I16,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 640,   // 20ms per buffer
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
};

// 引脚映射 (board header)
PIN_I2S_MCLK = 42
PIN_I2S_BCLK = 9
PIN_I2S_WS   = 45
PIN_I2S_DI   = 10
PIN_I2S_DO   = 8
PIN_PA_CTRL  = 46
```

ES8311 通过 I2C 初始化（地址 0x18），设置 ADC 通道为麦克风输入。

## WebSocket 协议

### ESP32 → PC

| 帧类型 | 内容 | 说明 |
|--------|------|------|
| 文本 | `{"type":"start"}` | 开始录音 |
| 二进制 | PCM 原始数据 | 16kHz/16bit/mono 音频流 |
| 文本 | `{"type":"stop"}` | 停止录音 |

### PC → ESP32

| 帧类型 | 内容 | 说明 |
|--------|------|------|
| 文本 | `{"type":"result","text":"识别结果"}` | 识别成功 |
| 文本 | `{"type":"error","msg":"错误信息"}` | 识别失败 |

## PC 端设计（tools/voice_server.py）

单文件 Python 脚本，整合所有 PC 侧功能：

### 1. WebSocket Server（asyncio + websockets 库）

- 监听 `{pc_port}` 端口
- 路径 `/audio`
- 接收 ESP32 的 WebSocket 连接

### 2. 音频缓冲

- 接收 PCM 二进制帧，拼接入内存缓冲区
- 收到 `stop` 消息后，将缓冲区数据转为 WAV 格式

### 3. 语音识别（faster-whisper）

- 使用 `faster_whisper.WhisperModel` 加载本地模型
- 默认 `base` 模型（中英混合）
- 支持通过参数指定模型大小
- 识别结果返回中文/英文

### 4. 键盘输入（keyboard 库）

- 使用 `keyboard.write()` 将识别文本输入到当前焦点窗口
- 如果 keyboard 不可用（Linux 无 root），回退到 `pyautogui`

### 5. 命令行参数

```bash
python voice_server.py [--host 0.0.0.0] [--port 12345] [--model base] [--lang zh]
```

### 依赖

```
websockets>=12.0
faster-whisper>=1.0
keyboard>=0.13
numpy
```

## config.json 格式扩展

```json
{
  "server_id": "...",
  "cookie": "...",
  "workspace_id": "...",
  "pc_port": 12345,
  "pc_host": "192.168.31.100"
}
```

`pc_host` 为 PC 的 IP 地址（WebSocket 服务器地址），ESP32 需要知道往哪里连。如果未配置，Voice 模式显示 "请配置PC地址"。

## 实现优先级

按以下顺序实现并验证：

1. ESP32 I2S 麦克风采集（先验证能录到音频）
2. ESP32 WebSocket 客户端连接 PC（先验证通信通）
3. PC WebSocket Server 接收音频并保存 WAV（验证端到端音频传输）
4. PC 集成 faster-whisper 识别（验证语音转文字）
5. PC 自动键盘输入（验证打字到光标）
6. ESP32 语音模式 UI 和状态机（整合所有功能）
7. 双击模式切换（最后完善交互）

## 约束

- ESP32 不做任何语音识别，只负责采集和传输
- 禁止使用云 API（除非后续明确要求）
- 禁止 HTTP 轮询传输音频（必须是 WebSocket 流式）
- 音频格式固定为 16kHz/16bit/mono PCM
- 延迟目标：< 2秒（停止说话到出字）
- 支持中文 + 英文混合识别