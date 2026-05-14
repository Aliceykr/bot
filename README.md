# Bot — ESP32-S3 AI 智能语音助手 & Game Boy 模拟器

基于 ESP-IDF + LVGL 的 ESP32-S3 嵌入式项目，集成 WiFi、天气查询、AI 聊天、在线/离线语音识别、语音合成、Game Boy 游戏模拟器（含音频）、BLE 蓝牙配网与远程音乐控制、MicroSD 卡音乐播放器（WAV + MP3）、音量控制、巴法云智能设备控制，配备 TFT LCD 图形界面与旋转编码器 + 按键矩阵交互。

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
| 存储 | MicroSD 卡（SPI 模式）| SPI3_HOST，FAT32 文件系统 |

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

### MPU6050 六轴传感器（I2C_NUM_0）

| 功能 | ESP32-S3 引脚 | MPU6050 引脚 |
|------|---------------|--------------|
| SDA  | GPIO13 | SDA |
| SCL  | GPIO7  | SCL |
| 3.3V | 3.3V   | VCC |
| GND  | GND    | GND、AD0（地址 0x68）|

400kHz I2C，±2g 量程，DLPF 44Hz。仅在进入 2048 游戏时初始化，退出后释放总线。倾斜阈值 0.30g，X/Y 主导轴判定方向，平放时无输入。

### MicroSD 卡（SPI3_HOST）

| 功能 | ESP32-S3 引脚 | SD 卡模块引脚 |
|------|---------------|--------------|
| CS   | GPIO0  | CS |
| MOSI | GPIO8  | DI (CMD) |
| SCK  | GPIO18 | CLK |
| MISO | GPIO21 | DO (DAT0) |
| VCC  | 3.3V   | VCC |
| GND  | GND    | GND |

使用 SPI3_HOST（LCD 占 SPI2_HOST），FAT32 文件系统，挂载点 `/sdcard`。

> 注意：CS 使用 GPIO0（Boot 按键脚），上电时不要按住即可。原 GPIO48 因板载 WS2812 LED 干扰 CS 信号而弃用。

---

## 目录结构

```
bot/
├── main/
│   ├── main.c                    # 应用入口：初始化外设、创建 LVGL 任务
│   ├── psram_task.c / .h         # PSRAM 栈任务创建工具（含自动回收 cleaner）
│   ├── CMakeLists.txt            # 编译入口，列出所有源文件
│   └── idf_component.yml        # IDF 组件依赖（esp-sr、helix-mp3）
├── mylvgl/
│   ├── my_demo.c                 # LVGL 主界面逻辑（菜单/天气/聊天/语音/游戏/BLE/音乐/音量/智能设备）
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
│   ├── speaker.c / speaker.h     # MAX98357A I2S 音频输出 + DC-block HPF + 音量 + 动态采样率
│   ├── tts.c / tts.h             # 百度语音合成 API（流式 PCM 播放）
│   ├── weather.c / weather.h     # 天气 HTTP 查询与解析
│   ├── bemfa.c / bemfa.h         # 巴法云智能设备 HTTP REST 控制（拉取列表 / 推送 on/off）
│   ├── bemfa_config.h            # 巴法云私钥（已 gitignore）
│   ├── bemfa_config.h.example    # 配置模板
│   ├── wifi.c / wifi.h           # WiFi STA 连接 + 守护任务 + mutex 线程安全
│   ├── sntp_time.c / sntp_time.h # SNTP 网络时间同步（阿里云 NTP）
│   ├── health.c / health.h       # 堆内存健康监控（60s 周期打印水位）
│   ├── esp_sr.c / esp_sr.h       # ESP-SR 离线中文命令词识别（AFE + MultiNet7）
│   ├── ble_prov.c / ble_prov.h   # NimBLE BLE 配网（HM-10 兼容 + mutex 线程安全）
│   ├── music.c / music.h         # SD 卡音乐播放器（WAV + MP3/helix 解码）
│   ├── keypad.c / keypad.h       # 3x3 矩阵键盘扫描（游戏控制）
│   ├── sdcard.c / sdcard.h       # MicroSD 卡 SPI 模式驱动（FATFS 挂载/卸载）
│   └── lv_font_simhei_16.c       # 思黑体 16px LVGL 中文字体
├── game/
│   ├── gb_emu.c / gb_emu.h       # Game Boy 模拟器集成（Walnut-CGB / Peanut-GB）
│   ├── gb_audio.c / gb_audio.h   # GB 音频：MiniGB APU → mono → speaker RingBuffer
│   ├── minigb_apu.c              # MiniGB APU 音频处理单元（第三方）
│   ├── game_2048.c / game_2048.h # 2048 桌面游戏（支持 MPU6050 倾斜控制）
│   ├── mpu6050.c / mpu6050.h     # MPU6050 六轴传感器 I2C 驱动（仅 2048 使用）
│   ├── game_runtime.c / .h       # 游戏生命周期管理（加载/运行/退出）
│   ├── rom_loader.c / .h         # SD 卡 ROM 扫描与加载（原 SPIFFS 已废弃）
│   ├── walnut_cgb.h              # Walnut-CGB 模拟器核心（高性能，32位路径）
│   ├── peanut_gb.h               # Peanut-GB 模拟器核心（原始 8 位，备用）
│   └── CMakeLists.txt
├── lvgl/                         # LVGL 9.x 源码
├── lv_conf.h                     # LVGL 配置
├── partitions.csv                # 自定义分区表（6.5MB 双 OTA + ESP-SR 模型）
├── sdkconfig.defaults            # 默认配置（含 ESP32-S3 / BLE / ESP-SR / WiFi / FATFS）
└── CMakeLists.txt                # 顶层 CMake（UTF-8 编码）
```

---

## 功能说明

### 1. 主菜单界面

开机后显示功能列表，通过旋转编码器上下选择、按键确认：

- **环境监测** — 占位（开发中）
- **天气与日期** — HTTP 拉取实时天气，展示温度、湿度、风力、实时时钟
- **游戏** — 内置 2048 游戏（无需 SD 卡）+ 扫描 SD 卡 `/sdcard/rom/` 中的 .gb/.gbc ROM 文件运行 Game Boy 模拟器
- **聊天助手** — 屏幕键盘输入文字，调用 LLM API 获取回复，滚动对话记录
- **语音助手** — 在线流程：录音 → 百度 ASR 识别 → LLM 回复 → 百度 TTS 合成播放
- **语音命令** — 离线 ESP-SR 中文命令词识别（按钮触发，无需唤醒词）
- **蓝牙** — 开启 BLE 广播，手机发送 "SSID_xxx password_xxx" 配网后自动连接 WiFi；支持 BLE 远程音乐控制（`/music on` 列歌、`/序号` 播放、`/music off` 停止）
- **音乐** — 扫描 SD 卡 `/sdcard/music/` 下的 WAV / MP3 文件，选择播放（支持暂停/切歌）
- **音量** — 滑块调节音量（0-100%），对数增益曲线，NVS 持久化，重启自动恢复
- **智能设备** — 巴法云 TCP 设备云控制，拉取已绑定设备列表，点击按钮 toggle 开关

### 2. Game Boy 模拟器

- 基于 Walnut-CGB（Peanut-GB 高性能重写版），支持 DMG + CGB 游戏
- 双取指链式架构 + 32 位 DMA 路径，专为 ESP32-S3 等 32 位 MCU 优化
- GB 原生分辨率 160x144，1.5 倍缩放到 240x216
- SPI 80MHz 异步 DMA 双行缓冲渲染
- ROM 从 SD 卡加载到 PSRAM（最大 4MB），小 ROM（<=256KB）自动复制到 DRAM 加速
- 内置 2048 游戏：无需 ROM 文件，列表顶部始终可见，即使 SD 卡上无 ROM 也可游玩
- 2048 倾斜控制：MPU6050 加速度计读取设备倾斜方向，自动映射到上下左右四个方向，与按键并存（按键优先，无按键时启用倾斜）
- 3x3 按键矩阵提供完整的 GB 控制输入（A/B/方向/START/SELECT/EXIT）
- 进入游戏自动暂停 WiFi 和 BLE，退出后恢复进入前活跃的服务

### 3. Game Boy 音频

- MiniGB APU 模拟全部 4 个 GB 声道（方波x2 + 波形 + 噪声）
- 每帧合成 ~268 个 mono 样本（16kHz），硬件 I2S MONO 槽输出
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
- 任务生命周期安全：graceful stop + 超时硬杀 + 资源泄漏保护

### 6. BLE 蓝牙配网与远程音乐控制

基于 NimBLE（比 Bluedroid 节省 ~40KB DRAM）的 HM-10 兼容配网功能 + BLE 远程音乐控制：

**配网协议：**
- 手机 BLE 扫描连接 "ESP32-Bot"（Service 0xFFE0, Characteristic 0xFFE1）
- 发送 "SSID_名称 password_密码" 即可自动连接 WiFi
- 50ms 空闲超时自动拼包，配网结果通过 BLE Notify 返回手机
- 收到凭据后自动 deinit BLE 释放 DRAM，再启动 WiFi 连接

**音乐控制协议（`/` 前缀命令）：**
- `/music on` — 扫描 SD 卡音乐目录，逐行 notify 返回歌曲列表（带序号）
- `/music off` — 异步停止后台播放
- `/<序号>` — 按序号播放对应歌曲（推荐，避免中文编码问题）
- `/<歌名>` — 前缀匹配文件名播放

**实现细节：**
- 非阻塞启动：`ble_prov_start` 不轮询等 host sync，由 `on_sync` 回调异步触发广播
- 游戏时自动暂停 BLE，退出后恢复
- 所有共享状态由模块级 mutex 保护，pending 计数器防止 deinit 与 notify 并发竞态
- deinit 使用 `s_deinit_in_progress` 门锁防双拆，controller 拆卸重试+超时兜底
- 音乐 stop 通过独立一次性任务异步执行，避免阻塞 BLE worker（最多 2 秒）
- 歌曲扫描缓冲动态分配到 PSRAM（~4.6KB），命令处理完毕即释放

### 7. WiFi 守护

- 首次连接：EventGroup 等待结果，最多重试 3 次
- 运行期断线：常驻守护任务自动重连，指数退避（5s → 5min）
- 用户主动断开：停止守护，不自动重连
- 支持动态凭据设置（BLE 配网写入），mutex 保护写入与读取并发
- 所有共享状态（s_status / s_user_stopped / s_backoff_idx）由 mutex 保护
- `wifi_copy_ip()` 提供带锁的 IP 快照，避免撕裂读取

### 8. MicroSD 卡存储

通过 SPI3_HOST 以 SPI 模式驱动 MicroSD 卡，挂载 FAT32 文件系统到 `/sdcard`：

- 标准 POSIX API 读写文件（`fopen("/sdcard/xxx", "rb")`）
- 支持 SDSC / SDHC 卡，自动检测容量
- 支持中文长文件名（UTF-8 API + GBK 代码页 936）
- 挂载/卸载安全，可重入
- 与 LCD（SPI2_HOST）互不干扰
- 存放 Game Boy ROM（`/sdcard/rom/`）和音乐文件（`/sdcard/music/`）
- 游戏列表内置 2048（无需 ROM 文件），始终可见

### 9. 音乐播放器

SD 卡音乐播放，支持 WAV 和 MP3 格式：

- **WAV 解码**：RIFF/PCM 16bit，8k-48k Hz，单/双声道自动降混为 mono
- **MP3 解码**：基于 Helix 定点解码器（~5KB 工作内存，可放 PSRAM），自动跳过 ID3v2 tag
- **动态采样率**：播放前切换 I2S 采样率匹配文件，播完后自动恢复 16kHz（TTS / GB 音频不受影响）
- **播控**：播放/暂停/停止/切歌，后台解码任务，UI 不阻塞
- **切歌安全**：所有 stop/play 操作通过后台任务异步执行，LVGL 线程零阻塞；music_stop 最多等 2 秒轮询确认旧任务退出
- **错误容忍**：MP3 连续 32 帧解码失败自动放弃，防止垃圾数据把解码器推进非法状态
- SD 卡目录：`/sdcard/music/`，支持中英文文件名

### 10. 音频输出

基于 MAX98357A I2S D 类功放：

- 16kHz / 16bit / 硬件 MONO 槽位（无需软件 stereo 展开）
- DMA auto_clear：无数据时自动输出零电平，消除 DC 漂移和爆破声
- DC-block 高通滤波器（截止 ~12.7Hz），消除 MAX98357A 直流偏置底噪
- 字节尾对齐：HTTP chunked 传输奇数字节时保留尾部，防止 16bit 样本错位
- 线性淡入（64 样本 / 4ms）：首次播放或 flush 后恢复时消除爆音
- `speaker_flush()`：清空 RingBuffer 实现立即静音（游戏退出 / 界面切换）
- `speaker_set_sample_rate()`：动态切换 I2S 输出采样率（8k-48k Hz），通过 flush 协议确保旧速率数据播完再切，避免变调
- **音量控制**：Q15 定点对数增益（0-100% → -60dB..0dB），volatile 原子写入无需持锁，NVS 持久化（500ms 防抖合并 slider 拖动写入）

### 11. 智能设备（巴法云）

通过巴法云 TCP 设备云 HTTP REST API 控制已绑定的智能设备：

- **设备列表**：GET `/vb/api/v2/allTopic` 拉取所有主题，兼容扁平 / 嵌套两种 JSON 响应结构
- **设备控制**：POST `/va/postJsonMsg` 推送 "on" / "off" 消息，点击按钮自动 toggle
- **线程安全**：模块级 mutex 串行化 HTTPS 请求，避免两个 HTTP client 并发引发 mbedTLS 冲突
- **PSRAM 动态缓冲**：HTTP 响应从 4KB 起步按需 2 倍扩容到最大 32KB，API 返回后立即释放
- **异步 UI**：后台 PSRAM 任务执行 HTTPS 操作，通过 FreeRTOS Queue + lv_timer 轮询更新 UI，LVGL 线程零阻塞
- **屏幕生命周期安全**：mutex + active 标志保护 Queue 句柄，退出屏幕时后台任务检测到 inactive 后丢弃结果而非写入已删除队列

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
| `music` | 4 | 10240 B (DRAM) | SD 卡音乐解码（WAV/MP3）播放（一次性） |
| `music_stop` | 3 | 2048 B (DRAM) | 异步停止音乐播放（一次性） |
| `psram_cleaner` | 2 | 3072 B | 回收 PSRAM 任务栈和 TCB |
| `esp_sr_read` | 6 | 5120 B | ESP-SR I2S 读取 + 16→32bit 转换（Core 0） |
| `esp_sr_feed` | 5 | 5120 B | ESP-SR AFE 音频喂入（Core 0） |
| `esp_sr_detect` | 5 | 6144 B | ESP-SR MultiNet 命令检测（Core 1） |
| `kpad_task` | 6 | 4096 B | 按键矩阵扫描（2ms 周期） |
| `bemfa_list` | 3 | 8192 B (PSRAM) | 巴法云设备列表 HTTPS 请求（一次性） |
| `bemfa_toggle` | 3 | 8192 B (PSRAM) | 巴法云设备 toggle HTTPS 请求（一次性） |
| `ble_mstop` | 3 | 2048 B (DRAM) | BLE 触发的异步音乐停止（一次性） |
| `health` | 1 | 2048 B | 堆内存监控（60s 周期） |

### PSRAM 任务创建

`psram_task.c` 提供 `xTaskCreatePSRAM()` 和 `xTaskCreatePSRAMPinnedToCore()`，将任务栈分配到 PSRAM（节省内部 DRAM），TCB 留在内部 DRAM（FreeRTOS 要求）。任务函数正常 `return` 后，后台 cleaner 任务自动回收栈和 TCB 内存。

### 线程安全

- LVGL 对象操作均在 `lv_task` 单线程执行
- 后台任务通过 FreeRTOS Queue 传递结果，`lv_timer` 回调轮询队列更新 UI
- `speaker_play()` 使用 RingBuffer 非阻塞写入，TTS/GB 音频可直接调用
- HTTP 响应缓冲由各模块 mutex 保护，识别完成后主动释放 PSRAM
- WiFi/BLE/ESP-SR/音乐/巴法云 所有共享状态由各自模块级 mutex 保护
- SPI 总线隔离：LCD 使用 SPI2_HOST，SD 卡使用 SPI3_HOST，互不干扰
- `speaker_set_sample_rate()` 通过 flush 协议与 tx_task 协调，确保 ring 清空后再切换 I2S 时钟
- `speaker_set_volume()` 使用 volatile + Q15 定点乘，无需持锁即可在播放路径上原子生效
- 音乐切歌/停止通过后台一次性任务异步执行，避免阻塞 LVGL 线程

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
- MicroSD 卡（FAT32 格式化，用于存放 ROM 和音乐文件）

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
cp user/bemfa_config.h.example user/bemfa_config.h
```

编辑 `user/asr_config.h` 填入百度 AI 平台 API Key 和 Secret Key。
编辑 `user/model_config.h` 填入 LLM API Key、接口 URL 和模型名称。
编辑 `user/bemfa_config.h` 填入巴法云私钥（注册 cloud.bemfa.com → 个人中心 → 复制"私钥"）。

**3. 准备 MicroSD 卡**

将 SD 卡格式化为 FAT32，创建以下目录结构：

```
/sdcard/
├── rom/          # 放置 .gb / .gbc Game Boy ROM 文件
└── music/        # 放置 .wav / .mp3 音乐文件（16bit PCM WAV 或 MP3）
```

> 注意：实际 ROM 目录为 `/sdcard/rom`（不带 s），不是 `/sdcard/roms`。

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
| Flash 模式 | QIO 80 MHz | 代码取指约为 DIO 的 2 倍 |
| Flash 大小 | 16 MB | N16R8 板载 |
| PSRAM | Octal 80MHz | 8MB 八线 PSRAM |
| SPIRAM_RODATA | 启用 | 只读常量放 PSRAM，释放 ~80KB 内部 DRAM |
| 编译优化 | -Os | 体积优化，节省 ~10-20% 代码空间 |
| WiFi 缓冲 | PSRAM 分配 | 静态 RX 缩至 4，动态缓冲从 PSRAM 分配，节省 DRAM |
| mbedTLS | PSRAM 分配 | SSL 从 PSRAM 分配，减少内部 DRAM 碎片 |
| Bluetooth LE | NimBLE | 比 Bluedroid 节省 ~40KB DRAM，仅外设角色 |
| BT/WiFi 共存 | 启用 | BLE + WiFi 同时活跃时必需 |
| ESP-SR | MultiNet7 CN | 离线中文命令词 + NSNet2 降噪 + VADNet1 |
| FATFS 长文件名 | 启用（堆分配） | 支持中文文件名（UTF-8 API + GBK 代码页 936） |
| FreeRTOS HZ | 1000 | 1ms tick 精度 |

### 分区表（partitions.csv）

| 分区 | 大小 | 用途 |
|------|------|------|
| app0 | 6.5 MB | OTA 分区 0 |
| app1 | 6.5 MB | OTA 分区 1 |
| model | 3 MB | ESP-SR 模型数据（SPIFFS 子类型） |
| nvs | 20 KB | WiFi 凭证、音量设置等持久数据 |
| otadata | 8 KB | OTA 状态 |

ROM 和音乐文件存放在 MicroSD 卡，不再使用 Flash SPIFFS 存储。

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

### user/bemfa_config.h

| 宏 | 说明 |
|----|------|
| `BEMFA_UID` | 巴法云私钥（openID）|
| `BEMFA_TYPE` | 设备类型（固定 `3` = TCP 设备云）|

---

## 注意事项

- **凭证安全**：`asr_config.h`、`model_config.h` 和 `bemfa_config.h` 含 API Key / 私钥，已加入 `.gitignore`
- **PSRAM 必须选 Octal Mode**：录音缓冲、ROM 数据、HTTP 响应、音乐解码缓冲均分配于 PSRAM
- **I2S 资源分配**：I2S_NUM_0 = 麦克风（RX），I2S_NUM_1 = 扬声器（TX），互不干扰
- **I2S_NUM_0 共享**：在线 ASR 和离线 ESP-SR 通过 deinit/reinit + taskNotify 握手切换
- **LCD SPI 时钟**：80 MHz，PARTIAL 模式异步 DMA 刷新
- **SPI 总线隔离**：LCD 用 SPI2_HOST，SD 卡用 SPI3_HOST
- **SD 卡 CS 脚**：GPIO0（Boot 按键脚），上电时不要按住；原 GPIO48 因板载 WS2812 LED 干扰而弃用
- **ESP32-S3 蓝牙限制**：仅支持 BLE，不支持 Classic BT（A2DP 不可用）
- **游戏模式**：进入游戏自动暂停 WiFi + BLE，退出后仅恢复进入前活跃的服务
- **音频安全**：DC-block HPF 消除直流偏置，字节尾对齐防止 PCM 错位，DMA auto_clear 消除空闲噪声
- **SD 卡必须插入**：Game Boy ROM 和音乐文件从 SD 卡读取，未插卡时音乐和 GB 游戏不可用（内置 2048 仍可玩）
- **音乐文件格式**：WAV 需为 16bit PCM（8k-48k Hz），MP3 由 Helix 定点解码器支持（MPEG-1/2 Layer III）

---

## 工程亮点

### 线程安全设计

ESP32-S3 多任务环境下，共享状态的并发访问是最常见的崩溃源。本项目采用分层防护策略：

- **模块级 mutex**：WiFi、BLE、Speaker、音乐播放器、巴法云、大模型、天气各自维护独立的 mutex，保护内部状态不被事件回调、守护任务、UI 任务撕裂读写；所有 mutex 在 `app_main` 单线程阶段提前创建，消除 lazy-init 竞态窗口
- **volatile 无锁读取**：`wifi_get_status()`、`speaker_get_volume()` 等高频查询路径使用 `volatile` + 32 位对齐原子语义，避免加锁拖慢 UI/HTTP 路径
- **锁内拷贝**：`baidu_token_copy()` 在持有 mutex 期间将 token 拷贝到调用方栈缓冲区，释放锁后调用方使用的是本地副本，彻底消除跨线程指针悬空风险
- **flush 协议**：Speaker 的 `speaker_flush()` / `speaker_set_sample_rate()` 不直接操作 ring buffer，而是通过 `s_flush_request` + `s_flush_done` 信号量通知 tx_task 自行清空，保证 ring buffer 的唯一消费者不变
- **协作式任务退出**：WiFi 守护任务通过 notify 通知自行退出（而非 `vTaskDelete` 硬杀），避免在持有 mutex 时被删除导致永久死锁
- **异步 UI 操作**：音乐切歌/停止通过一次性后台任务执行（`music_stop_task` / `music_play_task`），避免 `music_stop` 的 2 秒等待阻塞 LVGL 线程；任务创建失败时降级为同步调用，功能正确优先

### 内存精细管理

ESP32-S3 内部 DRAM 仅 ~338KB，项目在内存使用上做了多层优化：

- **PSRAM 优先策略**：HTTP 响应缓冲、录音缓冲、ROM 数据、音乐解码缓冲、WiFi/LwIP 动态缓冲均分配在 8MB PSRAM，内部 DRAM 只放必须的 LVGL 双缓冲和 FreeRTOS 任务栈
- **用后即释**：ASR HTTP 响应缓冲在识别完成后立即 `release_http_buf()` 释放回 NULL，下次识别从 4KB 重新起步，避免单次扩容到 32KB 后永久占用；baidu_token / model / weather 三个模块的响应缓冲在 API 返回前统一 `resp_buf_release()` 归零；音乐扫描缓冲（`s_music_scan_buf`）在退出音乐屏幕时释放
- **动态扩容 + 即时收缩**：HTTP 响应缓冲从 4KB 起步，按需 2 倍扩容到最大 32KB，API 返回后立即释放回 NULL（不再常驻到下次复用）
- **NVS 写入防抖**：音量 slider 拖动通过 500ms 软件定时器合并写入，避免连续 100 次 flash 写操作加速磨损
- **BLE/ESP-SR 懒加载**：蓝牙和离线语音识别仅在进入对应界面时初始化，退出时 deinit 释放 ~100KB DRAM，空闲期零占用

### I2S 资源共享

I2S_NUM_0 被在线 ASR（`asr.c`）和离线 ESP-SR（`esp_sr.c`）共享，通过安全的交接协议：

- `asr_mic_deinit()` 先置 `s_rec_active = false`，再通过 `s_stop_waiter` + `xTaskNotifyGive` 等录音任务确认退出循环后，才调用 `i2s_del_channel`，避免在 `i2s_channel_read` 阻塞期间释放底层资源导致 HardFault
- `asr_mic_reinit()` 重建 I2S 通道后，常驻录音任务自动恢复，无需重新创建任务

### 音频处理健壮性

- **字节尾对齐**：HTTP chunked 传输可能给出奇数字节，`speaker_play` 保留尾字节到下次拼合，防止 16bit PCM 样本错位变成白噪声
- **DC-block HPF**：一阶高通滤波器（截止 ~12.7Hz @16kHz）消除 MAX98357A 直流偏置，Q15 定点乘避免浮点开销
- **线性淡入**：每次 flush 或首次播放的前 64 个样本按线性 ramp 升起，消除爆破声
- **动态采样率切换**：音乐播放前通过 flush 协议清空 ring + disable I2S → reconfig clock → enable，确保旧速率数据完全播完再切；播完自动恢复 16kHz，TTS/GB 音频不受影响
- **MP3 错误容忍**：连续 32 帧解码失败自动放弃，防止垃圾数据把 Helix 解码器推进非法状态导致 crash；自动跳过 ID3v2 tag 防止误同步；输入缓冲 memmove 后 read_ptr 始终指向有效数据头部，短读场景下不会读到未初始化字节

### 系统级鲁棒性

- **WiFi 守护任务**：首次连接 EventGroup 等待 + 快速重试 3 次；运行期断线自动指数退避重连（5s → 5min），永不放弃；用户主动断开则停止守护；`wifi_full_shutdown_for_ble()` 通过 `GUARDIAN_NOTIFY_SHUTDOWN` 通知守护任务协作式退出，避免 `vTaskDelete` 硬杀导致 mutex 死锁
- **BLE deinit 防护**：`s_deinit_in_progress` 门锁防止双拆；`s_pending_host_calls` 计数器等待跨任务 NimBLE API 调用完成后再拆 host；`s_adv_gen` 代际计数器防止 stop 后残留广播（ghost advertising）
- **BLE timer 竞态修复**：`accumulate_rx` 的 `xTimerReset` 移入 LOCK 块内，与 deinit 路径的 `s_rx_timer = NULL` 互斥，消除 UAF 窗口
- **任务生命周期安全**：ESP-SR 三个任务（read/feed/detect）通过 graceful stop + 2 秒超时硬杀 + ring buffer 残留保护，避免资源泄漏；音乐播放器的 `music_stop` 采用 10 轮 ×200ms 轮询确认旧任务退出，防止快速切歌产生僵尸任务
- **NULL 指针防护**：所有 `malloc` / `xTaskCreatePSRAM` 返回值均检查，失败时安全回退（UI 提示"内存不足"）而非解引用崩溃
- **异步取消语义**：天气查询支持真正的取消（`s_weather_cancelled` 标志 + drain queue + 双重检查），取消后后台任务完成也不会弹出界面
- **LVGL 对象生命周期**：BLE 结果弹窗通过 `LV_EVENT_DELETE` 回调统一清零全局句柄，覆盖所有销毁路径（OK 按钮 / 父屏切换 / 显式 delete），消除悬空指针
- **HTTP 缓冲即用即释**：baidu_token / model / weather 三个模块的 `s_resp_buf` 在 API 返回前统一 `resp_buf_release()`，避免 PSRAM 长期驻留浪费
- **模块 init 提前到单线程阶段**：`baidu_token_init()` / `bemfa_init()` / `model_init()` / `weather_init()` / `music_init()` 在 `app_main` 中调用，消除并发首次调用创建多个 mutex 的竞态
- **baidu_token 线程安全拷贝**：新增 `baidu_token_copy()` 在锁内将 token 拷贝到调用方缓冲区，ASR/TTS 使用该接口避免释放锁后 token 被其他线程覆写导致悬空读取
- **WiFi 守护任务协作式退出**：`wifi_full_shutdown_for_ble()` 不再使用 `vTaskDelete` 硬杀守护任务（可能在持有 mutex 时被杀导致死锁），改为发送 `GUARDIAN_NOTIFY_SHUTDOWN` 通知让任务自行退出，等待确认后再继续拆除 WiFi 驱动
- **PSRAM 任务 handle 竞态防护**：`task_entry` 开头加 `taskYIELD()`，确保创建者完成 `cleanup.handle` 赋值后新任务才开始执行用户函数，防止高优先级任务立即完成导致 cleaner 收到未初始化的 handle

---

## 代码统计

| 类别 | 行数 |
|------|------|
| 纯手写代码 | ~6,400 行 |
| 字体数据（lv_font_simhei_16 + lcdfont）| ~385,000 行 |
| 模拟器库（walnut_cgb.h + minigb_apu.c）| ~10,500 行 |
| LVGL 库 | 未计入 |

---

## 内存使用（ESP32-S3 N16R8）

### Flash（16 MB）

| 内容 | 大小 |
|------|------|
| 固件（ESP-IDF + LVGL + 应用 + 游戏模拟器 + ESP-SR + NimBLE + Helix MP3 + FATFS）| ~4.7 MB |
| ESP-SR 模型分区 | 3 MB |
| OTA 备份分区 | 6.5 MB |

### PSRAM（8 MB Octal）

| 用途 | 大小 |
|------|------|
| ASR 录音缓冲区 | ~320 KB（识别后释放） |
| HTTP 响应缓冲（动态扩容）| 4-32 KB（用后释放） |
| ROM 数据（游戏运行时）| 32 KB - 2 MB |
| 音乐解码缓冲（WAV/MP3）| ~16 KB（播放期间） |
| WiFi / LwIP 缓冲 | ~50 KB |
| BSS + rodata 外部段 | ~100 KB |
| 剩余可用 | **~7 MB** |

### 内部 DRAM（~338 KB 可用段）

| 用途 | 大小 |
|------|------|
| LVGL PARTIAL 双缓冲（34行 x 2）| ~32 KB |
| speaker RingBuffer | 64 KB |
| WiFi / LwIP / mbedTLS 运行时 | ~100 KB |
| FreeRTOS 任务栈 + TCB | ~35 KB |
| 剩余可用 | **~107 KB** |
