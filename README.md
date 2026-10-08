# ESP32-S3 用量监视器

在 Waveshare ESP32-S3-Touch-AMOLED-2.16 开发板上显示 OpenCode Token 用量的固件。

<img width="307" height="409" alt="1b2b0536bc8ad161e428c0deda3aafb6" src="https://github.com/user-attachments/assets/d59a0a6f-1483-4df2-9ebe-b20832e409ec" />
<img width="307" height="409" alt="b598914ee3e2701626b0c7eb8ab07a65" src="https://github.com/user-attachments/assets/d649e905-e27e-4900-8f70-dff1deca24a8" />




## 功能

- 显示 **5小时** / **每周** / **每月** 用量进度条
- 显示距离重置的倒计时（天时分秒格式）
- 超过 10% 进度条变黄，超过 50% 变红
- 每 60 秒自动刷新，按最左边按键可手动刷新
- WiFi 配置界面（按钮输入密码）
- 配置存储在 LittleFS 文件系统

## 硬件

- **开发板**: Waveshare ESP32-S3-Touch-AMOLED-2.16
- **屏幕**: 480×480 AMOLED (CO5300 驱动)
- **触摸**: CST9220
- **PSRAM**: 8MB | **Flash**: 16MB

## 环境搭建

1. 安装 [PlatformIO](https://platformio.org/)
2. 克隆仓库：
   ```bash
   git clone https://github.com/2700zc/esp32-usage-monitor.git
   cd esp32-usage-monitor
   ```

## 配置

复制配置模板并填入你的信息：

```bash
cp data/config.json.example data/config.json
```

编辑 `data/config.json`：

```json
{
  "server_id": "你的 OpenCode server_id",
  "cookie": "oc_locale=zh; auth=你的认证 cookie",
  "workspace_id": "你的 workspace_id (wrk_开头)",
  "baidu_app_id": "",
  "baidu_api_key": "",
  "baidu_secret_key": ""
}
```

### 获取配置信息

1. 登录 [opencode.ai](https://opencode.ai)
2. 打开浏览器开发者工具 (F12) → 网络 (Network)
3. 访问用量页面，抓取 `_server` 请求
4. 从请求 URL 中提取 `id` 参数 → `server_id`
5. 从请求头中提取 `Cookie` → `cookie`
6. 从请求参数中提取 `args` 里的 workspace ID → `workspace_id`

## 编译与上传

```bash
# 编译
pio run -e esp32-usage-monitor

# 上传固件
pio run -e esp32-usage-monitor -t upload

# 上传文件系统（包含 config.json）
pio run -e esp32-usage-monitor -t uploadfs

# 同时上传固件和文件系统
pio run -e esp32-usage-monitor -t upload -t uploadfs
```

## 使用说明

1. 首次启动进入 WiFi 配置界面
2. 用三个按键输入 WiFi 密码：
   - **按键A（左）**: 切换字符
   - **按键B（中）**: 确认输入
   - **按键Boot（右）**: 重置
3. 连接 WiFi 后长按按键A（600ms）进入桌面
4. 桌面自动每 60 秒刷新用量数据
5. 按最左边按键可手动刷新

## 项目结构

```
├── data/
│   ├── config.json          # 运行时配置（已 gitignore）
│   └── config.json.example  # 配置模板
├── src/
│   ├── main.cpp             # 主循环和状态机
│   ├── api_client.cpp/h     # OpenCode API 客户端
│   ├── usage_display.cpp/h  # 用量显示界面
│   ├── wifi_setup.cpp/h     # WiFi 配置界面
│   ├── config.h             # 配置结构体定义
│   ├── audio_recorder.cpp/h # 录音（保留未使用）
│   ├── usb_keyboard.cpp/h   # USB 键盘（保留未使用）
│   └── hw/                  # 硬件驱动层
│       ├── display.cpp/h    # AMOLED 显示驱动
│       ├── input.cpp/h      # 按键和触摸输入
│       ├── power.cpp/h      # 电源管理 (AXP2101)
│       ├── audio.cpp/h      # 音频驱动 (ES8311)
│       ├── imu.cpp/h        # 加速度计
│       ├── rtc.cpp/h        # 实时时钟
│       ├── expander.cpp/h   # IO 扩展器
│       ├── border.cpp/h     # 边框绘制
│       ├── net.cpp/h        # 网络工具
│       ├── pins.h            # 引脚定义
│       └── hw.cpp/h         # 硬件初始化
├── lib/                      # 本地库
└── platformio.ini            # PlatformIO 配置
```

## 注意事项

- `data/config.json` 包含认证信息，已被 `.gitignore` 排除，不会上传到仓库
- 每次修改 `config.json` 后需要重新上传文件系统：`pio run -e esp32-usage-monitor -t uploadfs`
- Cookie 有效期有限，失效后需要从浏览器重新获取并更新配置

## 友情链接
- 同时还做了一个Windows上的用量显示悬浮窗，地址是：https://github.com/2700zc/TokenHub 配置好了之后，开箱即用

## License

MIT
