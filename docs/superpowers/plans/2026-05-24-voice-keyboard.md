# AI Voice Keyboard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add voice keyboard mode to the ESP32 usage monitor — press BOOT to record, stream PCM via WebSocket to PC, PC runs faster-whisper, types result into focused app.

**Architecture:** ESP32 captures I2S audio (16kHz/16bit/mono) and streams via WebSocket to a Python server on PC. Python server uses faster-whisper for ASR and keyboard library for text input. Double-click BOOT switches between monitor and voice modes.

**Tech Stack:** ESP-IDF I2S driver, ESP-IDF WebSocket client, ES8311 codec, Python asyncio/websockets/faster-whisper/keyboard

---

### Task 1: Add pc_host to config.h

**Files:**
- Modify: `src/config.h`

- [ ] **Step 1: Add pc_host field to AppConfig**

Add `char pc_host[64]` field to AppConfig struct and update loadConfig to read it:

```cpp
struct AppConfig {
  char server_id[256];
  char cookie[2048];
  char workspace_id[128];
  char pc_host[64];
  uint16_t pc_port;
  bool valid;
};
```

In loadConfig, add after the pc_port line:
```cpp
strlcpy(cfg.pc_host, doc["pc_host"] | "", sizeof(cfg.pc_host));
```

- [ ] **Step 2: Commit**

```bash
git add src/config.h
git commit -m "feat: add pc_host field to AppConfig for voice keyboard"
```

---

### Task 2: Create I2S audio module

**Files:**
- Create: `src/voice/i2s_audio.h`
- Create: `src/voice/i2s_audio.cpp`

- [ ] **Step 1: Create i2s_audio.h**

```cpp
#pragma once
#include <stdint.h>
#include <stddef.h>

bool audioInit();
bool audioStart();
void audioStop();
size_t audioRead(uint8_t* buf, size_t len);
```

- [ ] **Step 2: Create i2s_audio.cpp**

Initialize I2S for 16kHz/16bit/mono capture via ES8311 ADC on the Waveshare 2.16 board. Use ESP-IDF I2S driver (`driver/i2s_std.h`). Configure pins from board header (PIN_I2S_MCLK=42, PIN_I2S_BCLK=9, PIN_I2S_WS=45, PIN_I2S_DI=10, PIN_I2S_DO=8). Also initialize ES8311 via I2C (address 0x18) for ADC mode, and enable PA_CTRL (GPIO46) for microphone amplifier.

Key implementation details:
- Use `i2s_std_config_t` with 16kHz, 16bit, mono
- DMA buffers: 8 slots × 640 bytes (20ms per buffer)
- `audioStart()` installs and enables the I2S driver
- `audioStop()` disables the driver
- `audioRead()` reads from I2S DMA, returns bytes read

- [ ] **Step 3: Commit**

```bash
git add src/voice/
git commit -m "feat: add I2S audio capture module for voice keyboard"
```

---

### Task 3: Create WebSocket client module

**Files:**
- Create: `src/voice/ws_client.h`
- Create: `src/voice/ws_client.cpp`

- [ ] **Step 1: Create ws_client.h**

```cpp
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

bool wsConnect(const char* host, uint16_t port);
void wsDisconnect();
bool wsIsConnected();
bool wsSendBin(const uint8_t* data, size_t len);
bool wsSendText(const char* text);

typedef void (*WsResultCallback)(const char* text);
void wsSetResultCallback(WsResultCallback cb);

void wsPoll();
```

- [ ] **Step 2: Create ws_client.cpp**

Use ESP-IDF native WebSocket client (`esp_websocket_client.h`). Connect to `ws://{host}:{port}/audio`. Handle events:
- `WEBSOCKET_EVENT_DATA`: If text frame, parse JSON for `{"type":"result","text":"..."}` and invoke result callback
- `WEBSOCKET_EVENT_CONNECTED` / `WEBSOCKET_EVENT_DISCONNECTED`: Track connection state

Use `esp_websocket_client_init()` with `esp_websocket_client_config_t` setting URI, `skip_cert_common_name=true`. Task-based: the IDF WebSocket client runs its own background task.

- [ ] **Step 3: Commit**

```bash
git add src/voice/ws_client.*
git commit -m "feat: add WebSocket client module for voice streaming"
```

---

### Task 4: Create voice UI module

**Files:**
- Create: `src/voice/voice_ui.h`
- Create: `src/voice/voice_ui.cpp`

- [ ] **Step 1: Create voice_ui.h**

```cpp
#pragma once
void voiceUiDrawIdle(const char* ip);
void voiceUiDrawListening(uint32_t elapsedMs, bool wsConnected);
void voiceUiDrawProcessing();
void voiceUiDrawResult(const char* text);
```

- [ ] **Step 2: Create voice_ui.cpp**

Implement four drawing functions using `hwCanvas()` / spr and SAFE_L/SAFE_T/SAFE_W/SAFE_H coordinates:
- `voiceUiDrawIdle`: Show "VOICE" title, IP, "Press BOOT to start", and WiFi status
- `voiceUiDrawListening`: Show recording icon + "Listening..." + elapsed time (mm:ss) + WebSocket status indicator
- `voiceUiDrawProcessing`: Show processing animation (reuse think.anim) + "Processing..."
- `voiceUiDrawResult`: Show checkmark + transcribed text (auto-wrap, truncated if too long)

Use u8g2 fonts for CJK support (u8g2_font_wqy12_t_gb2312b, u8g2_font_wqy16_t_gb2312b).

- [ ] **Step 3: Commit**

```bash
git add src/voice/voice_ui.*
git commit -m "feat: add voice mode UI drawing functions"
```

---

### Task 5: Modify main.cpp — add mode state machine and double-click

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Add includes and state enums**

Add at top of main.cpp:
```cpp
#include "voice/i2s_audio.h"
#include "voice/ws_client.h"
#include "voice/voice_ui.h"
```

Add enums:
```cpp
enum AppMode { MODE_MONITOR, MODE_VOICE };
enum VoiceState { VOICE_IDLE, VOICE_RECORDING, VOICE_PROCESSING, VOICE_DONE };
```

Add globals:
```cpp
static AppMode s_mode = MODE_MONITOR;
static VoiceState s_voiceState = VOICE_IDLE;
static uint32_t s_recordStart = 0;
static uint32_t s_doneStart = 0;
static char s_resultText[256] = {};
static uint32_t s_lastBootPress = 0;
static bool s_doubleClickPending = false;
static bool s_wsConnected = false;
static volatile bool s_resultReady = false;
```

- [ ] **Step 2: Add WebSocket result callback**

```cpp
static void onWsResult(const char* text) {
    strlcpy(s_resultText, text, sizeof(s_resultText));
    s_resultReady = true;
}
```

- [ ] **Step 3: Add double-click detection**

In loop(), replace the BOOT key handling section with:
```cpp
// BOOT key: double-click detection
if (hwBtnBoot().wasPressed) {
    uint32_t now = millis();
    if (now - s_lastBootPress < 300) {
        // Double click: toggle mode
        s_doubleClickPending = false;
        if (s_mode == MODE_MONITOR) {
            s_mode = MODE_VOICE;
            s_voiceState = VOICE_IDLE;
            wsConnect(s_cfg.pc_host, s_cfg.pc_port);
        } else {
            s_mode = MODE_MONITOR;
            if (s_voiceState == VOICE_RECORDING) {
                audioStop();
                wsSendText("{\"type\":\"stop\"}");
            }
            s_voiceState = VOICE_IDLE;
            wsDisconnect();
        }
        s_lastBootPress = 0;
    } else {
        s_doubleClickPending = true;
        s_lastBootPress = now;
    }
}
// Single click timeout
if (s_doubleClickPending && millis() - s_lastBootPress >= 300) {
    s_doubleClickPending = false;
    if (s_mode == MODE_MONITOR) {
        // Original behavior: cancel thinking
        if (s_thinking) s_thinking = false;
    } else {
        // Voice mode: start/stop recording
        if (s_voiceState == VOICE_IDLE) {
            s_voiceState = VOICE_RECORDING;
            s_recordStart = millis();
            audioStart();
            wsSendText("{\"type\":\"start\"}");
        } else if (s_voiceState == VOICE_RECORDING) {
            audioStop();
            wsSendText("{\"type\":\"stop\"}");
            s_voiceState = VOICE_PROCESSING;
        }
    }
}
```

- [ ] **Step 4: Add voice loop logic**

In loop(), add audio streaming in VOICE_RECORDING state:
```cpp
if (s_mode == MODE_VOICE && s_voiceState == VOICE_RECORDING) {
    uint8_t pcmBuf[640];
    size_t n = audioRead(pcmBuf, sizeof(pcmBuf));
    if (n > 0 && wsIsConnected()) {
        wsSendBin(pcmBuf, n);
    }
}
```

Handle result from WebSocket:
```cpp
wsPoll();
if (s_resultReady && s_voiceState == VOICE_PROCESSING) {
    s_resultReady = false;
    s_voiceState = VOICE_DONE;
    s_doneStart = millis();
}
```

Auto-return from DONE state after 2s:
```cpp
if (s_voiceState == VOICE_DONE && millis() - s_doneStart >= 2000) {
    s_voiceState = VOICE_IDLE;
}
```

- [ ] **Step 5: Add voice mode display rendering**

Replace the display section in loop():
```cpp
if (s_thinking) {
    usageDisplayDrawThinking(s_thinkingSince);
} else if (s_mode == MODE_MONITOR) {
    if (s_showTime) {
        usageDisplayDrawTime(WiFi.localIP().toString().c_str(), s_timeValid);
    } else {
        usageDisplayDraw(s_usage, WiFi.localIP().toString().c_str());
    }
} else {
    // Voice mode
    s_wsConnected = wsIsConnected();
    switch (s_voiceState) {
    case VOICE_IDLE:
        voiceUiDrawIdle(WiFi.localIP().toString().c_str());
        break;
    case VOICE_RECORDING:
        voiceUiDrawListening(millis() - s_recordStart, s_wsConnected);
        break;
    case VOICE_PROCESSING:
        voiceUiDrawProcessing();
        break;
    case VOICE_DONE:
        voiceUiDrawResult(s_resultText);
        break;
    }
}
```

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp
git commit -m "feat: integrate voice keyboard mode into main loop"
```

---

### Task 6: Update platformio.ini

**Files:**
- Modify: `platformio.ini`

- [ ] **Step 1: Add build flags and dependencies**

Add to `build_flags`:
```
-DCONFIG_ESP_WEBSOCKET_CLIENT
```

The ESP-IDF WebSocket client is already included in the ESP-IDF framework. For I2S, the ESP-IDF driver is also native. No extra Arduino libraries are needed, but we need to ensure the IDF components are linked.

Add under `[env:esp32-usage-monitor]` or in the global `build_flags`:
```
-DBOARD_HAS_ES8311
```

- [ ] **Step 2: Commit**

```bash
git add platformio.ini
git commit -m "feat: add voice keyboard build flags to platformio.ini"
```

---

### Task 7: Create PC voice_server.py

**Files:**
- Create: `tools/voice_server.py`

- [ ] **Step 1: Create voice_server.py**

Single-file Python script implementing:
1. asyncio + websockets WebSocket server on `/audio` path
2. Audio buffer: accumulate PCM frames, convert to WAV when `stop` received
3. faster-whisper integration: transcribe WAV, return text
4. keyboard.write() for text input
5. Command-line args: --host, --port, --model, --lang

Key structure:
```python
import asyncio
import argparse
import struct
import wave
import io
import sys
import tempfile

async def handle_audio(websocket):
    pcm_buf = bytearray()
    model = None  # lazy load
    while True:
        msg = await websocket.recv()
        if isinstance(msg, str):
            # JSON control message
            if '"start"' in msg:
                pcm_buf.clear()
            elif '"stop"' in msg:
                # Convert to WAV, transcribe
                wav_data = pcm_to_wav(pcm_buf)
                text = transcribe(wav_data, args.model, args.lang)
                await websocket.send(f'{{"type":"result","text":"{text}"}}')
                keyboard_write(text)
                pcm_buf.clear()
        elif isinstance(msg, bytes):
            pcm_buf.extend(msg)

def pcm_to_wav(pcm, rate=16000, bits=16, channels=1):
    buf = io.BytesIO()
    with wave.open(buf, 'wb') as w:
        w.setnchannels(channels)
        w.setsampwidth(bits // 8)
        w.setframerate(rate)
        w.writeframes(pcm)
    return buf.getvalue()

def transcribe(wav_data, model_name, lang):
    from faster_whisper import WhisperModel
    model = WhisperModel(model_name, device="cpu", compute_type="int8")
    segments, info = model.transcribe(io.BytesIO(wav_data), language=lang)
    return " ".join(s.text for s in segments)

def keyboard_write(text):
    try:
        import keyboard
        keyboard.write(text)
    except:
        import pyautogui
        pyautogui.write(text)
```

- [ ] **Step 2: Commit**

```bash
git add tools/voice_server.py
git commit -m "feat: add PC voice server with faster-whisper and keyboard input"
```

---

### Task 8: Compile and verify

- [ ] **Step 1: Run PlatformIO build**

```bash
cd D:\esp32\esp32-usage-monitor
pio run -e esp32-usage-monitor
```

Fix any compilation errors iteratively. Common issues:
- Missing includes
- I2S API differences between ESP-IDF versions
- WebSocket client config differences

- [ ] **Step 2: Commit any fixes**

```bash
git add -A
git commit -m "fix: resolve compilation errors for voice keyboard"
```