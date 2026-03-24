# Bot — ESP32-S3 AI 智能语音助手

基于 ESP-IDF + LVGL 的 ESP32-S3 嵌入式 AI 助手，集成 WiFi、天气查询、AI 聊天、语音识别、语音合成功能，配备 TFT LCD 图形界面与旋转编码器交互。

---

## 目录

- [硬件清单](#硬件清单)
- [引脚接线](#引脚接线)
- [目录结构](#目录结构)
- [功能说明](#功能说明)
- [系统架构](#系统架构)
- [快速开始](#快速开始)
- [配置说明](#配置说明)
- [menuconfig 推荐配置](#menuconfig-推荐配置)
- [调试工具（PC 端脚本）](#调试工具pc-端脚本)
- [注意事项](#注意事项)

---

## 硬件清单

| 组件 | 型号 / 规格 | 说明 |
|------|------------|------|
| 主控 | ESP32-S3-DevKitC-1 N16R8 | 16MB Flash + 8MB Octal PSRAM |
| 屏幕 | 2.4" TFT LCD，240×320，ILI9341 | SPI 接口 |
| 麦克风 | INMP441 | I2S 数字麦克风，I2S_NUM_0 |
| 扬声器 | MAX98357A + 喇叭 | I2S D 类功放，I2S_NUM_1 |
| 输入 | 旋转编码器（A/B/SW）| GPIO 轮询 |

---

## 引脚接线

### LCD（SPI2，ILI9341）

| 功能 | ESP32-S3 引脚 | 说明 |
|------|---------------|------|
| MOSI | GPIO11 | SPI 数据 |
| SCLK | GPIO12 | SPI 时钟，40 MHz |
| RES  | GPIO10 | 复位 |
| DC   | GPIO9  | 数据/命令选择 |
| BLK  | GPIO46 | 背光（高电平开启）|

> CS 引脚在驱动中硬件拉低，不使用软件片选。

### INMP441 麦克风（I2S_NUM_0）

| 功能 | ESP32-S3 引脚 | INMP441 引脚 |
|------|---------------|--------------|
| BCLK | GPIO38 | SCK |
| LRCK | GPIO39 | WS |
| DATA | GPIO40 | SD |
| 3.3V | 3.3V | VDD |
| GND  | GND  | GND、L/R（接地选左声道）|

录音格式：32bit Stereo（I2S Philips 模式），取左声道高16bit，放大3倍后存为 int16，采样率 16000 Hz。

### MAX98357A 功放（I2S_NUM_1）

| 功能 | ESP32-S3 引脚 | MAX98357A 引脚 |
|------|---------------|----------------|
| BCLK | GPIO15 | BCLK |
| LRCK | GPIO16 | LRC |
| DATA | GPIO17 | DIN |
| 3.3V | 3.3V   | SD（常开，不做静音控制）|
| 悬空 | —      | GAIN（默认 9 dB）|

输出格式：16bit Stereo（mono 源复制为左右声道），采样率 16000 Hz。MAX98357A L/R 引脚接地取左声道输出。

### 旋转编码器

| 功能 | ESP32-S3 引脚 |
|------|---------------|
| A    | GPIO4 |
| B    | GPIO5 |
| SW（按键）| GPIO6 |

编码器采用 2ms 轮询（`enc_task`），旋转驱动 LVGL 列表上下导航，按键确认选中项。

---

## 目录结构

```
bot/
├── main/
│   ├── main.c                    # 应用入口：初始化外设、创建 LVGL 任务
│   └── CMakeLists.txt            # 编译入口，列出所有源文件
├── mylvgl/
│   ├── my_demo.c                 # LVGL 主界面逻辑（菜单/天气/聊天/语音助手）
│   ├── my_demo.h
│   ├── lv_port_disp.c            # LVGL 显示驱动（全帧双缓冲 + DMA）
│   ├── lv_port_disp.h
│   ├── lv_port_indev.c           # LVGL 输入设备（旋转编码器）
│   └── lv_port_indev.h
├── user/
│   ├── lcd.c / lcd.h             # ILI9341 SPI LCD 底层驱动（DMA 异步刷新）
│   ├── lcdfont.h                 # ASCII 点阵字库（12/16/24/32px）
│   ├── asr.c / asr.h             # INMP441 I2S 录音 + 百度语音识别 REST API
│   ├── asr_config.h              # 百度 ASR 凭证与引脚定义（已 gitignore）
│   ├── asr_config.h.example      # 配置模板（提交到版本库）
│   ├── model.c / model.h         # LLM 聊天（OpenAI 兼容 REST API）
│   ├── model_config.h            # API Key / URL / 模型名（已 gitignore）
│   ├── model_config.h.example    # 配置模板
│   ├── speaker.c / speaker.h     # MAX98357A I2S 音频输出驱动 + 播放队列
│   ├── tts.c / tts.h             # 百度语音合成 REST API（流式 PCM 送扬声器）
│   ├── weather.c / weather.h     # 天气 HTTP 查询与解析
│   ├── wifi.c / wifi.h           # WiFi 连接管理（station 模式）
│   ├── sntp_time.c / sntp_time.h # SNTP 网络时间同步
│   ├── usb_audio.c / usb_audio.h # USB CDC 音频输出（调试用，发送 PCM 到 PC）
│   └── lv_font_simhei_16.c       # 思黑体 16px LVGL 中文字体
├── lvgl/                         # LVGL 9.x 源码（子模块）
├── lv_conf.h                     # LVGL 配置（色深、缓冲区、字体等）
├── partitions.csv                # 自定义分区表
├── CMakeLists.txt                # 顶层 CMake
├── recv_audio.py                 # PC 端：从 USB CDC 接收 PCM 保存为 WAV
├── speaker_audio.py              # PC 端：发送 WAV 到 ESP32 USB CDC 播放（调试）
├── gen_chars.py                  # 工具：提取文本用字字符集
├── gen_gb2312.py                 # 工具：生成 GB2312 字体 C 文件
└── gen_main.py                   # 工具：字体生成主入口
```

---

## 功能说明

### 1. 主菜单界面

开机后显示功能列表，通过旋转编码器上下选择、按键确认：

- **WiFi 连接** — 后台发起连接，弹窗显示进度，连接成功后自动同步 NTP 时间。
- **天气查询** — HTTP 拉取实时天气，独立天气详情界面展示温度、天气状况等。
- **AI 聊天** — 进入聊天界面，通过屏幕键盘输入文字，调用 LLM API 获取回复，滚动对话记录。
- **语音助手** — 进入语音助手界面，按键触发录音，结束后依次执行 ASR → LLM → TTS 完整语音对话流程。

### 2. 语音助手完整流程

```
用户按键开始录音
    ↓
INMP441 I2S 采集（16kHz 16bit mono，最长10秒，缓存于 PSRAM）
    ↓
百度语音识别 API（pro_api，dev_pid=80001 普通话）
    ↓
识别文字送入 LLM（OpenAI 兼容接口，max_tokens=128，1-2句简短回复）
    ↓
百度语音合成 API（tsn.baidu.com，返回16kHz WAV，流式推送 PCM）
    ↓
MAX98357A 播放合成语音（I2S_NUM_1，spk_tx_task 消费 RingBuffer）
```

状态栏依次显示：「录音中...」→「识别中...」→「思考中...」→「播放中...」→「完成」

### 3. 图形界面（LVGL）

- LVGL 9.x，分辨率 240×320（ILI9341）
- 全帧双缓冲 DMA 刷新，约 30fps
- 中文显示：思黑体 16px
- 弹窗系统：加载弹窗（spinner）+ 结果弹窗，统一管理编码器 group，防止 group 泄漏

### 4. 音频输出（speaker）

- I2S_NUM_1 TX，Philips 标准模式，16kHz 16bit Stereo
- Mono PCM → Stereo 扩展（L/R 填入相同样本），由 `spk_tx_task` 完成
- 64 KB FreeRTOS RingBuffer 作为播放队列，`speaker_play()` 非阻塞写入，`spk_tx_task` 消费并写 I2S DMA

### 5. 语音合成（TTS）

- 调用百度在线语音合成（`POST https://tsn.baidu.com/text2audio`）
- `aue=6` 返回 WAV 封装的 PCM（RIFF 头 44 字节，`tts.c` 自动跳过），之后为 16kHz 16bit mono 裸 PCM
- HTTP 回调中流式推送 PCM 到 `speaker_play()`，缓冲满时 `vTaskDelay(1ms)` 等待消耗，不做大块内存缓存
- Token 与 ASR 共用同一套百度 OAuth2 凭证，各自独立缓存（`s_tts_token` / `s_access_token`）
- TTS 参数：女声（per=0），语速5，音调5，音量9，中文（lan=zh）

### 6. 语音识别（ASR）

- INMP441 工作于 I2S Philips 32bit Stereo 模式
- 数据处理：取左声道（偶数索引），右移16位取高16bit 有效位，放大3倍后截幅至 int16
- 录音缓冲区分配于 PSRAM（`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`），最大 10 秒 ≈ 320 KB
- 上传原始 PCM 到百度 `pro_api`，`Content-Type: audio/pcm;rate=16000`
- 响应 JSON 解析：`err_no=0` 时取 `result[0]` 字段

### 7. AI 聊天（LLM）

- 调用 OpenAI 兼容 REST API，支持任意兼容接口（如 DeepSeek、Qwen 等）
- System prompt 要求1-2句简短回复，不含 emoji，支持中文
- max_tokens=128，非流式（stream=false）
- 模型名、API Key、接口 URL 均在 `model_config.h` 中配置

---

## 系统架构

### 任务结构

| 任务名 | 优先级 | 栈大小 | 说明 |
|--------|--------|--------|------|
| `lv_tick` | 5 | 2048 B | 每 5ms 调用 `lv_tick_inc(5)`，驱动 LVGL 时钟 |
| `lv_task` | 4 | 32768 B | 每 5ms 调用 `lv_timer_handler()`，执行 UI 渲染与 flush |
| `enc_task` | 6 | 2048 B | 每 2ms 轮询编码器 GPIO，产生 LVGL 编码器事件 |
| `spk_tx` | 3 | 2048 B | 从播放 RingBuffer 读 PCM，扩展为 Stereo，写 I2S DMA |
| `usb_tx` | 3 | 2048 B | 从 USB RingBuffer 读数据，通过 CDC 发送到 PC |
| `wifi_task` | 3 | 4096 B | WiFi 连接 + NTP 同步（一次性，完成后退出）|
| `weather_task` | 3 | 16384 B | HTTP 天气查询（一次性）|
| `chat_task` | 3 | 16384 B | LLM API 请求（一次性）|
| `asr_task` | 3 | 16384 B | 百度 ASR 识别请求（一次性）|
| `asr_llm` | 3 | 16384 B | 语音助手：ASR → LLM → TTS 串行执行（一次性）|

### 线程安全策略

所有 LVGL 对象操作均在 `lv_task` 单线程中执行。后台任务通过 **FreeRTOS Queue** 传递结果，`lv_timer` 回调在 LVGL 线程内轮询队列并更新 UI，严格避免跨任务直接操作 LVGL 对象。

`speaker_play()` 使用 `xRingbufferSend(..., timeout=0)` 非阻塞写入，天然线程安全，TTS HTTP 回调可直接调用。

### LCD 刷新机制

全帧双缓冲 + DMA 非阻塞刷新流程：

1. LVGL 渲染完整帧到 `buf1`（PSRAM，240×320×2 = 153600 字节）
2. `disp_flush` 回调通过 `LCD_Send_Buf_Async` 提交 DMA，分块发送（每块 ≤ 4092 字节）
3. 最后一块异步入队后立即调用 `lv_display_flush_ready()`，LVGL 可立即渲染下一帧到 `buf2`
4. 下次 flush 前 `LCD_Send_Buf_Wait` 阻塞等待上帧 DMA 完成
5. SPI 总线通过 Mutex 保护，防止多任务并发访问

### 弹窗与 Group 管理

统一辅助函数：

| 函数 | 说明 |
|------|------|
| `create_loading_dialog(msg, cancel_cb)` | 创建加载弹窗（spinner + 取消按钮），自动绑定编码器 group |
| `create_result_dialog(msg, ok_cb)` | 创建结果弹窗（文字 + OK 按钮），自动释放 group |
| `safe_delete_loading_dialog(cont)` | 安全删除弹窗，防二次触发，守卫 `lv_obj_is_valid()` |
| `indev_set_group(group)` | 统一切换编码器绑定 group |

每个弹窗通过 `lv_obj_add_event_cb(..., LV_EVENT_DELETE, group_delete_cb)` 在删除时自动释放 group，彻底防止 group 泄漏。

---

## 快速开始

### 环境要求

- ESP-IDF v5.x（推荐 v5.3 及以上）
- Python 3.x（用于 PC 端调试脚本）

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

编辑 `user/asr_config.h`，填入百度 AI 开放平台的 API Key 和 Secret Key。
编辑 `user/model_config.h`，填入 LLM API Key、接口 URL 和模型名称。

**3. 配置 menuconfig（首次）**

```bash
idf.py menuconfig
```

按下方「menuconfig 推荐配置」表格逐项设置后保存退出。

**4. 编译、烧录、监视**

```bash
idf.py build
idf.py flash monitor
```

---

## 配置说明

### user/asr_config.h

| 宏 | 说明 |
|----|------|
| `BAIDU_API_KEY` | 百度 AI 应用 API Key |
| `BAIDU_SECRET_KEY` | 百度 AI 应用 Secret Key |
| `BAIDU_TOKEN_URL` | OAuth2 token 地址（固定 `https://aip.baidubce.com/oauth/2.0/token`）|
| `BAIDU_ASR_URL` | ASR 接口地址（`https://vop.baidu.com/pro_api`）|
| `MIC_SCK_PIN` | INMP441 BCLK 引脚（默认 GPIO38）|
| `MIC_WS_PIN` | INMP441 LRCK 引脚（默认 GPIO39）|
| `MIC_SD_PIN` | INMP441 DATA 引脚（默认 GPIO40）|
| `MIC_SAMPLE_RATE` | 采样率（默认 16000 Hz）|
| `MIC_MAX_SECONDS` | 最大录音时长（默认 10 秒）|

### user/model_config.h

| 宏 | 说明 |
|----|------|
| `MODEL_API_KEY` | LLM 服务 API Key |
| `MODEL_API_URL` | 接口地址（OpenAI 兼容，如 `https://api.deepseek.com/v1/chat/completions`）|
| `MODEL_NAME` | 模型名称（如 `deepseek-chat`）|

> 这两个文件已加入 `.gitignore`，不会被提交到版本库。

---

## menuconfig 推荐配置

| 配置项 | 推荐值 | 路径 |
|--------|--------|------|
| PSRAM 模式 | Octal Mode | Component config → ESP PSRAM → SPIRAM mode |
| FreeRTOS Tick Rate | 1000 Hz | Component config → FreeRTOS → configTICK_RATE_HZ |
| 内部堆保留给 PSRAM | 65536 字节 | Component config → ESP PSRAM → Reserve internal memory |
| mbedTLS 内存分配 | Default（跟随系统 malloc）| Component config → mbedTLS → Memory allocation strategy |
| USB CDC | 启用 TinyUSB CDC ACM | Component config → TinyUSB → CDC |

> PSRAM 必须选 Octal Mode，否则录音缓冲区（320 KB）无法分配，导致启动时报 `录音缓冲区分配失败`。

---

## 调试工具（PC 端脚本）

### recv_audio.py — 接收麦克风录音

从 ESP32 USB CDC 实时接收 PCM 数据，保存为 WAV 文件，用于验证麦克风采集是否正常。

```bash
pip install pyserial
python recv_audio.py                      # 交互式选择串口，录制 10 秒
python recv_audio.py COM3 5 mic.wav       # 指定串口、时长、文件名
```

输出文件：16000 Hz / 16bit / Mono WAV，可用 Audacity 等软件验证波形。

### speaker_audio.py — 发送音频播放

将 WAV 文件通过 USB CDC 发送给 ESP32，由 MAX98357A 播放，用于验证扬声器驱动是否正常。

```bash
pip install pyserial numpy
# 可选：pip install scipy  （更高质量重采样）
python speaker_audio.py                        # 交互式选择串口和文件
python speaker_audio.py --port COM3 --file audio.wav
```

支持任意采样率/位深/声道数 WAV 文件，自动转换为 16kHz 16bit mono 后发送。

---

## 注意事项

- **凭证安全**：`asr_config.h` 和 `model_config.h` 含 API Key，已加入 `.gitignore`，请勿手动 `git add` 这两个文件。
- **PSRAM 必须启用 Octal Mode**：录音缓冲区（最大 320 KB）、LVGL 帧缓冲（双缓冲 ~300 KB）均分配于 PSRAM，Quad Mode 带宽不足。
- **I2S 资源分配**：I2S_NUM_0 用于麦克风（RX），I2S_NUM_1 用于扬声器（TX），两者相互独立，录音与播放可并行。
- **TTS 内存优化**：`tts.c` 中 `encoded[4096]`、`body[5120]`、`url[320]` 均声明为 `static`，避免在任务栈上分配大数组导致栈溢出（节省约 9.3 KB 栈空间）。
- **WAV 头处理**：百度 TTS `aue=6` 实际返回 RIFF WAV 格式（非裸 PCM），`tts.c` 会自动跳过前 44 字节头后再送入播放器。
- **LCD SPI 时钟**：40 MHz，全帧（240×320）DMA 异步刷新，CPU 无需等待传输完成。
- **WiFi + mbedTLS 内存**：运行时需额外约 100 KB 内部堆，必须在 menuconfig 中预留足够内部内存（`Reserve internal memory ≥ 65536`）。
- **编码器导航**：旋转 = 上下移动焦点，按键 = 确认。弹窗弹出时焦点自动切换到弹窗 group，关闭后恢复主菜单 group。