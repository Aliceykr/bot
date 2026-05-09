# Bot — ESP32-S3 AI 智能语音助手 & Game Boy 模拟器

基于 ESP-IDF + LVGL 的 ESP32-S3 嵌入式项目，集成 WiFi、天气查询、AI 聊天、语音识别、语音合成、Game Boy 游戏模拟器，配备 TFT LCD 图形界面与旋转编码器交互。

---

## 硬件清单

| 组件 | 型号 / 规格 | 说明 |
|------|------------|------|
| 主控 | ESP32-S3-DevKitC-1 N16R8 | 16MB Flash + 8MB Octal PSRAM |
| 屏幕 | 2.4" TFT LCD，240x320，ILI9341 | SPI 接口，80 MHz |
| 麦克风 | INMP441 | I2S 数字麦克风，I2S_NUM_0 |
| 扬声器 | MAX98357A + 喇叭 | I2S D 类功放，I2S_NUM_1 |
| 输入 | 旋转编码器（A/B/SW）| PCNT 硬件计数 + GPIO 轮询 |

---

## 引脚接线

### LCD（SPI2，ILI9341）

| 功能 | ESP32-S3 引脚 | 说明 |
|------|---------------|------|
| MOSI | GPIO11 | SPI 数据 |
| SCLK | GPIO12 | SPI 时钟，80 MHz |
| RES  | GPIO10 | 复位 |
| DC   | GPIO9  | 数据/命令选择 |
| BLK  | GPIO46 | 背光（高电平开启）|

### INMP441 麦克风（I2S_NUM_0）

| 功能 | ESP32-S3 引脚 | INMP441 引脚 |
|------|---------------|--------------|
| BCLK | GPIO38 | SCK |
| LRCK | GPIO39 | WS |
| DATA | GPIO40 | SD |
| 3.3V | 3.3V | VDD |
| GND  | GND  | GND、L/R（接地选左声道）|

### MAX98357A 功放（I2S_NUM_1）

| 功能 | ESP32-S3 引脚 | MAX98357A 引脚 |
|------|---------------|----------------|
| BCLK | GPIO15 | BCLK |
| LRCK | GPIO16 | LRC |
| DATA | GPIO17 | DIN |
| 3.3V | 3.3V   | SD（常开）|

### 旋转编码器

| 功能 | ESP32-S3 引脚 |
|------|---------------|
| A    | GPIO4 |
| B    | GPIO5 |
| SW（按键）| GPIO6 |

A/B 相由 PCNT 硬件正交解码，SW 按键 5ms 轮询状态机（20ms 防抖）。

---

## 目录结构

```
bot/
├── main/
│   ├── main.c                    # 应用入口：初始化外设、创建 LVGL 任务
│   ├── CMakeLists.txt            # 编译入口，列出所有源文件
│   └── idf_component.yml        # IDF 组件依赖
├── mylvgl/
│   ├── my_demo.c                 # LVGL 主界面逻辑（菜单/天气/聊天/语音/游戏）
│   ├── my_demo.h
│   ├── lv_port_disp.c            # LVGL 显示驱动（PARTIAL 双缓冲 + 异步 DMA）
│   ├── lv_port_disp.h
│   ├── lv_port_indev.c           # LVGL 输入设备（PCNT 编码器）
│   └── lv_port_indev.h
├── user/
│   ├── lcd.c / lcd.h             # ILI9341 SPI LCD 底层驱动
│   ├── lcdfont.h                 # ASCII 点阵字库（12/16/24/32px）
│   ├── asr.c / asr.h             # INMP441 I2S 录音 + 百度语音识别 API
│   ├── asr_config.h              # 百度 ASR 凭证（已 gitignore）
│   ├── asr_config.h.example      # 配置模板
│   ├── baidu_token.c / .h        # 百度 OAuth2 token 管理（缓存 + 过期刷新）
│   ├── model.c / model.h         # LLM 聊天（OpenAI 兼容 REST API）
│   ├── model_config.h            # API Key / URL / 模型名（已 gitignore）
│   ├── model_config.h.example    # 配置模板
│   ├── speaker.c / speaker.h     # MAX98357A I2S 音频输出 + RingBuffer 播放队列
│   ├── tts.c / tts.h             # 百度语音合成 API（流式 PCM 播放）
│   ├── weather.c / weather.h     # 天气 HTTP 查询与解析
│   ├── wifi.c / wifi.h           # WiFi STA 连接 + 守护任务（断线重连）
│   ├── sntp_time.c / sntp_time.h # SNTP 网络时间同步（阿里云 NTP）
│   ├── health.c / health.h       # 堆内存健康监控（周期打印 DRAM/PSRAM 水位）
│   ├── lv_font_simhei_16.c       # 思黑体 16px LVGL 中文字体
│   └── lv_font_simhei_20.c       # 思黑体 20px LVGL 中文字体
├── game/
│   ├── gb_emu.c / gb_emu.h       # Peanut-GB Game Boy 模拟器集成
│   ├── game_runtime.c / .h       # 游戏生命周期管理（加载/运行/退出）
│   ├── rom_loader.c / .h         # SPIFFS ROM 扫描与加载
│   ├── peanut_gb.h               # Peanut-GB 单头文件模拟器
│   └── CMakeLists.txt
├── lvgl/                         # LVGL 9.x 源码
├── lv_conf.h                     # LVGL 配置
├── partitions.csv                # 自定义分区表（6MBx2 OTA + 4MB SPIFFS）
├── sdkconfig.defaults            # 公共默认配置
├── sdkconfig.defaults.esp32s3    # ESP32-S3 专用配置
└── CMakeLists.txt                # 顶层 CMake
```

---

## 功能说明

### 1. 主菜单界面

开机后显示功能列表，通过旋转编码器上下选择、按键确认：

- **WiFi 连接** — 后台连接 WiFi，弹窗显示进度，成功后自动同步 NTP 时间
- **天气查询** — HTTP 拉取实时天气，展示温度、湿度、风力、实时时钟
- **游戏** — 扫描 SPIFFS 中的 .gb/.gbc ROM 文件，选择运行 Peanut-GB 模拟器
- **聊天助手** — 屏幕键盘输入文字，调用 LLM API 获取回复，滚动对话记录
- **语音助手** — 录音 → ASR 识别 → LLM 回复 → TTS 合成语音播放
- **重启** — 软件重启设备

### 2. 语音助手完整流程

```
用户按键开始录音
    ↓
INMP441 I2S 采集（16kHz 16bit mono，最长10秒，缓存于 PSRAM）
    ↓
百度语音识别 API（dev_pid=80001 普通话）
    ↓
识别文字送入 LLM（OpenAI 兼容接口，max_tokens=128）
    ↓
百度语音合成 API（流式返回 WAV PCM）
    ↓
MAX98357A 播放合成语音（RingBuffer + I2S DMA）
```

### 3. Game Boy 模拟器

- 基于 Peanut-GB，GB 原生分辨率 160x144，1.5 倍缩放到 240x216
- SPI 80MHz 异步 DMA 双行缓冲渲染
- 小 ROM（<=256KB）自动复制到 DRAM 加速
- Frame skip = 1（60fps 仿真，30fps 显示）
- 游戏运行时独占 SPI 总线，LVGL 显示暂停
- 长按编码器 SW 键（800ms）退出游戏

### 4. WiFi 守护

- 首次连接：EventGroup 等待结果，最多重试 3 次
- 运行期断线：常驻守护任务自动重连，指数退避（5s → 5min）
- 用户主动断开：停止守护，不自动重连

---

## 系统架构

### 任务结构

| 任务名 | 优先级 | 栈大小 | 核心 | 说明 |
|--------|--------|--------|------|------|
| `lv_tick` | 5 | 2048 B | 任意 | LVGL 时钟（5ms tick） |
| `lv_task` | 4 | 32768 B (PSRAM) | 任意 | LVGL 渲染 + flush |
| `enc_task` | 6 | 2048 B | 任意 | SW 按键状态机（5ms 轮询） |
| `spk_tx` | 3 | 2048 B | 任意 | RingBuffer → I2S DMA 播放 |
| `asr_rec` | 5 | 2048 B | 任意 | I2S 录音常驻任务 |
| `wifi_task` | 3 | 4096 B | 任意 | WiFi 连接 + NTP 同步（一次性） |
| `wifi_guard` | 4 | 3072 B | 任意 | 断线重连守护（常驻） |
| `weather_task` | 3 | 16384 B | 任意 | 天气查询（一次性） |
| `chat_task` | 3 | 16384 B | 任意 | LLM 请求（一次性） |
| `asr_task` | 3 | 16384 B | 任意 | ASR 识别（一次性） |
| `asr_llm` | 3 | 16384 B | 任意 | ASR → LLM → TTS（一次性） |
| `game_run` | 10 | 8192 B | 任意 | GB 模拟器主循环（游戏运行期间） |
| `exit_watch` | 4 | 2048 B | 任意 | 长按退出监视 |
| `health` | 1 | 2048 B | 任意 | 堆内存监控（60s 周期） |

### 线程安全

- LVGL 对象操作均在 `lv_task` 单线程执行
- 后台任务通过 FreeRTOS Queue 传递结果，`lv_timer` 回调轮询队列更新 UI
- `speaker_play()` 使用 RingBuffer 非阻塞写入，TTS HTTP 回调可直接调用
- HTTP 响应缓冲由各模块 mutex 保护

### LCD 刷新机制

PARTIAL 模式 + DRAM 双缓冲 + 异步 DMA：

1. LVGL 按 buffer 大小（34 行 = 16KB）分块渲染脏区到 DRAM buffer
2. `disp_flush` 通过 `spi_device_queue_trans` 异步提交 DMA
3. 立即调用 `lv_display_flush_ready()`，LVGL 可渲染下一块到另一个 buffer
4. 下次 flush 前等待上一轮 DMA 完成

游戏模式下 `lv_port_disp_suspend()` 暂停 LVGL 输出，游戏 runtime 独占 SPI 总线。

---

## 快速开始

### 环境要求

- ESP-IDF v5.1+（推荐 v5.1.2）
- ESP32-S3 目标芯片（`idf.py set-target esp32s3`）

### 步骤

**1. 克隆仓库**

```bash
git clone <repo-url>
cd bot
```

**2. 填写配置文件**

```bash
cp user/asr_config.h.example user/asr_config.h
cp user/model_config.h.example user/model_config.h
```

编辑 `user/asr_config.h` 填入百度 AI 平台的 API Key 和 Secret Key。
编辑 `user/model_config.h` 填入 LLM API Key、接口 URL 和模型名称。

**3. 放入 Game Boy ROM（可选）**

将 .gb / .gbc 文件放入 `spiffs_image/roms/` 目录，构建时自动打包到 SPIFFS 分区。

**4. 编译、烧录**

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COM5 -b 2000000 flash
```

---

## 配置说明

### sdkconfig.defaults.esp32s3（已预置）

| 配置项 | 值 | 说明 |
|--------|-----|------|
| CPU 频率 | 240 MHz | 满速运行 |
| Flash 模式 | QIO | 代码取指速度约为 DIO 的 2 倍 |
| Flash 频率 | 80 MHz | flash 时钟 |
| Flash 大小 | 16 MB | N16R8 板载 |
| PSRAM | Octal 80MHz | 8MB 八线 PSRAM |
| 编译优化 | -O2 | 性能优化 |
| FreeRTOS HZ | 1000 | 1ms tick 精度 |
| mbedTLS 动态缓冲 | 启用 | SSL 内存按需分配，释放内部 DRAM |
| BSS 段外部 | 启用 | 零初始化数据放 PSRAM |

### 分区表（partitions.csv）

| 分区 | 大小 | 用途 |
|------|------|------|
| app0 | 6 MB | OTA 分区 0 |
| app1 | 6 MB | OTA 分区 1 |
| spiffs | 4 MB | 文件系统（ROM、字体等） |
| nvs | 20 KB | WiFi 凭证等持久数据 |

### user/asr_config.h

| 宏 | 说明 |
|----|------|
| `BAIDU_API_KEY` | 百度 AI 应用 API Key |
| `BAIDU_SECRET_KEY` | 百度 AI 应用 Secret Key |
| `MIC_SCK_PIN` | INMP441 BCLK（默认 GPIO38）|
| `MIC_WS_PIN` | INMP441 LRCK（默认 GPIO39）|
| `MIC_SD_PIN` | INMP441 DATA（默认 GPIO40）|
| `MIC_SAMPLE_RATE` | 采样率（默认 16000 Hz）|
| `MIC_MAX_SECONDS` | 最大录音时长（默认 10 秒）|

### user/model_config.h

| 宏 | 说明 |
|----|------|
| `MODEL_API_KEY` | LLM 服务 API Key |
| `MODEL_API_URL` | 接口地址（OpenAI 兼容）|
| `MODEL_NAME` | 模型名称（如 `deepseek-chat`）|

---

## 注意事项

- **凭证安全**：`asr_config.h` 和 `model_config.h` 含 API Key，已加入 `.gitignore`
- **PSRAM 必须选 Octal Mode**：录音缓冲、ROM 数据、HTTP 响应均分配于 PSRAM
- **I2S 资源分配**：I2S_NUM_0 = 麦克风（RX），I2S_NUM_1 = 扬声器（TX），互不干扰
- **LCD SPI 时钟**：80 MHz，PARTIAL 模式异步 DMA 刷新
- **WiFi + mbedTLS**：运行时需约 100 KB 内部堆，`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=16384` 预留空间
- **编码器导航**：旋转 = 移动焦点，按键 = 确认。弹窗弹出时焦点自动切到弹窗 group

---

## 内存使用（ESP32-S3 N16R8）

### Flash（16 MB）

| 内容 | 大小 |
|------|------|
| 固件（ESP-IDF + LVGL + 应用 + 游戏模拟器）| ~4 MB |
| SPIFFS（ROM 存储）| 4 MB |
| OTA 备份分区 | 6 MB |
| 引导 + NVS + 其他 | ~0.5 MB |

### PSRAM（8 MB Octal）

| 用途 | 大小 |
|------|------|
| ASR 录音缓冲区 | ~320 KB |
| HTTP 响应缓冲（动态扩容）| 4-32 KB |
| ROM 数据（游戏运行时）| 32 KB - 2 MB |
| BSS 外部段 | ~12 KB |
| 剩余可用 | **~7 MB** |

### 内部 DRAM（~338 KB 可用段）

| 用途 | 大小 |
|------|------|
| LVGL PARTIAL 双缓冲（34行 x 2）| ~32 KB |
| speaker RingBuffer | 64 KB |
| WiFi / LwIP / mbedTLS 运行时 | ~100 KB |
| FreeRTOS 任务栈 + TCB | ~30 KB |
| 剩余可用 | **~110 KB** |
