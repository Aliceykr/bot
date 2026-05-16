# Bot — ESP32-S3 AI Voice Assistant & Game Boy Emulator

> 🌐 Language: [简体中文](README.md) | **English** | [Français](README.fr.md) | [Español](README.es.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

ESP-IDF + LVGL embedded project on ESP32-S3 that integrates WiFi, weather lookup, AI chat, online/offline speech recognition, text-to-speech, Game Boy emulator with audio, BLE provisioning with remote music control, a MicroSD music player (WAV + MP3), volume control, and Bemfa Cloud smart-device control. The device has a TFT LCD GUI driven by a rotary encoder plus a 3x3 keypad matrix.

---

## Hardware

| Component | Model / spec | Notes |
|-----------|--------------|-------|
| MCU | ESP32-S3-DevKitC-1 N16R8 | 16 MB flash + 8 MB Octal PSRAM |
| Display | 2.4" TFT LCD, 240x320, ILI9341 | SPI, 80 MHz |
| Microphone | INMP441 | I2S digital mic on I2S_NUM_0 |
| Speaker | MAX98357A + speaker | I2S class-D amp on I2S_NUM_1 |
| Input | Rotary encoder (A/B/SW) | PCNT hardware decode + GPIO polling |
| Buttons | 3x3 keypad matrix (9 keys) | Game D-pad + A/B/START/SELECT/EXIT |
| Storage | MicroSD card (SPI mode) | SPI3_HOST, FAT32 |

---

## Pinout

### LCD (SPI2, ILI9341)

| Function | ESP32-S3 pin | Notes |
|----------|--------------|-------|
| MOSI | GPIO11 | SPI data |
| SCLK | GPIO12 | SPI clock, 80 MHz |
| RES  | GPIO10 | Reset |
| DC   | GPIO9  | Data/command select |
| BLK  | GPIO46 | Backlight (active high) |

### INMP441 microphone (I2S_NUM_0)

| Function | ESP32-S3 pin | INMP441 pin |
|----------|--------------|-------------|
| BCLK | GPIO38 | SCK |
| LRCK | GPIO39 | WS |
| DATA | GPIO40 | SD |
| 3.3V | 3.3V | VDD |
| GND  | GND  | GND, L/R (tie low for left channel) |

### MAX98357A amplifier (I2S_NUM_1)

| Function | ESP32-S3 pin | MAX98357A pin |
|----------|--------------|----------------|
| BCLK | GPIO15 | BCLK |
| LRCK | GPIO16 | LRC |
| DATA | GPIO17 | DIN |
| 3.3V | 3.3V   | SD (always on) |

### Rotary encoder

| Function | ESP32-S3 pin |
|----------|--------------|
| A    | GPIO4 |
| B    | GPIO5 |
| SW (push) | GPIO6 |

A/B phases are decoded by hardware PCNT, the SW button uses a 5 ms polled state machine with 20 ms debounce.

### 3x3 keypad matrix

| Row / Col | GPIO41 (C0) | GPIO42 (C1) | GPIO47 (C2) |
|------------|-------------|-------------|-------------|
| GPIO1 (R0)  | B | UP | A |
| GPIO2 (R1)  | LEFT | EXIT | RIGHT |
| GPIO14 (R2) | SELECT | DOWN | START |

2 ms scan period, two consecutive consistent samples required to confirm. Game mode toggle: `keypad_set_game_mode(true)` enables key output to the emulator.

### MPU6050 6-axis sensor (I2C_NUM_0)

| Function | ESP32-S3 pin | MPU6050 pin |
|----------|--------------|-------------|
| SDA  | GPIO20 | SDA |
| SCL  | GPIO7  | SCL |
| 3.3V | 3.3V   | VCC |
| GND  | GND    | GND, AD0 (address 0x68) |

400 kHz I2C, ±2 g range, DLPF 44 Hz. Initialised only when entering 2048, the bus is released on exit. Tilt detection uses hysteresis thresholds (ENTER 0.30 g, EXIT 0.15 g) plus direction lock with X/Y dominant axis to choose the move direction. Flat orientation produces no input. The hysteresis design eliminates the "spurious opposite move when returning to flat" jitter.

### MicroSD card (SPI3_HOST)

| Function | ESP32-S3 pin | SD module pin |
|----------|--------------|---------------|
| CS   | GPIO0  | CS |
| MOSI | GPIO8  | DI (CMD) |
| SCK  | GPIO18 | CLK |
| MISO | GPIO21 | DO (DAT0) |
| VCC  | 3.3V   | VCC |
| GND  | GND    | GND |

SPI3_HOST is used because LCD owns SPI2_HOST. FAT32 mounted at `/sdcard`.

> Note: CS is on GPIO0 (the boot button pin); just don't hold it down at power-on. The original GPIO48 was abandoned because the on-board WS2812 LED was injecting noise on the CS line.

---

## Project layout

```
bot/
├── main/
│   ├── main.c                    # App entry: peripherals init, LVGL task
│   ├── psram_task.c / .h         # PSRAM-stacked task helpers (auto cleaner)
│   ├── CMakeLists.txt
│   └── idf_component.yml         # IDF component deps (esp-sr, helix-mp3)
├── mylvgl/
│   ├── my_demo.c                 # Main LVGL UI (menu / weather / chat / asr / game / ble / music / volume / smart-device)
│   ├── my_demo.h
│   ├── lv_port_disp.c / .h       # LVGL display driver (PARTIAL double-buffer + async DMA)
│   ├── lv_port_indev.c / .h      # LVGL input device (PCNT encoder)
├── user/
│   ├── lcd.c / lcd.h             # ILI9341 SPI LCD driver
│   ├── lcdfont.h                 # ASCII bitmap fonts (12 / 16 / 24 / 32 px)
│   ├── asr.c / asr.h             # INMP441 I2S recording + Baidu ASR API
│   ├── asr_config.h              # Baidu ASR credentials (gitignored)
│   ├── asr_config.h.example
│   ├── baidu_token.c / .h        # Baidu OAuth2 token management (cache + refresh)
│   ├── model.c / model.h         # LLM chat client (OpenAI-compatible REST)
│   ├── model_config.h            # API key / URL / model name (gitignored)
│   ├── model_config.h.example
│   ├── speaker.c / speaker.h     # MAX98357A I2S output + DC-block HPF + volume + dynamic sample rate
│   ├── tts.c / tts.h             # Baidu TTS API (streaming PCM playback)
│   ├── weather.c / weather.h     # Weather HTTP query and parsing
│   ├── bemfa.c / bemfa.h         # Bemfa Cloud HTTP REST client (list / topicInfo / push)
│   ├── bemfa_config.h            # Bemfa private key (gitignored)
│   ├── bemfa_config.h.example
│   ├── wifi.c / wifi.h           # WiFi STA + reconnect guardian + mutex
│   ├── sntp_time.c / sntp_time.h # SNTP time sync (Aliyun NTP)
│   ├── health.c / health.h       # Heap watermark monitor (60 s period)
│   ├── esp_sr.c / esp_sr.h       # ESP-SR offline Chinese command-word recognition (AFE + MultiNet7)
│   ├── ble_prov.c / ble_prov.h   # NimBLE BLE provisioning (HM-10 compatible) + mutex
│   ├── music.c / music.h         # SD-card music player (WAV + MP3 via Helix)
│   ├── keypad.c / keypad.h       # 3x3 keypad scan (game input)
│   ├── sdcard.c / sdcard.h       # MicroSD SPI driver (FATFS mount/unmount)
│   └── lv_font_simhei_16.c       # SimHei 16 px LVGL Chinese font
├── game/
│   ├── gb_emu.c / gb_emu.h       # Game Boy emulator integration (Walnut-CGB / Peanut-GB)
│   ├── gb_audio.c / gb_audio.h   # GB audio: MiniGB APU → mono → speaker ring buffer
│   ├── minigb_apu.c              # MiniGB APU (third party)
│   ├── game_2048.c / game_2048.h # 2048 (with MPU6050 tilt control)
│   ├── mpu6050.c / mpu6050.h     # MPU6050 I2C driver (used by 2048 only)
│   ├── game_runtime.c / .h       # Game lifecycle (load / run / exit)
│   ├── rom_loader.c / .h         # SD-card ROM scan and loader
│   ├── walnut_cgb.h              # Walnut-CGB core (high-perf, 32-bit path)
│   ├── peanut_gb.h               # Peanut-GB core (original 8-bit, fallback)
│   └── CMakeLists.txt
├── lvgl/                         # LVGL 9.x sources
├── lv_conf.h                     # LVGL config
├── partitions.csv                # Custom partitions (6.5 MB dual OTA + ESP-SR model)
├── sdkconfig.defaults            # Defaults (ESP32-S3 / BLE / ESP-SR / WiFi / FATFS)
└── CMakeLists.txt                # Top-level CMake (UTF-8)
```

---

## Features

### 1. Main menu

The boot screen shows the feature list. Scroll with the rotary encoder, confirm with push:

- **Environment monitor** — placeholder (work in progress)
- **Weather & date** — fetch live weather over HTTP, show temperature, humidity, wind, real-time clock
- **Game** — the built-in 2048 (no SD card required) plus any `.gb` / `.gbc` ROMs scanned from `/sdcard/rom/` running on the Game Boy emulator
- **Chat assistant** — on-screen keyboard, calls an LLM API, scrolling chat history
- **Voice assistant** — online flow: record → Baidu ASR → LLM → Baidu TTS → playback
- **Voice command** — offline ESP-SR command-word recognition as a toggle (just like the Bluetooth toggle, no separate screen). When a command is recognised, SR is torn down and the corresponding feature opens.
- **Bluetooth** — start BLE advertising; phone sends `SSID_xxx password_xxx` to provision WiFi; also supports BLE-driven music control (`/music on` to list, `/<index>` to play, `/music off` to stop)
- **Music** — play `.wav` / `.mp3` from `/sdcard/music/` (pause/skip supported)
- **Volume** — slider 0–100 % with logarithmic gain curve, persisted to NVS, restored on boot
- **Smart device** — Bemfa Cloud TCP devices: list, send explicit on/off, with optimistic UI

### 2. Game Boy emulator

- Built on Walnut-CGB (high-perf rewrite of Peanut-GB), supports DMG and CGB titles
- Dual fetch chain + 32-bit DMA path, optimised for 32-bit MCUs like the ESP32-S3
- Native 160x144 → 1.5x scaled to 240x216
- 80 MHz async DMA double-line buffer rendering
- ROMs are loaded from SD card to PSRAM (up to 4 MB); small ROMs (≤256 KB) are also copied to DRAM for speed
- Built-in 2048 lives at the top of the list, always visible even with no SD card
- 2048 tilt control: MPU6050 accelerometer maps device tilt to four directions and runs alongside the keypad (keypad takes priority; tilt is only used when no key is held)
- 3x3 keypad covers full GB inputs (A/B/dir/START/SELECT/EXIT)
- Entering a game suspends WiFi and BLE; on exit only the services that were active before are resumed

### 3. Game Boy audio

- MiniGB APU emulates all four GB channels (square x2, wave, noise)
- ~268 mono samples per frame (16 kHz), I2S MONO slot output
- APU runs on its own task on Core 0, the emulator runs on Core 1, so audio synthesis truly runs in parallel and stays out of the frame budget
- Synced with the emulator, non-blocking output (drops samples when buffer is full to protect the frame rate)

### 4. Voice assistant (online)

```
User holds the key to record
    ↓
INMP441 I2S capture (16 kHz, 16-bit mono, up to 10 s, buffered in PSRAM)
    ↓
Baidu ASR API (dev_pid=80001 Mandarin)
    ↓
Recognised text fed to LLM (OpenAI-compatible API, max_tokens=128)
    ↓
Baidu TTS API (streaming WAV PCM)
    ↓
MAX98357A playback (RingBuffer + I2S DMA)
```

### 5. Voice command (offline ESP-SR)

ESP-SR MultiNet7 offline Chinese command words, no network required. Designed as a toggle (same UX as the Bluetooth toggle): tapping "Voice command" in the menu turns SR on or off. When a command is recognised, the device tears SR down completely and switches to the matching feature.

**Command list (9 entries, all prefixed with "打开" / "open" for better discrimination):**

| ID | Pinyin | Feature | Needs WiFi |
|----|--------|---------|------------|
| 1 | dakai huanjing jiance (open environment monitor) | Env monitor placeholder | No |
| 2 | dakai tianqi he riqi (open weather and date) | Weather lookup | Yes |
| 3 | dakai youxi (open game) | Open game list | No |
| 4 | dakai liaotian zhushou (open chat assistant) | Chat assistant | Yes |
| 5 | dakai yuyin zhushou (open voice assistant) | Online ASR + LLM + TTS | Yes |
| 6 | dakai lanya (open Bluetooth) | BLE provisioning | No |
| 7 | dakai yinyue (open music) | SD-card music player | No |
| 8 | dakai yinliang (open volume) | Volume control | No |
| 9 | dakai zhineng shebei (open smart device) | Bemfa Cloud control | Yes |

**Recognition → dispatch flow (5-state FSM):**

```
IDLE  ──toggle──▶ STARTING ──init+listen ok──▶ ACTIVE
                     │                            │
                     └─fail─▶ STOPPING ─▶ IDLE   ↓ command recognised
                                                DISPATCH
                                                   │
              ┌────────────────────────────────────┘
              ▼
   esp_sr_deinit (~2 s sync)
              │
   ┌─needs WiFi but offline at SR start─▶ show "please connect WiFi"
   │
   └─else ──▶ wifi_resume_after_game (if needed) ──▶ wait WiFi ready (≤8 s)
                                                     │
                              ┌──────────────────────┘
                              ▼
                  post sr_pending_action to LVGL ──▶ show_xxx_screen
                                                       │
                                                       ▼
                                                     IDLE
```

**Key design points:**

- **Disable noise suppression for higher accuracy**: NSNet2 is turned off (per Espressif's own guidance, NS hurts MultiNet accuracy)
- **State transitions only on the LVGL thread**: every `sr_state` write happens on LVGL; background tasks (start / dispatch / stop) only read, eliminating races between rapid toggling and async tasks
- **WiFi guard up front**: when a WiFi-requiring command is recognised offline, we show the "please connect WiFi" prompt and skip the open
- **Resource serialisation**: recognised → free SR memory (~100 KB) → resume WiFi → wait for IP → switch screen, so the target feature never fights the SR model/tasks for DRAM
- **Graceful failure**: if a worker (`malloc` / `xTaskCreate`) cannot start, we post a `dispatch_failed=true` action instead of doing a synchronous SR teardown on the LVGL thread; `execute_action` keeps the "SR off" dialog and only flips state back to IDLE
- **AFE warning suppressed**: `esp_log_level_set("AFE", ESP_LOG_ERROR)` hides the non-fatal "ringbuffer empty" warning during deinit
- **Shared I2S with online ASR**: handover via deinit/reinit; SR uses 32-bit stereo Philips (matches `asr.c`) and software-converts to 16-bit mono before feeding AFE

### 6. BLE provisioning & remote music control

NimBLE-based HM-10 compatible provisioning plus BLE music control:

**Provisioning protocol:**
- Phone scans and connects to `ESP32-Bot` (Service `0xFFE0`, Characteristic `0xFFE1`)
- Send `SSID_<name> password_<pass>` to auto-connect WiFi
- 50 ms idle timeout splits the packet, the result is sent back via BLE Notify
- After credentials arrive, BLE deinits to free DRAM, then WiFi connects

**Music control (commands prefixed with `/`):**
- `/music on` — scan SD-card music dir and notify back the indexed list
- `/music off` — async-stop background playback
- `/<index>` — play by list index (recommended, avoids encoding issues)
- `/<filename>` — prefix-match playback

**Implementation notes:**
- Non-blocking start: `ble_prov_start` does not poll for host sync; the `on_sync` callback drives advertising
- BLE is paused on game entry and resumed on exit
- All shared state is protected by a module-level mutex; pending counter prevents deinit-vs-notify races
- `s_deinit_in_progress` gate prevents double teardown; controller teardown has retry + timeout fallback
- Music stop runs in a one-shot task to avoid blocking the BLE worker (≤ 2 s)
- The song scan buffer is allocated in PSRAM (~4.6 KB) and freed once the command finishes

### 7. WiFi guardian

- First connect: EventGroup wait, up to 3 retries
- Runtime drop: a guardian task auto-reconnects with exponential back-off (5 s → 5 min)
- User-initiated disconnect stops the guardian (no auto-retry)
- Dynamic credentials supported (BLE provisioning writes them); writers and readers are mutex-protected
- All shared state (`s_status` / `s_user_stopped` / `s_backoff_idx`) is mutex-protected
- `wifi_copy_ip()` returns a locked IP snapshot to avoid torn reads

### 8. MicroSD card storage

SPI3_HOST drives the card in SPI mode and mounts FAT32 at `/sdcard`:

- Standard POSIX I/O (`fopen("/sdcard/...", "rb")`)
- SDSC and SDHC supported, capacity auto-detected
- Long Chinese filenames work (UTF-8 API + GBK code page 936)
- Mount/unmount is reentrant
- Independent from LCD (SPI2_HOST)
- Stores Game Boy ROMs at `/sdcard/rom/` and music at `/sdcard/music/`
- Game list always shows the built-in 2048 (no ROM file needed)

### 9. Music player

WAV and MP3 from SD card:

- **WAV**: RIFF/PCM 16-bit, 8–48 kHz, mono or stereo (auto down-mixed to mono)
- **MP3**: Helix fixed-point decoder (~5 KB working memory, fits PSRAM), auto-skips ID3v2 tags
- **Dynamic sample rate**: switch I2S clock to match the file before playback, restore 16 kHz afterwards (TTS / GB audio unaffected)
- **Controls**: play / pause / stop / skip; decoding runs in the background
- **Skip safety**: every stop/play goes through a one-shot task so LVGL never blocks; `music_stop` polls up to 2 s for the previous task to confirm exit
- **Error tolerance**: 32 consecutive MP3 frame failures abort to keep the decoder out of an invalid state
- Music dir: `/sdcard/music/`, English and Chinese filenames both work

### 10. Audio output

MAX98357A I2S class-D amp:

- 16 kHz / 16-bit / hardware MONO slot (no software stereo expansion)
- DMA `auto_clear`: zero output when idle, eliminating DC drift and pop noises
- DC-block HPF (~12.7 Hz cutoff) cancels MAX98357A DC offset
- Byte-tail alignment: HTTP chunked transfers may produce odd byte counts; we keep the trailing byte for the next chunk to avoid 16-bit sample misalignment
- Linear fade-in (64 samples / 4 ms) on first play and after flush to suppress click sounds
- `speaker_flush()` clears the ring buffer for instant silence (game exit / screen change)
- `speaker_set_sample_rate()` switches I2S clock dynamically (8–48 kHz); a flush handshake guarantees old-rate samples drain before the switch
- **Volume**: Q15 fixed-point logarithmic gain (0–100 % → -60 dB..0 dB), `volatile` atomic write for lockless playback, NVS-persisted with 500 ms debounce on slider drags

### 11. Smart device (Bemfa Cloud)

Controls smart devices bound to your Bemfa Cloud TCP account via HTTP REST:

- **List**: GET `/vb/api/v2/allTopic` for all topics, parses both flat and nested JSON variants
- **Single device status**: GET `/vb/api/v2/topicInfo` for one topic's latest msg / online — response < 200 B, much cheaper than re-pulling the full list (10 KB+) after every send
- **Send**: POST `/va/postJsonMsg` with `on` / `off` (or any custom message); the caller fully owns the payload — no inferring from cached state
- **Toggle helper**: `bemfa_toggle` is kept (auto-flips based on `current_msg`), but the UI now defaults to the explicit-send path
- **UX**: tapping a device opens a three-button "Open / Close / Cancel" dialog; on success the row optimistically flips state, then a 1.2 s delayed `topicInfo` query backfills the authoritative state; on failure the full list refreshes to restore truth
- **Thread safety**: a module mutex serialises HTTPS requests so two clients never race in mbedTLS
- **PSRAM dynamic buffer**: HTTP response starts at 4 KB, doubles up to 32 KB on demand, freed immediately after each call
- **Async UI**: HTTPS runs on PSRAM-stacked background tasks; a FreeRTOS Queue + lv_timer feeds results back to LVGL (zero blocking)
- **Lifecycle safety**: a mutex + active flag guard the list / send / info queue handles; results from in-flight tasks are dropped if the user has already left the screen

---

## System architecture

### Tasks

| Task | Priority | Stack | Notes |
|------|----------|-------|-------|
| `lv_tick` | 5 | 2048 B | LVGL tick (5 ms) |
| `lv_task` | 4 | 16384 B (DRAM) | LVGL render + flush |
| `enc_task` | 6 | 2048 B | SW button state machine (5 ms poll) |
| `spk_tx` | 3 | 4096 B | RingBuffer → I2S DMA |
| `asr_rec` | 5 | 2048 B | I2S recording resident task |
| `wifi_task` | 3 | 6144 B (PSRAM) | WiFi connect + NTP sync (one-shot) |
| `wifi_guard` | 4 | 3072 B | Reconnect guardian (resident) |
| `weather_task` | 3 | 16384 B (PSRAM) | Weather lookup (one-shot) |
| `chat_task` | 3 | 16384 B (PSRAM) | LLM request (one-shot) |
| `asr_task` | 3 | 16384 B (PSRAM) | ASR (one-shot) |
| `asr_llm` | 3 | 16384 B (PSRAM) | ASR → LLM → TTS (one-shot) |
| `game_run` | 10 | 12288 B (DRAM) | GB emulator main loop (Core 1) |
| `apu_task` | 5 | 4096 B | GB APU audio synthesis (Core 0) |
| `music` | 4 | 10240 B (DRAM) | SD-card music decode (one-shot) |
| `music_stop` | 3 | 2048 B (DRAM) | Async music stop (one-shot) |
| `psram_cleaner` | 2 | 3072 B | Recycles PSRAM task stacks and TCBs |
| `esp_sr_read` | 6 | 5120 B | ESP-SR I2S read + 16↔32 bit conversion (Core 0) |
| `esp_sr_feed` | 5 | 5120 B | ESP-SR AFE feed (Core 0) |
| `esp_sr_detect` | 5 | 6144 B | ESP-SR MultiNet detect (Core 1) |
| `kpad_task` | 6 | 4096 B | Keypad scan (2 ms period) |
| `bemfa_list` | 3 | 8192 B (PSRAM) | Bemfa device list HTTPS (one-shot) |
| `bemfa_send` | 3 | 8192 B (PSRAM) | Bemfa send on/off HTTPS (one-shot) |
| `bemfa_info` | 3 | 8192 B (PSRAM) | Bemfa single-device backfill (one-shot) |
| `ble_mstop` | 3 | 2048 B (DRAM) | Async music stop triggered by BLE (one-shot) |
| `health` | 1 | 2048 B | Heap monitor (60 s) |

### PSRAM tasks

`psram_task.c` provides `xTaskCreatePSRAM()` and `xTaskCreatePSRAMPinnedToCore()`: the task stack lives in PSRAM (saving internal DRAM) while the TCB stays in internal DRAM (FreeRTOS requirement). When the task function returns normally, the cleaner task reclaims the stack and TCB.

### Thread safety

- All LVGL object operations happen on the single `lv_task` thread
- Background tasks pass results via FreeRTOS Queues; an `lv_timer` polls and updates the UI
- `speaker_play()` uses non-blocking ring-buffer writes; TTS / GB audio call it directly
- HTTP response buffers are mutex-protected per module and freed back to PSRAM on completion
- WiFi / BLE / ESP-SR / music / Bemfa all guard shared state with module-level mutexes
- SPI bus isolation: LCD on SPI2_HOST, SD card on SPI3_HOST
- `speaker_set_sample_rate()` uses a flush handshake with the tx task so the ring is empty before the I2S clock changes
- `speaker_set_volume()` uses `volatile` + Q15 fixed-point multiply for lockless atomic effect on the playback path
- Music skip / stop runs in one-shot background tasks to keep LVGL unblocked

### LCD refresh

PARTIAL mode + DRAM double buffer + async DMA:

1. LVGL renders dirty regions into a DRAM buffer (34 lines = 16 KB chunks)
2. `disp_flush` queues the transfer with `spi_device_queue_trans`
3. Calls `lv_display_flush_ready()` immediately so LVGL can render the next chunk into the other buffer
4. The next flush waits for the previous DMA to complete

In game mode `lv_port_disp_suspend()` pauses LVGL output and the game runtime takes the SPI bus.

---

## Quick start

### Requirements

- ESP-IDF v5.4+ (v5.4.3 recommended)
- ESP32-S3 target
- MicroSD card (FAT32) for ROM and music files

### Steps

**1. Clone**

```bash
git clone <repo-url>
cd bot
```

**2. Fill in configs**

```bash
cp user/asr_config.h.example user/asr_config.h
cp user/model_config.h.example user/model_config.h
cp user/bemfa_config.h.example user/bemfa_config.h
```

Edit `user/asr_config.h` with Baidu AI API key and secret.
Edit `user/model_config.h` with the LLM API key, URL, and model name.
Edit `user/bemfa_config.h` with your Bemfa private key (cloud.bemfa.com → personal centre → copy "私钥").

**3. Prepare the SD card**

Format as FAT32 and create:

```
/sdcard/
├── rom/          # .gb / .gbc files
└── music/        # .wav / .mp3 files (16-bit PCM WAV or MP3)
```

> Note: the actual ROM directory is `/sdcard/rom` (no trailing s), not `/sdcard/roms`.

**4. Build and flash**

```bash
idf.py set-target esp32s3
idf.py build
# Windows:
idf.py -p COM5 -b 2000000 flash
# Linux:
idf.py -p /dev/ttyUSB0 -b 2000000 flash
```

> Linux: if `/dev/ttyUSB0` permission is denied, `sudo usermod -aG dialout $USER` (then re-login) or `sudo chmod 666 /dev/ttyUSB0`.

> The project supports Windows and Linux side-by-side: `.vscode/settings.json` and `.vscode/tasks.json` keep both setups, Linux paths are active and Windows paths are kept as comments.

---

## Configuration

### sdkconfig.defaults (already set)

| Option | Value | Notes |
|--------|-------|-------|
| CPU freq | 240 MHz | Full speed |
| Flash mode | QIO 80 MHz | ~2x code-fetch vs DIO |
| Flash size | 16 MB | N16R8 board |
| PSRAM | Octal 80 MHz | 8 MB Octal PSRAM |
| SPIRAM_RODATA | enabled | Read-only constants in PSRAM, frees ~80 KB internal DRAM |
| Compile opt | `-Os` | Size-first, ~10–20 % smaller code |
| WiFi buffers | PSRAM | Static RX trimmed to 4, dynamic buffers in PSRAM |
| mbedTLS | PSRAM | SSL allocations in PSRAM, less internal DRAM fragmentation |
| Bluetooth LE | NimBLE | ~40 KB DRAM saved vs Bluedroid; peripheral only |
| BT/WiFi coexistence | enabled | Required for BLE + WiFi simultaneous operation |
| ESP-SR | MultiNet7 CN | Offline Chinese commands; NSNet2 disabled for accuracy; VADNet1 enabled |
| FATFS LFN | heap allocated | Chinese filenames (UTF-8 API + GBK 936) |
| FreeRTOS HZ | 1000 | 1 ms tick |

### Partitions (partitions.csv)

| Partition | Size | Purpose |
|-----------|------|---------|
| app0 | 6.5 MB | OTA slot 0 |
| app1 | 6.5 MB | OTA slot 1 |
| model | 3 MB | ESP-SR model data (SPIFFS subtype) |
| nvs | 20 KB | WiFi credentials, volume, etc. |
| otadata | 8 KB | OTA state |

ROM and music files live on the SD card, not in flash SPIFFS.

### user/asr_config.h

| Macro | Description |
|-------|-------------|
| `BAIDU_API_KEY` | Baidu AI app API key |
| `BAIDU_SECRET_KEY` | Baidu AI app secret |
| `MIC_SCK_PIN` | INMP441 BCLK (default GPIO38) |
| `MIC_WS_PIN` | INMP441 LRCK (default GPIO39) |
| `MIC_SD_PIN` | INMP441 DATA (default GPIO40) |
| `MIC_SAMPLE_RATE` | Sample rate (default 16 kHz) |
| `MIC_MAX_SECONDS` | Max recording length (default 10 s) |

### user/model_config.h

| Macro | Description |
|-------|-------------|
| `MODEL_API_KEY` | LLM API key |
| `MODEL_API_URL` | Endpoint URL (OpenAI-compatible) |
| `MODEL_NAME` | Model name (e.g. `deepseek-chat`) |

### user/bemfa_config.h

| Macro | Description |
|-------|-------------|
| `BEMFA_UID` | Bemfa private key (openID) |
| `BEMFA_TYPE` | Device protocol type (fixed `3` = TCP cloud) |

---

## Notes

- **Credential safety**: `asr_config.h`, `model_config.h`, `bemfa_config.h` carry secrets and are `.gitignore`-d
- **PSRAM must be Octal**: recording buffer, ROM data, HTTP responses, music decode buffer all live in PSRAM
- **I2S allocation**: I2S_NUM_0 = mic (RX), I2S_NUM_1 = speaker (TX); they don't overlap
- **I2S_NUM_0 sharing**: online ASR and offline ESP-SR hand off via deinit/reinit + taskNotify
- **LCD SPI clock**: 80 MHz with PARTIAL mode async DMA flush
- **SPI bus isolation**: LCD on SPI2_HOST, SD card on SPI3_HOST
- **SD CS pin**: GPIO0 (boot button); don't hold it at power-on. The original GPIO48 was abandoned because the on-board WS2812 LED disturbed the CS line
- **ESP32-S3 Bluetooth limit**: BLE only, no Classic BT (so A2DP is unavailable)
- **Game mode**: entering a game suspends WiFi + BLE; on exit only the previously-active services are resumed
- **Audio safety**: DC-block HPF removes DC offset, byte-tail alignment prevents PCM misalignment, DMA `auto_clear` removes idle noise
- **SD card required**: GB ROMs and music files come from the card; without one, music and emulator are unavailable (built-in 2048 still works)
- **Music formats**: WAV must be 16-bit PCM (8–48 kHz); MP3 is decoded by Helix (MPEG-1/2 Layer III)

---

## Engineering highlights

### Thread-safe design

In a multitasking ESP32-S3 environment shared-state races are the most common crash cause. The project uses layered defences:

- **Module-level mutexes**: WiFi, BLE, Speaker, music player, Bemfa, LLM, weather all keep their own mutex protecting internal state from being torn between event callbacks, guardians and the UI task. Every mutex is created in the single-threaded `app_main` phase to remove the lazy-init race
- **Volatile lockless reads**: hot paths like `wifi_get_status()` and `speaker_get_volume()` use `volatile` + 32-bit aligned atomics so we don't slow down UI/HTTP paths
- **Copy-under-lock**: `baidu_token_copy()` copies the token to the caller's stack buffer while the mutex is held; once the lock is released the caller works on its own copy so cross-thread pointer dangling is impossible
- **Flush handshake**: Speaker's `speaker_flush()` / `speaker_set_sample_rate()` doesn't touch the ring buffer directly; instead it raises `s_flush_request` and waits on `s_flush_done` so the ring buffer keeps a single consumer (the tx task)
- **Cooperative task exit**: the WiFi guardian exits via a notification rather than `vTaskDelete` to avoid being killed mid-mutex
- **Async UI ops**: music play/stop/skip run in one-shot tasks (`music_stop_task` / `music_play_task`) so the 2-second wait doesn't block LVGL; if task creation fails we fall back to synchronous calls

### Memory discipline

ESP32-S3 has only ~338 KB internal DRAM, so the project squeezes hard:

- **PSRAM-first**: HTTP responses, recording, ROM data, music decoding, WiFi/LwIP dynamic buffers all live in PSRAM; internal DRAM is reserved for LVGL double buffers and FreeRTOS task stacks
- **Use-then-release**: ASR HTTP buffer is freed via `release_http_buf()` after every recognition (next call starts at 4 KB again); baidu_token / model / weather all run `resp_buf_release()` before returning; the music scan buffer is freed when leaving the music screen
- **Dynamic grow + immediate shrink**: HTTP response starts at 4 KB, doubles up to 32 KB, and is dropped to NULL once the API returns
- **NVS write debounce**: volume slider drags coalesce via a 500 ms timer (no thrash on flash)
- **Lazy load BLE / ESP-SR**: both initialise only when the user opens the screen; on exit they deinit and ~100 KB DRAM comes back

### I2S resource sharing

I2S_NUM_0 is shared between online ASR (`asr.c`) and offline ESP-SR (`esp_sr.c`) with a safe handover:

- `asr_mic_deinit()` first sets `s_rec_active = false`, then waits on `s_stop_waiter` for the recording task to acknowledge the loop exit, only then calls `i2s_del_channel`. This avoids ripping the channel out from under a blocking `i2s_channel_read` (which would HardFault)
- `asr_mic_reinit()` rebuilds the channel; the resident recording task simply continues, no task recreation

### Audio robustness

- **Byte-tail alignment**: HTTP chunked transfers can deliver an odd number of bytes; `speaker_play` keeps the trailing byte for the next chunk so 16-bit samples don't get misaligned into noise
- **DC-block HPF**: 1st-order high-pass (~12.7 Hz @ 16 kHz) cancels MAX98357A DC offset, Q15 fixed-point so no floating-point cost
- **Linear fade-in**: 64 samples ramped up linearly after every flush or first play, eliminates click sounds
- **Dynamic sample rate**: before music playback the flush handshake clears the ring + disables I2S → reconfigures the clock → re-enables, so old-rate samples never get replayed at the new rate; afterwards we restore 16 kHz so TTS / GB audio aren't affected
- **MP3 fault tolerance**: 32 consecutive failed frames abort decode (prevents Helix from being driven into illegal state); ID3v2 tags are skipped to avoid false sync; after `memmove` the read pointer always stays inside valid data so short-read scenarios don't read uninitialised bytes

### System-level robustness

- **WiFi guardian**: first-connect EventGroup wait + 3 quick retries; runtime drops trigger exponential back-off (5 s → 5 min) and never give up; a user-initiated disconnect stops the guardian. `wifi_full_shutdown_for_ble()` notifies the guardian via `GUARDIAN_NOTIFY_SHUTDOWN` so it exits cooperatively, avoiding `vTaskDelete` deadlocks while the mutex is held
- **BLE deinit guards**: `s_deinit_in_progress` blocks double teardown; `s_pending_host_calls` waits for cross-task NimBLE calls before tearing down the host; `s_adv_gen` generation counter prevents ghost advertising after stop
- **BLE timer race fix**: the `xTimerReset` inside `accumulate_rx` sits inside the LOCK block, mutually exclusive with the `s_rx_timer = NULL` write in the deinit path → no UAF window
- **Task lifecycle**: ESP-SR's three tasks (read/feed/detect) use graceful stop + 2 s timeout hard-kill + ring-buffer leftover protection. The music player's `music_stop` polls 10 × 200 ms to confirm the previous task exited, preventing zombie tasks during rapid skipping. ESP-SR's upper layer uses a 5-state FSM (IDLE / STARTING / ACTIVE / DISPATCH / STOPPING) to manage toggle, dispatch, and user-stop flows; every `sr_state` change happens on the LVGL thread, eliminating double-tap races and worker-led IDLE transitions that would let a dispatch action steal the screen
- **NULL-guard everywhere**: every `malloc` / `xTaskCreatePSRAM` return value is checked; on failure the UI shows "out of memory" rather than crashing
- **Async cancellation**: weather lookup supports true cancellation (`s_weather_cancelled` flag + queue drain + double check); even if the background task finishes after cancel, the dialog never appears
- **LVGL object lifecycle**: BLE result dialogs route every cleanup path (OK button / parent screen change / explicit delete) through one `LV_EVENT_DELETE` handler that nulls the global handle, killing dangling pointers
- **HTTP buffer use-and-release**: baidu_token / model / weather all call `resp_buf_release()` before each API returns, so PSRAM doesn't sit on stale buffers
- **Module init in single-thread phase**: `baidu_token_init()` / `bemfa_init()` / `model_init()` / `weather_init()` / `music_init()` run inside `app_main` so we never race-create multiple mutexes on first concurrent calls
- **baidu_token thread-safe copy**: the new `baidu_token_copy()` copies the token to the caller's buffer under lock; ASR/TTS use it so the released token can never be overwritten by another thread mid-read
- **WiFi guardian cooperative exit**: `wifi_full_shutdown_for_ble()` no longer hard-kills the guardian; it sends `GUARDIAN_NOTIFY_SHUTDOWN` and waits for confirmation before tearing the WiFi driver down
- **PSRAM task handle race**: `task_entry` starts with `taskYIELD()` so the creator finishes assigning `cleanup.handle` before the new task runs; otherwise a high-priority task could finish first and feed the cleaner an uninitialised handle

---

## Code stats

| Category | Lines |
|----------|-------|
| Hand-written code | ~6,400 |
| Font data (lv_font_simhei_16 + lcdfont) | ~385,000 |
| Emulator libraries (walnut_cgb.h + minigb_apu.c) | ~10,500 |
| LVGL library | not counted |

---

## Memory budget (ESP32-S3 N16R8)

### Flash (16 MB)

| Content | Size |
|---------|------|
| Firmware (ESP-IDF + LVGL + app + GB emu + ESP-SR + NimBLE + Helix MP3 + FATFS) | ~4.7 MB |
| ESP-SR model partition | 3 MB |
| OTA backup partition | 6.5 MB |

### PSRAM (8 MB Octal)

| Use | Size |
|-----|------|
| ASR recording buffer | ~320 KB (released after recognition) |
| HTTP response buffer (dynamic) | 4–32 KB (released after each call) |
| ROM data (during gameplay) | 32 KB – 2 MB |
| Music decode buffer (WAV / MP3) | ~16 KB (during playback) |
| WiFi / LwIP buffers | ~50 KB |
| BSS + rodata (external segment) | ~100 KB |
| Free | **~7 MB** |

### Internal DRAM (~338 KB usable)

| Use | Size |
|-----|------|
| LVGL PARTIAL double buffer (34 lines x 2) | ~32 KB |
| Speaker RingBuffer | 64 KB |
| WiFi / LwIP / mbedTLS runtime | ~100 KB |
| FreeRTOS task stacks + TCBs | ~35 KB |
| Free | **~107 KB** |
