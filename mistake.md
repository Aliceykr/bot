# Bug 修复交接记录

本文档记录一次静态代码审查中发现的高风险问题。目标是让后续接手者即使没有对话上下文，也能根据这里的定位、根因和修改方向完成修复。

审查范围：工程自有代码，主要包括 `main/`、`mylvgl/`、`user/`、`game/`、`BLE_APP/lib/`。未审查第三方 `lvgl/` 源码本身。

## 1. PSRAM 任务绕过包装器退出，导致任务栈和 TCB 泄漏

### 位置

- `main/psram_task.c`
- `main/psram_task.h`
- `mylvgl/my_demo.c`

### 相关代码

`xTaskCreatePSRAM()` / `xTaskCreatePSRAMPinnedToCore()` 的设计是：用户任务函数正常 `return` 后，包装入口 `task_entry()` 会把 PSRAM 栈和内部 DRAM TCB 交给 cleaner 任务释放。

`main/psram_task.c` 中的关键逻辑：

- `task_entry()` 调用用户函数。
- 用户函数返回后，`task_entry()` 向 `s_cleanup_queue` 投递 `cleanup_item_t`。
- cleaner 释放 `stack` 和 `tcb`。

但以下任务是通过 `xTaskCreatePSRAM()` 创建的，却在任务函数内部直接调用 `vTaskDelete(NULL)`：

- `bemfa_list_task()`：`mylvgl/my_demo.c`
- `bemfa_send_task()`：`mylvgl/my_demo.c`
- `bemfa_info_task()`：`mylvgl/my_demo.c`
- `bemfa_voice_task()`：`mylvgl/my_demo.c`

创建点：

- `bemfa_kick_refresh()` 调用 `xTaskCreatePSRAM(bemfa_list_task, ...)`
- `bemfa_kick_send()` 调用 `xTaskCreatePSRAM(bemfa_send_task, ...)`
- `bemfa_kick_info()` 调用 `xTaskCreatePSRAM(bemfa_info_task, ...)`
- `bemfa_voice_released_cb()` 调用 `xTaskCreatePSRAM(bemfa_voice_task, ...)`

### 根因

`vTaskDelete(NULL)` 直接删除当前任务，导致包装入口 `task_entry()` 后半段不会执行。这样 cleanup 信息不会入队，PSRAM 栈和 TCB 永远不会释放。

### 影响

反复进入智能设备页面、刷新设备列表、发送设备开关、执行语音控制后，会持续泄漏 PSRAM 栈和少量内部 DRAM。长期运行可能出现：

- PSRAM 逐步下降；
- 后台任务创建失败；
- HTTP / cJSON / 音频缓冲分配失败；
- 系统运行一段时间后功能变得不稳定。

### 修改方向

优先做法：所有通过 `xTaskCreatePSRAM()` 创建的用户任务都不要调用 `vTaskDelete(NULL)`，任务函数执行完后直接 `return`。

具体改法：

- 在 `bemfa_list_task()` 中，把所有 `vTaskDelete(NULL); return;` 改为 `return;`。
- 在 `bemfa_send_task()` 末尾，把 `vTaskDelete(NULL);` 改为 `return;`。
- 在 `bemfa_info_task()` 中，把所有 `vTaskDelete(NULL); return;` 改为 `return;`。
- 在 `bemfa_voice_task()` 末尾，把 `vTaskDelete(NULL);` 改为 `return;`。

注意：

- 不要修改普通 `xTaskCreate()` 创建的一次性任务退出方式，普通动态任务可以继续 `vTaskDelete(NULL)`。
- 可考虑把 `psram_task_exit()` 标记为不推荐使用，或者让它只作为注释意义存在。它当前是空函数，不能释放资源。
- 后续可以用 `rg -n "xTaskCreatePSRAM|vTaskDelete\\(NULL\\)" mylvgl user main game` 再查一遍，确认没有其他 PSRAM 栈任务自删。

### 验证方法

1. 编译通过。
2. 反复进入“智能设备”页面并刷新列表 20 次，观察 `health.c` 打印的 PSRAM 和 DRAM 水位。
3. 反复点击设备开关 20 次，观察内存是否回到接近原值。
4. 反复执行巴法云语音控制，确认任务结束后没有持续内存下降。

## 2. ESP-SR 的 I2S 初始化失败会触发 abort 重启

### 位置

- `user/esp_sr.c`

### 相关代码

`sr_i2s_init()` 内部使用了 `ESP_ERROR_CHECK()`：

- `i2s_new_channel(...)`
- `i2s_channel_init_std_mode(...)`
- `i2s_channel_enable(...)`

外层 `esp_sr_start_listening()` 已经写了错误处理逻辑：

- 调 `asr_mic_deinit()` 释放 ASR 的 I2S_NUM_0。
- 调 `sr_i2s_init()`。
- 若失败，调用 `asr_mic_reinit()` 并返回 `false`。

但由于 `sr_i2s_init()` 内部是 `ESP_ERROR_CHECK()`，失败时会直接 abort，外层的恢复逻辑根本执行不到。

### 根因

错误处理风格不一致。外层希望函数返回 `esp_err_t`，内层却用 abort 型宏。

### 影响

如果出现以下情况，点击“语音命令”可能直接整机重启：

- I2S_NUM_0 没有被完全释放；
- 内部 DRAM 不足导致 I2S 分配失败；
- I2S GPIO / 驱动状态异常；
- ESP-SR 和 ASR 资源切换过程中遇到边界状态。

### 修改方向

把 `sr_i2s_init()` 改成真正返回错误码，不使用 `ESP_ERROR_CHECK()`。

建议结构：

```c
static esp_err_t sr_i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_sr_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sr i2s_new_channel failed: %s", esp_err_to_name(err));
        s_sr_rx_chan = NULL;
        return err;
    }

    err = i2s_channel_init_std_mode(s_sr_rx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sr i2s init std failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_sr_rx_chan);
        s_sr_rx_chan = NULL;
        return err;
    }

    err = i2s_channel_enable(s_sr_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sr i2s enable failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_sr_rx_chan);
        s_sr_rx_chan = NULL;
        return err;
    }

    return ESP_OK;
}
```

注意：

- `std_cfg` 仍按当前代码配置，保持和 ASR 麦克风采样格式一致。
- 任一步失败都要清理已经创建的 channel，并把 `s_sr_rx_chan = NULL`。
- 外层 `esp_sr_start_listening()` 的失败恢复路径可以保留：失败后 `asr_mic_reinit()`，返回 `false`。

### 验证方法

1. 正常进入“语音命令”，确认能开启和关闭。
2. 人为制造 I2S 初始化失败场景，例如让 `asr_mic_deinit()` 不释放成功或临时占用 I2S_NUM_0，确认系统不重启，而是弹出“语音监听启动失败”或类似提示。
3. 开关“语音命令”多次，确认 ASR 语音助手仍能录音。

## 3. 聊天助手长输入可能不是 null 结尾，导致堆越界读

### 位置

- `mylvgl/my_demo.c`
- `user/model.c`

### 相关代码

`chat_send_cb()` 中：

```c
char *msg = malloc(MODEL_MAX_INPUT);
strncpy(msg, txt, MODEL_MAX_INPUT - 1);
```

这里没有补：

```c
msg[MODEL_MAX_INPUT - 1] = '\0';
```

聊天输入框创建处也没有看到长度限制：

- `show_chat_screen()`
- `lv_textarea_create(scr)`
- `lv_textarea_set_one_line(chat_input, true)`

后续 `chat_fetch_task()` 调用：

```c
model_chat(msg, &res.data);
```

`model_chat()` 中又把原始 `user_msg` 传给：

```c
cJSON_AddStringToObject(msg, "content", user_msg);
```

### 根因

`strncpy()` 在源字符串长度大于等于拷贝长度时不会自动写入 `\0`。如果用户输入超过 `MODEL_MAX_INPUT - 1` 字节，`msg` 不是合法 C 字符串。

### 影响

长输入可能导致：

- HTTP JSON 请求内容带入堆上的脏数据；
- cJSON 继续读取堆后面的字节；
- 请求构造异常；
- 极端情况下触发崩溃。

### 修改方向

至少修补 null 终止：

```c
strncpy(msg, txt, MODEL_MAX_INPUT - 1);
msg[MODEL_MAX_INPUT - 1] = '\0';
```

更推荐同时限制输入框长度：

```c
lv_textarea_set_max_length(chat_input, MODEL_MAX_INPUT - 1);
```

还可考虑把 `malloc(MODEL_MAX_INPUT)` 改成 `calloc(1, MODEL_MAX_INPUT)`，但不能只依赖 calloc，仍建议显式补 `\0`。

### 验证方法

1. 在聊天框输入超过 512 字节的文本。
2. 确认不会崩溃，请求内容被截断但仍合法。
3. 串口日志中不应出现异常 JSON、HTTP 构造失败或 LoadProhibited。

## 4. 天气查询取消后快速重试，旧请求可能污染新请求

### 位置

- `mylvgl/my_demo.c`

### 相关代码

取消按钮：

```c
s_weather_cancelled = true;
weather_fetching = false;
```

发起新请求时：

```c
s_weather_cancelled = false;
weather_fetching = true;
xTaskCreatePSRAM(weather_fetch_task, ...);
```

后台任务结束时：

```c
if (!s_weather_cancelled) {
    xQueueSend(weather_result_queue, &res, 0);
}
```

### 根因

天气请求使用一个全局布尔值区分是否取消。该布尔值不属于某一次请求，而是所有请求共享。取消第一次请求后，如果用户马上发起第二次请求，全局标志被改回 `false`。此时第一次慢请求结束时会认为自己没有被取消，并把旧结果投递到队列。

### 影响

用户快速操作时可能出现：

- 取消的天气请求仍然弹出结果；
- 旧天气结果覆盖新查询；
- loading 弹窗和页面切换状态错乱。

### 修改方向

使用请求 generation id，而不是单个布尔值。

建议加全局变量：

```c
static volatile uint32_t s_weather_req_id = 0;
static volatile uint32_t s_weather_cancelled_id = 0;
```

或更简单：

- 每次开始天气请求时 `uint32_t req_id = ++s_weather_req_id;`
- 把 `req_id` 通过任务参数传给 `weather_fetch_task`。
- 结果结构中也带 `req_id`。
- timer 收到结果时，只处理 `req_id == s_weather_req_id` 的结果。
- 取消时可以递增 `s_weather_req_id`，让所有旧任务自然失效。

注意：

- 任务参数不要传栈地址，要堆分配或使用小结构。
- 结果队列 drain 仍可保留，但不能作为唯一防线。
- 语音命令入口和菜单点击入口都要使用同一套启动函数，避免两套逻辑遗漏。

### 验证方法

1. 进入天气查询后立刻取消。
2. 马上再次进入天气查询。
3. 人为让第一次请求慢于第二次返回，确认旧结果不会显示。
4. 连续取消/重试 10 次，确认不会出现错乱弹窗。

## 5. ROM 目录注释、头文件和实现不一致

### 位置

- `game/rom_loader.c`
- `game/rom_loader.h`
- `mylvgl/my_demo.c`

### 相关代码

`rom_loader.c` 文件头注释说 ROM 位于：

```text
/sdcard/roms/
```

`rom_loader.h` 也说扫描：

```text
/sdcard/roms/
```

但实际宏定义是：

```c
#define ROM_DIR SDCARD_MOUNT_POINT "/rom"
```

UI 提示也是 `/rom`：

- `SD 卡未挂载或 /rom 目录不存在`
- `SD 卡 /rom 目录暂无 .gb / .gbc`

### 根因

目录名从 `roms` 改成 `rom` 或反过来时，没有同步所有注释、提示和实现。

### 影响

用户按头文件、注释或报告建立 `/sdcard/roms` 目录，会发现游戏列表没有 ROM。这个问题不会崩溃，但会直接造成 Game Boy ROM 功能不可用。

### 修改方向

二选一，必须统一。

推荐方案：统一为 `/sdcard/roms`，因为注释和常见语义更清楚。

需要修改：

- `game/rom_loader.c` 中 `ROM_DIR` 改为 `SDCARD_MOUNT_POINT "/roms"`。
- `mylvgl/my_demo.c` 中 UI 提示从 `/rom` 改为 `/roms`。
- 检查 README、study.md、设计报告里是否提到 ROM 目录，全部统一。

如果决定保留 `/rom`，则反向修改：

- `rom_loader.c` 文件头注释。
- `rom_loader.h` 注释。
- README / study.md / 文档。

### 验证方法

1. SD 卡创建目标目录，例如 `/sdcard/roms`。
2. 放入 `.gb` 和 `.gbc` 文件。
3. 进入游戏页面，确认列表显示文件名和大小。
4. 选择 ROM，确认能进入运行流程。

## 6. LCD SPI 初始化未检查返回值，失败后可能空句柄崩溃

### 位置

- `user/lcd.c`
- `mylvgl/lv_port_disp.c`
- `game/gb_emu.c`
- `game/game_2048.c`

### 相关代码

`LCD_GPIO_Init()` 中：

```c
spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi);
```

返回值没有检查。如果初始化失败，`s_spi` 可能仍为 `NULL`。

后续显示刷新、GB 模拟器和 2048 都会直接或间接使用 `s_spi`。

### 根因

LCD 初始化假设 SPI2 总线和设备一定创建成功，没有失败路径。

### 影响

在以下情况下可能出现空句柄访问或 SPI driver 断言：

- SPI2 被其他模块占用；
- DMA channel 分配失败；
- 内部内存不足；
- 引脚或驱动初始化失败。

### 修改方向

把 `LCD_GPIO_Init()` 或 `LCD_Init()` 改成能返回 `bool` / `esp_err_t` 最好。但如果不想大改接口，也至少检查错误并让 `s_spi` 保持 NULL，同时后续发送函数防御。

建议最小改法：

```c
esp_err_t ret = spi_bus_initialize(...);
if (ret != ESP_OK) {
    ESP_LOGE("LCD", "spi_bus_initialize failed: %s", esp_err_to_name(ret));
    s_spi = NULL;
    return;
}

ret = spi_bus_add_device(..., &s_spi);
if (ret != ESP_OK) {
    ESP_LOGE("LCD", "spi_bus_add_device failed: %s", esp_err_to_name(ret));
    spi_bus_free(SPI2_HOST);
    s_spi = NULL;
    return;
}
```

同时在 `LCD_Send_Buf()`、`lcd_spi_send()`、LVGL `disp_flush()`、游戏 DMA 入口处考虑：

```c
if (!s_spi) {
    // log once, skip transfer / return
}
```

更完整的做法是让 `LCD_Init()` 返回失败，`app_main()` 在 LCD 不可用时不要启动 LVGL。

### 验证方法

1. 正常硬件下 LCD 仍能显示菜单。
2. 临时制造 SPI 初始化失败，确认不会崩溃，而是打印清晰错误。
3. LCD 初始化失败时 LVGL 不应继续对空 `s_spi` 发 DMA。

## 7. Flutter BLE 服务断开后状态清理不完整

### 位置

- `BLE_APP/lib/ble_service.dart`

### 相关代码

`connect()` 中：

```dart
device.connectionState.listen((state) {
  if (state == BluetoothConnectionState.disconnected) {
    _connectionController.add(false);
    _cleanup();
  }
});
```

该订阅没有保存，也没有取消。

`_cleanup()` 中：

```dart
_notifySub?.cancel();
_notifySub = null;
_char = null;
```

没有清 `_device`，也没有取消 connectionState 订阅。

### 根因

BLE 服务类是单例，连接/断开会多次发生。连接状态监听如果不保存订阅，重复连接会累积多个 listener；断开后 `_device` 保留旧对象，`isConnected` 和 `deviceName` 可能基于旧对象返回不准确结果。

### 影响

手机端多次连接/断开后可能出现：

- 重复弹“设备已断开”；
- 连接状态回调重复触发；
- 页面认为仍连接旧设备；
- notify 或连接状态行为不稳定。

### 修改方向

增加一个连接状态订阅字段：

```dart
StreamSubscription? _connStateSub;
```

在 `connect()` 前先清理旧连接状态：

```dart
await _connStateSub?.cancel();
_connStateSub = null;
```

保存订阅：

```dart
_connStateSub = device.connectionState.listen(...);
```

在 `_cleanup()` 里：

```dart
_notifySub?.cancel();
_notifySub = null;
_connStateSub?.cancel();
_connStateSub = null;
_char = null;
_device = null;
```

注意：

- 如果 `_cleanup()` 是由 connectionState listener 自己调用的，直接 cancel 当前订阅通常没问题，但可以用微任务或判空保护，避免平台实现边界。
- `disconnect()` 中先 `await _device?.disconnect()`，再 `_cleanup()`。

### 验证方法

1. 手机端连续连接/断开 5 次。
2. 每次断开只出现一次断开提示。
3. 回到扫描页后再次连接，功能页能正常发送命令并接收 notify。

## 8. 巴法云页面未检查 queue/timer 创建失败

### 位置

- `mylvgl/my_demo.c`

### 相关代码

`show_bemfa_screen()` 中：

```c
bemfa_list_queue   = xQueueCreate(...);
bemfa_send_queue   = xQueueCreate(...);
bemfa_info_queue   = xQueueCreate(...);
bemfa_voice_queue  = xQueueCreate(...);
bemfa_poll_timer   = lv_timer_create(...);
```

随后直接：

```c
bemfa_screen_active = true;
bemfa_kick_refresh();
```

### 根因

没有处理队列或 timer 创建失败。内存紧张时，页面会进入 active 状态，但后台任务可能无法投递结果，或者 UI 没有 timer 轮询结果。

### 影响

内存不足场景下可能出现：

- 页面卡在“加载中...”；
- 后台任务执行了 HTTP 请求但结果被丢弃；
- 用户无法得到明确错误提示；
- 后续退出页面时需要处理部分队列为 NULL 的混合状态。

### 修改方向

在创建完 queue/timer 后立即检查：

```c
if (!bemfa_list_queue || !bemfa_send_queue || !bemfa_info_queue ||
    !bemfa_voice_queue || !bemfa_poll_timer) {
    // 删除已创建的 queue/timer
    // 删除页面或显示“内存不足”
    // indev_set_group(group)
    return;
}
```

注意：

- 如果 `lv_timer_create()` 失败，不能进入 active。
- 失败路径要释放 `bemfa_group`、已创建 queue、页面对象。
- 不要调用 `bemfa_kick_refresh()`。

### 验证方法

1. 人为降低可用内存或临时让某个 `xQueueCreate` 返回 NULL。
2. 进入智能设备页面，应显示明确错误或返回菜单，不应卡住。

## 9. 后续修复建议顺序

建议按以下顺序修：

1. 修 PSRAM 任务自删泄漏。
2. 修 ESP-SR `ESP_ERROR_CHECK()` abort 问题。
3. 修聊天输入 null 终止和最大长度。
4. 修天气请求 generation id。
5. 统一 ROM 目录。
6. 补 LCD SPI 初始化错误处理。
7. 修 Flutter BLE cleanup。
8. 补巴法云页面 queue/timer 创建失败处理。

每修一项都先编译，再做最小功能验证。不要把所有问题一次性大改完，否则嵌入式上定位回归会很痛。
