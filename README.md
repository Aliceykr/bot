# Bot — ESP32-S3 AI 智能语音助手 & Game Boy 模拟器

基于 ESP-IDF + LVGL 的 ESP32-S3 嵌入式项目，集成 WiFi、天气查询、AI 聊天、在线/离线语音识别、语音合成、Game Boy 游戏模拟器（含音频）、BLE 蓝牙配网，配备 TFT LCD 图形界面与旋转编码器 + 按键矩阵交互。

---

## 硬件清单

| 组件 | 型号 / 规格 | 说明 |
|------|------------|------|
| 主控 | ESP32-S3-DevKitC-1 N16R8 | 16MB Flash + 8MB Octal PSRAM |
| 屏幕 | 2.4" TFT LCD，240x320，ILI9341 | SPI 接口，80 MHz |
| 麦克风 | INMP441 | I2S 数字麦克风，I2S_NUM_0 |
| 扬声器 | MAX98357A + 喇叭 | I2S D 类功放，I2S_NUM_1 |
| 输入 | 旋转编码器（A/B/SW）| PCNT 硬件计数 + GPIO 轮询 |
| 按键 | 3x3 矩阵键盘（9 键）| 游戏方向键 + A/B/START/SELECT/EXIT |

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

### 3x3 按键矩阵

| 行/列 | GPIO41 (C0) | GPIO42 (C1) | GPIO47 (C2) |
|--------|-------------|-------------|-------------|
| GPIO1 (R0)  | B | UP | A |
| GPIO2 (R1)  | LEFT | EXIT | RIGHT |
| GPIO14 (R2) | SELECT | DOWN | START |

2ms 扫描周期，两次连续一致读取才确认。游戏模式切换：`keypad_set_game_mode(true)` 启用按键输出。

---

## 目录结构

```
bot/
├── main/
│   ├── main.c                    # 应用入口：初始化外设、创建 LVGL 任务
│   ├── psram_task.c / .h         # PSRAM 栈任务创建工具（含自动回收 cleaner）
│   ├── CMakeLists.txt            # 编译入口，列出所有源文件
│   └── idf_component.yml        # IDF 组件依赖（esp-sr）
├── mylvgl/
│   ├── my_demo.c                 # LVGL 主界面逻辑（菜单/天气/聊天/语音/游戏/BLE）
│   ├── my_demo.h
│   ├── lv_port_disp.c / .h       # LVGL 显示驱动（PARTIAL 双缓冲 + 异步 DMA）
│   ├── lv_port_indev.c / .h      # LVGL 输入设备（PCNT 编码器）
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
│   ├── wifi.c / wifi.h           # WiFi STA 连接 + 守护任务（断线重连）+ mutex 线程安全
│   ├── sntp_time.c / sntp_time.h # SNTP 网络时间同步（阿里云 NTP）
│   ├── health.c / health.h       # 堆内存健康监控（60s 周期打印水位）
│   ├── esp_sr.c / esp_sr.h       # ESP-SR 离线中文命令词识别（AFE + MultiNet7）
│   ├── ble_prov.c / ble_prov.h   # NimBLE BLE 配网（HM-10 兼容 0xFFE0/0xFFE1 + mutex 线程安全）
│   ├── keypad.c / keypad.h       # 3x3 矩阵键盘扫描（游戏控制）
│   └── lv_font_simhei_16.c       # 思黑体 16px LVGL 中文字体
├── game/
│   ├── gb_emu.c / gb_emu.h       # Game Boy 模拟器集成（Walnut-CGB / Peanut-GB）
│   ├── gb_audio.c / gb_audio.h   # GB 音频：MiniGB APU → stereo→mono → speaker
│   ├── minigb_apu.c              # MiniGB APU 音频处理单元（第三方）
│   ├── game_runtime.c / .h       # 游戏生命周期管理（加载/运行/退出）
│   ├── rom_loader.c / .h         # SPIFFS ROM 扫描与加载
│   ├── walnut_cgb.h              # Walnut-CGB 模拟器核心（高性能，32位路径）
│   ├── peanut_gb.h               # Peanut-GB 模拟器核心（原始 8 位，备用）
│   └── CMakeLists.txt
├── spiffs_image/
│   └── roms/                     # 放置 .gb / .gbc ROM 文件
├── lvgl/                         # LVGL 9.x 源码
├── lv_conf.h                     # LVGL 配置
├── partitions.csv                # 自定义分区表（6MB+5.5MB OTA + SPIFFS + model）
├── sdkconfig.defaults            # 默认配置（含 ESP32-S3 / BLE / ESP-SR / WiFi 优化）
└── CMakeLists.txt                # 顶层 CMake
```

---

## 功能说明

### 1. 主菜单界面

开机后显示功能列表，通过旋转编码器上下选择、按键确认：

- **WiFi 连接** — 后台连接 WiFi，弹窗显示进度，成功后自动同步 NTP 时间
- **天气与日期** — HTTP 拉取实时天气，展示温度、湿度、风力、实时时钟
- **游戏** — 扫描 SPIFFS 中的 .gb/.gbc ROM 文件，选择运行 Game Boy 模拟器
- **聊天助手** — 屏幕键盘输入文字，调用 LLM API 获取回复，滚动对话记录
- **语音助手** — 在线流程：录音 → 百度 ASR 识别 → LLM 回复 → 百度 TTS 合成播放
- **语音命令** — 离线 ESP-SR 中文命令词识别（按钮触发，无需唤醒词）
- **蓝牙** — 开启 BLE 广播，手机发送 "SSID_xxx password_xxx" 进行 WiFi 配网

### 2. Game Boy 模拟器

- 基于 Walnut-CGB（Peanut-GB 高性能重写版），支持 DMG + CGB 游戏
- 双取指链式架构 + 32 位 DMA 路径，专为 ESP32-S3 等 32 位 MCU 优化
- GB 原生分辨率 160x144，1.5 倍缩放到 240x216
- SPI 80MHz 异步 DMA 双行缓冲渲染
- 小 ROM（<=256KB）自动复制到 DRAM 加速
- 3x3 按键矩阵提供完整的 GB 控制输入（A/B/方向/START/SELECT/EXIT）
- 进入游戏自动暂停 WiFi 和 BLE，退出后恢复进入前活跃的服务

### 3. Game Boy 音频

- MiniGB APU 模拟全部 4 个 GB 声道（方波x2 + 波形 + 噪声）
- 每帧合成 ~268 个 stereo 样本（16kHz），合并为 mono 推入 speaker RingBuffer
- APU 在独立任务/Core 0 运行，主仿真在 Core 1，真正并行不占帧预算
- 与游戏模拟器同步运行，非阻塞输出（缓冲满时丢帧保仿真帧率）

### 4. 语音助手（在线）

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

### 5. 语音命令（离线 ESP-SR）

使用 ESP-SR 的 MultiNet7 中文离线命令词识别，无需网络：

- 按键触发识别（无需唤醒词），AFE 噪声抑制 + VAD
- 预定义 11 个中文命令：返回、确认、取消、连接网络、查看天气、打开游戏、打开聊天、语音助手、退出游戏、调大音量、调小音量
- I2S_NUM_0 与在线 ASR 共享，通过 deinit/reinit 切换

### 6. BLE 蓝牙配网

基于 NimBLE（比 Bluedroid 节省 ~40KB DRAM）的 HM-10 兼容配网功能：

- 手机 BLE 扫描连接 "ESP32-Bot"（Service 0xFFE0, Characteristic 0xFFE1）
- 发送 "SSID_名称 password_密码" 即可自动连接 WiFi
- 50ms 空闲超时自动拼包，配网结果通过 BLE Notify 返回手机
- 非阻塞启动：`ble_prov_start` 不轮询等 host sync，由 `on_sync` 回调异步触发广播
- 收到凭据后自动 deinit BLE 释放 DRAM，再启动 WiFi 连接
- 游戏时自动暂停 BLE，退出后恢复
- 所有共享状态（s_active / s_status_cb / RX 缓冲）由模块级 mutex 保护，防并发竞态
- deinit 使用 `s_deinit_in_progress` 门锁防双拆，controller 拆卸重试+超时兜底

### 7. WiFi 守护

- 首次连接：EventGroup 等待结果，最多重试 3 次
- 运行期断线：常驻守护任务自动重连，指数退避（5s → 5min）
- 用户主动断开：停止守护，不自动重连
- 支持动态凭据设置（BLE 配网写入）
- 所有状态变量由 mutex 保护，确保事件回调、守护任务、公开 API 之间的线程安全

---

## 系统架构

### 任务结构

| 任务名 | 优先级 | 栈大小 | 说明 |
|--------|--------|--------|------|
| `lv_tick` | 5 | 2048 B | LVGL 时钟（5ms tick） |
| `lv_task` | 4 | 16384 B (DRAM) | LVGL 渲染 + flush |
| `enc_task` | 6 | 2048 B | SW 按键状态机（5ms 轮询） |
| `spk_tx` | 3 | 4096 B | RingBuffer → I2S DMA 播放 |
| `asr_rec` | 5 | 2048 B | I2S 录音常驻任务 |
| `wifi_task` | 3 | 6144 B (PSRAM) | WiFi 连接 + NTP 同步（一次性） |
| `wifi_guard` | 4 | 3072 B | 断线重连守护（常驻） |
| `weather_task` | 3 | 16384 B (PSRAM) | 天气查询（一次性） |
| `chat_task` | 3 | 16384 B (PSRAM) | LLM 请求（一次性） |
| `asr_task` | 3 | 16384 B (PSRAM) | ASR 识别（一次性） |
| `asr_llm` | 3 | 16384 B (PSRAM) | ASR → LLM → TTS（一次性） |
| `game_run` | 10 | 12288 B (DRAM) | GB 模拟器主循环（Core 1） |
| `apu_task` | 5 | 4096 B | GB APU 音频合成（Core 0） |
| `psram_cleaner` | 2 | 3072 B | 回收 PSRAM 任务栈和 TCB |
| `esp_sr_feed` | 6 | 4096 B | ESP-SR AFE 音频喂入（Core 0） |
| `esp_sr_detect` | 5 | 8192 B (估计) | ESP-SR MultiNet 命令检测（Core 1） |
| `kpad_task` | 6 | 4096 B | 按键矩阵扫描（2ms 周期） |
| `health` | 1 | 2048 B | 堆内存监控（60s 周期） |

### PSRAM 任务创建

`psram_task.c` 提供 `xTaskCreatePSRAM()` 和 `xTaskCreatePSRAMPinnedToCore()`，将任务栈分配到 PSRAM（节省内部 DRAM），TCB 留在内部 DRAM（FreeRTOS 要求）。任务函数正常 `return` 后，后台 cleaner 任务自动回收栈和 TCB 内存。

### 线程安全

- LVGL 对象操作均在 `lv_task` 单线程执行
- 后台任务通过 FreeRTOS Queue 传递结果，`lv_timer` 回调轮询队列更新 UI
- `speaker_play()` 使用 RingBuffer 非阻塞写入，TTS/GB 音频可直接调用
- WiFi 模块状态变量（status / user_stopped / ip_str）由 mutex 保护，事件回调、守护任务、公开 API 之间安全并发
- BLE 模块核心状态（s_active / s_status_cb / RX 缓冲 / deinit 门锁）由模块级 mutex 保护，NimBLE host 任务、timer service、worker、LVGL 任务之间安全并发
- HTTP 响应缓冲由各模块 mutex 保护
- 录音任务启停使用 `ulTaskNotifyTake` 确认同步，避免 I2S 时序冲突

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

- ESP-IDF v5.4+（推荐 v5.4.3）
- ESP32-S3 目标芯片

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

编辑 `user/asr_config.h` 填入百度 AI 平台 API Key 和 Secret Key。
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

### sdkconfig.defaults（已预置）

| 配置项 | 值 | 说明 |
|--------|-----|------|
| CPU 频率 | 240 MHz | 满速运行 |
| Flash 模式 | QIO | 代码取指约为 DIO 的 2 倍 |
| Flash 频率 | 80 MHz | flash 时钟 |
| Flash 大小 | 16 MB | N16R8 板载 |
| PSRAM | Octal 80MHz | 8MB 八线 PSRAM |
| SPIRAM_RODATA | 启用 | 只读常量放 PSRAM，释放 ~80KB 内部 DRAM |
| SPIRAM_TRY_ALLOCATE_WIFI_LWIP | 启用 | WiFi/LwIP 缓冲从 PSRAM 分配，为 BLE 共存腾 DRAM |
| 编译优化 | -Os | 体积优化，节省 ~10-20% 代码空间 |
| mbedTLS 动态缓冲 | 启用 | SSL 内存按需分配 |
| mbedTLS 外部内存 | 启用 | SSL 从 PSRAM 分配，减少内部 DRAM 碎片 |
| FreeRTOS HZ | 1000 | 1ms tick 精度 |
| Bluetooth LE | NimBLE | 轻量 BLE 栈（~40KB DRAM，比 Bluedroid 省），仅 Peripheral 角色 |
| BT/WiFi 共存 | 启用 | BLE 和 WiFi 同时运行时的软件共存调度 |
| WiFi 静态 RX 缓冲 | 4（默认 10）| 减少 DRAM 连续占用，动态缓冲从 PSRAM 分配 |
| ESP-SR | MultiNet7 + NSNET2 + VADNet | 离线中文命令词识别 + 噪声抑制 + 语音活动检测 |

### 分区表（partitions.csv）

| 分区 | 大小 | 用途 |
|------|------|------|
| app0 | 6 MB | OTA 分区 0 |
| app1 | 5.5 MB | OTA 分区 1 |
| storage | ~1.5 MB | SPIFFS 文件系统（ROM 等） |
| model | 3 MB | ESP-SR 模型数据 |
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
- **I2S_NUM_0 共享**：在线 ASR 和离线 ESP-SR 通过 deinit/reinit 共享 I2S_NUM_0
- **LCD SPI 时钟**：80 MHz，PARTIAL 模式异步 DMA 刷新
- **编码器导航**：旋转 = 移动焦点，按键 = 确认。弹窗弹出时焦点自动切到弹窗 group
- **ESP32-S3 蓝牙限制**：仅支持 BLE，不支持 Classic BT（A2DP 不可用）
- **游戏模式**：进入游戏自动暂停 WiFi + BLE，退出后仅恢复进入前活跃的服务
- **BLE 按需加载**：蓝牙关闭时 NimBLE 栈完全释放（deinit），归还 ~40-50KB DRAM 给其他模块
- **任务栈分配**：涉及 SPIFFS/flash IO 的任务（LVGL、游戏）使用内部 DRAM 栈；纯 HTTP/cJSON 任务使用 PSRAM 栈
- **WiFi 缓冲优化**：静态 RX 缓冲从默认 10 降至 4（-10KB DRAM），动态缓冲从 PSRAM 分配

---

## 代码统计

| 类别 | 行数 |
|------|------|
| 纯手写代码 | ~6,200 行 |
| 字体数据（lv_font_simhei_16 + lcdfont）| ~384,000 行 |
| 模拟器库（walnut_cgb.h + minigb_apu.c）| ~10,500 行 |
| LVGL 库 | 未计入 |

---

## 内存使用（ESP32-S3 N16R8）

### Flash（16 MB）

| 内容 | 大小 |
|------|------|
| 固件（ESP-IDF + LVGL + 应用 + 游戏模拟器 + ESP-SR + BLE）| ~4.6 MB |
| SPIFFS（ROM 存储）| ~1.5 MB |
| ESP-SR 模型分区 | 3 MB |
| OTA 备份分区 | 5.5 MB |

### PSRAM（8 MB Octal）

| 用途 | 大小 |
|------|------|
| ASR 录音缓冲区 | ~320 KB |
| HTTP 响应缓冲（动态扩容）| 4-32 KB |
| ROM 数据（游戏运行时）| 32 KB - 2 MB |
| BSS + rodata 外部段 | ~100 KB |
| 剩余可用 | **~7 MB** |

### 内部 DRAM（~338 KB 可用段）

| 用途 | 大小 |
|------|------|
| LVGL PARTIAL 双缓冲（34行 x 2）| ~32 KB |
| speaker RingBuffer | 64 KB |
| WiFi / LwIP / mbedTLS 运行时 | ~100 KB |
| NimBLE 栈运行时（按需加载）| ~40 KB |
| FreeRTOS 任务栈 + TCB | ~30 KB |
| 剩余可用 | **~110 KB** |
