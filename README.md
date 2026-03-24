# Bot - ESP32-S3 AI 智能助手工程

基于 ESP-IDF + LVGL 的 ESP32-S3 嵌入式 AI 助手，支持 WiFi 连接、天气查询、AI 聊天、语音录音，配备 TFT LCD 图形界面和旋转编码器交互。

## 硬件

| 组件 | 型号 |
|------|------|
| 主控 | ESP32-S3-DevKitC-1 N16R8（16MB Flash + 8MB Octal PSRAM）|
| 屏幕 | 2.4 寸 TFT LCD，分辨率 240×320，驱动芯片 ST7735S |
| 麦克风 | INMP441 I2S 数字麦克风 |
| 输入 | 旋转编码器（A/B/SW）|

## 引脚接线

### LCD（SPI2）

| 功能 | ESP32-S3 引脚 |
|------|---------------|
| MOSI | GPIO11 |
| SCLK | GPIO12 |
| RES  | GPIO10 |
| DC   | GPIO9  |
| BLK  | GPIO46 |

### INMP441 麦克风（I2S，I2S_NUM_0）

| 功能 | ESP32-S3 引脚 |
|------|---------------|
| SCK (BCLK) | 见 `user/asr_config.h` |
| WS  (LRCK) | 见 `user/asr_config.h` |
| SD  (DATA) | 见 `user/asr_config.h` |

### 旋转编码器

| 功能 | ESP32-S3 引脚 |
|------|---------------|
| A    | GPIO4 |
| B    | GPIO5 |
| SW   | GPIO6 |

> 所有引脚定义可在对应头文件顶部修改。

## 目录结构

```
bot/
├── main/
│   ├── main.c                  # 应用入口：初始化 LCD/LVGL/USB，创建任务
│   └── CMakeLists.txt
├── mylvgl/
│   ├── my_demo.c               # LVGL 主界面逻辑（菜单、弹窗、天气/聊天/语音界面）
│   ├── my_demo.h
│   ├── lv_port_disp.c          # LVGL 显示驱动（全帧 DMA 刷新）
│   ├── lv_port_disp.h
│   ├── lv_port_indev.c         # LVGL 输入设备（旋转编码器）
│   └── lv_port_indev.h
├── user/
│   ├── lcd.c / lcd.h           # ST7735S SPI LCD 驱动（含 DMA 异步接口）
│   ├── lcdfont.h               # ASCII 点阵字库
│   ├── asr.c / asr.h           # INMP441 I2S 录音 + 百度 ASR 云识别
│   ├── asr_config.h            # 百度 ASR 凭证（gitignored）
│   ├── asr_config.h.example    # 配置模板
│   ├── model.c / model.h       # LLM 聊天（OpenAI 兼容 REST API）
│   ├── model_config.h          # API Key/URL（gitignored）
│   ├── model_config.h.example  # 配置模板
│   ├── weather.c / weather.h   # 天气查询 API
│   ├── wifi.c / wifi.h         # WiFi 连接管理
│   ├── sntp_time.c / sntp_time.h # NTP 时间同步
│   └── usb_audio.c / usb_audio.h # USB CDC 音频输出（调试用）
├── lvgl/                       # LVGL 9.x 库
├── CMakeLists.txt
└── README.md
```

## 主要功能

- **图形界面**：LVGL 9.x，240×320 全帧 DMA 刷新，旋转编码器导航
- **WiFi 连接**：后台任务连接，弹窗显示进度和结果，NTP 时间同步
- **天气查询**：HTTP 获取实时天气，独立天气详情界面
- **AI 聊天**：OpenAI 兼容 API，键盘输入，滚动对话记录
- **语音录音**：INMP441 I2S 采集，PCM 数据通过 USB CDC 发送至电脑（ASR 云识别接口已实现，可选启用）

## 编译烧录

```bash
# 首次使用需复制配置模板并填写凭证
cp user/asr_config.h.example user/asr_config.h
cp user/model_config.h.example user/model_config.h
# 编辑填写 API Key 等信息后编译
idf.py build
idf.py flash monitor
```

## 重要配置（menuconfig）

| 配置项 | 推荐值 | 路径 |
|--------|--------|------|
| PSRAM 模式 | Octal Mode | Component config → ESP PSRAM |
| FreeRTOS Tick Rate | 1000 Hz | Component config → FreeRTOS → configTICK_RATE_HZ |
| mbedTLS 内存分配 | Default (跟随系统 malloc) | Component config → mbedTLS → Memory allocation strategy |
| 内部堆保留 | 65536 字节 | Component config → ESP PSRAM → Reserve internal memory |

## 架构说明

### 任务结构

| 任务 | 优先级 | 栈大小 | 职责 |
|------|--------|--------|------|
| `lv_tick` | 5 | 2048 | 每 5ms 调用 `lv_tick_inc` |
| `lv_task` | 4 | 32768 | 每 5ms 调用 `lv_timer_handler`（含 UI 渲染和 flush）|
| `enc_task` | 6 | 2048 | 每 2ms 轮询编码器 GPIO |
| `wifi_task` | 3 | 4096 | WiFi 连接 + NTP 同步（一次性）|
| `weather_task` | 3 | 16384 | HTTP 天气查询（一次性）|
| `chat_task` | 3 | 16384 | LLM API 请求（一次性）|

### 弹窗与 UI 线程安全

所有 LVGL 操作均在 `lv_task` 单线程中执行。后台任务通过 **FreeRTOS Queue** 传递结果，由 `lv_timer` 轮询队列后在 LVGL 线程内更新 UI，避免跨任务直接操作 LVGL 对象。

弹窗采用统一辅助函数管理：
- `create_loading_dialog()` — 创建加载弹窗（spinner + 取消按钮），自动管理编码器 group
- `create_result_dialog()` — 创建结果弹窗（文字 + OK 按钮），自动释放 group 防泄漏
- `safe_delete_loading_dialog()` — 安全删除弹窗，防二次触发
- `indev_set_group()` — 统一编码器 group 切换

### LCD 刷新机制

采用全帧双缓冲 + DMA 非阻塞刷新：

1. LVGL 渲染完整帧到 `buf1`（PSRAM，153600 字节）
2. `disp_flush` 通过 `LCD_Send_Buf_Async` 提交 DMA，分块发送（每块 ≤ 4092 字节），最后一块异步入队后立即返回
3. LVGL 并行渲染下一帧到 `buf2`
4. 下次 flush 前 `LCD_Send_Buf_Wait` 确保上帧 DMA 完成
5. Mutex 保护 SPI 总线，防止 FreeRTOS 多任务并发访问

## 注意事项

- `user/asr_config.h` 和 `user/model_config.h` 已加入 `.gitignore`，不会提交到版本库
- ASR 云识别功能默认关闭（录音数据通过 USB CDC 发送至电脑），如需启用请取消 `asr.c` 中的注释
- LCD SPI 时钟 40MHz，全帧刷新约 30ms（~33fps）
- PSRAM 双缓冲共占用约 300KB，WiFi + mbedTLS 运行时需额外约 100KB 内部堆
