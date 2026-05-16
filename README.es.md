# Bot — Asistente de voz IA ESP32-S3 y Emulador Game Boy

> 🌐 Idioma: [简体中文](README.md) | [English](README.en.md) | [Français](README.fr.md) | **Español** | [Русский](README.ru.md) | [العربية](README.ar.md)

Proyecto embebido ESP32-S3 basado en ESP-IDF + LVGL que integra WiFi, consulta del tiempo, chat IA, reconocimiento de voz en línea/sin conexión, síntesis de voz, emulador Game Boy con audio, aprovisionamiento BLE y control musical remoto, reproductor de música MicroSD (WAV + MP3), control de volumen y control de dispositivos inteligentes Bemfa Cloud. Interfaz gráfica TFT LCD con encoder rotativo y matriz de teclas.

---

## Hardware

| Componente | Modelo / Especificación | Notas |
|------------|------------------------|-------|
| MCU | ESP32-S3-DevKitC-1 N16R8 | 16 MB Flash + 8 MB PSRAM Octal |
| Pantalla | TFT LCD 2,4", 240x320, ILI9341 | SPI, 80 MHz |
| Micrófono | INMP441 | Micrófono digital I2S, I2S_NUM_0 |
| Altavoz | MAX98357A + altavoz | Amplificador clase D I2S, I2S_NUM_1 |
| Entrada | Encoder rotativo (A/B/SW) | Decodificación hardware PCNT + polling GPIO |
| Botones | Matriz 3x3 (9 teclas) | D-pad juego + A/B/START/SELECT/EXIT |
| Almacenamiento | Tarjeta MicroSD (modo SPI) | SPI3_HOST, FAT32 |

---

## Conexiones

### LCD (SPI2, ILI9341)

| Función | Pin ESP32-S3 |
|---------|--------------|
| MOSI | GPIO11 |
| SCLK | GPIO12 |
| RES  | GPIO10 |
| DC   | GPIO9  |
| BLK  | GPIO46 |

### Micrófono INMP441 (I2S_NUM_0)

| Función | ESP32-S3 | INMP441 |
|---------|----------|---------|
| BCLK | GPIO38 | SCK |
| LRCK | GPIO39 | WS |
| DATA | GPIO40 | SD |

### Amplificador MAX98357A (I2S_NUM_1)

| Función | ESP32-S3 | MAX98357A |
|---------|----------|-----------|
| BCLK | GPIO15 | BCLK |
| LRCK | GPIO16 | LRC |
| DATA | GPIO17 | DIN |

### Encoder rotativo

| Función | Pin |
|---------|-----|
| A | GPIO4 |
| B | GPIO5 |
| SW | GPIO6 |

### Matriz de teclas 3x3

| Fila/Col | GPIO41 (C0) | GPIO42 (C1) | GPIO47 (C2) |
|----------|-------------|-------------|-------------|
| GPIO1 (R0)  | B | ARRIBA | A |
| GPIO2 (R1)  | IZQUIERDA | EXIT | DERECHA |
| GPIO14 (R2) | SELECT | ABAJO | START |

### MPU6050 (I2C_NUM_0)

| Función | ESP32-S3 | MPU6050 |
|---------|----------|---------|
| SDA | GPIO20 | SDA |
| SCL | GPIO7  | SCL |

I2C 400 kHz, rango ±2 g, DLPF 44 Hz. Inicializado solo para el juego 2048, liberado al salir. Detección de inclinación con histéresis (ENTER 0,30 g, EXIT 0,15 g) + bloqueo de dirección.

### Tarjeta MicroSD (SPI3_HOST)

| Función | ESP32-S3 |
|---------|----------|
| CS   | GPIO0  |
| MOSI | GPIO8  |
| SCK  | GPIO18 |
| MISO | GPIO21 |

> Nota: CS usa GPIO0 (botón Boot); no mantener presionado al encender.

---

## Funcionalidades

### 1. Menú principal

Al arrancar se muestra la lista de funciones. Navegación con encoder rotativo, confirmación con pulsación:

- **Monitor ambiental** — Marcador de posición (en desarrollo)
- **Tiempo y fecha** — Consulta meteorológica HTTP en tiempo real
- **Juegos** — 2048 integrado + emulador Game Boy (ROMs desde `/sdcard/rom/`)
- **Asistente de chat** — Teclado en pantalla, llamada API LLM, historial desplazable
- **Asistente de voz** — Grabación → Baidu ASR → LLM → Baidu TTS → reproducción
- **Comando de voz** — Reconocimiento sin conexión ESP-SR (interruptor on/off, sin pantalla dedicada)
- **Bluetooth** — Aprovisionamiento WiFi vía BLE + control musical remoto
- **Música** — Reproducción WAV/MP3 desde `/sdcard/music/`
- **Volumen** — Deslizador 0–100 %, curva logarítmica, persistido en NVS
- **Dispositivos inteligentes** — Control Bemfa Cloud: lista, envío on/off explícito

### 2. Emulador Game Boy

- Basado en Walnut-CGB (reescritura de alto rendimiento de Peanut-GB), soporta DMG + CGB
- Resolución nativa 160x144, escalado 1,5x a 240x216
- Renderizado DMA asíncrono doble búfer a 80 MHz SPI
- ROMs cargadas desde tarjeta SD a PSRAM (hasta 4 MB)
- 2048 integrado siempre visible al inicio de la lista
- Control por inclinación MPU6050 para 2048 (coexiste con teclas)
- Entrada de juego completa vía matriz 3x3 (A/B/direcciones/START/SELECT/EXIT)
- WiFi y BLE suspendidos al entrar al juego, restaurados al salir

### 3. Asistente de voz (en línea)

```
Pulsación tecla → grabación INMP441 (16 kHz, 16 bits mono, máx 10 s, PSRAM)
    ↓
API Baidu ASR (mandarín)
    ↓
Texto reconocido → LLM (compatible OpenAI, max_tokens=128)
    ↓
API Baidu TTS (PCM WAV en streaming)
    ↓
Reproducción MAX98357A (RingBuffer + I2S DMA)
```

### 4. Comando de voz (ESP-SR sin conexión)

MultiNet7 ESP-SR, 9 comandos prefijados con "打开" (abrir):

| ID | Comando | Función | WiFi requerido |
|----|---------|---------|----------------|
| 1 | Abrir monitor ambiental | Monitor | No |
| 2 | Abrir tiempo y fecha | Tiempo | Sí |
| 3 | Abrir juegos | Juegos | No |
| 4 | Abrir asistente de chat | Chat | Sí |
| 5 | Abrir asistente de voz | ASR+LLM+TTS | Sí |
| 6 | Abrir Bluetooth | BLE | No |
| 7 | Abrir música | Música | No |
| 8 | Abrir volumen | Volumen | No |
| 9 | Abrir dispositivos inteligentes | Bemfa | Sí |

Máquina de estados de 5 estados (IDLE / STARTING / ACTIVE / DISPATCH / STOPPING). Todas las transiciones de estado ocurren en el hilo LVGL.

### 5. Aprovisionamiento BLE y control musical

- Conexión BLE a "ESP32-Bot" (Servicio 0xFFE0, Característica 0xFFE1)
- Enviar `SSID_nombre password_contraseña` para configurar WiFi
- Comandos musicales: `/music on`, `/music off`, `/<índice>`

### 6. Dispositivos inteligentes (Bemfa Cloud)

- **Lista**: GET `/vb/api/v2/allTopic`
- **Estado unitario**: GET `/vb/api/v2/topicInfo` (< 200 B, más rápido que lista completa)
- **Envío**: POST `/va/postJsonMsg` con `on` / `off`
- **UX**: clic en dispositivo → diálogo "Abrir / Cerrar / Cancelar" → actualización optimista inmediata → verificación de estado tras 1,2 s

---

## Arquitectura del sistema

### Tareas principales

| Tarea | Prioridad | Pila | Notas |
|-------|-----------|------|-------|
| `lv_tick` | 5 | 2048 B | Reloj LVGL (5 ms) |
| `lv_task` | 4 | 16384 B (DRAM) | Renderizado LVGL |
| `spk_tx` | 3 | 4096 B | RingBuffer → I2S DMA |
| `wifi_guard` | 4 | 3072 B | Reconexión automática |
| `game_run` | 10 | 12288 B (DRAM) | Bucle principal emulador (Core 1) |
| `apu_task` | 5 | 4096 B | Síntesis audio GB (Core 0) |
| `esp_sr_read/feed/detect` | 6/5/5 | 5120/5120/6144 B | Pipeline ESP-SR |
| `bemfa_list/send/info` | 3 | 8192 B (PSRAM) | Solicitudes HTTPS Bemfa |
| `health` | 1 | 2048 B | Monitoreo de memoria (60 s) |

---

## Inicio rápido

### Requisitos

- ESP-IDF v5.4+ (v5.4.3 recomendado)
- Objetivo ESP32-S3
- Tarjeta MicroSD (FAT32)

### Pasos

```bash
git clone <repo-url>
cd bot
cp user/asr_config.h.example user/asr_config.h
cp user/model_config.h.example user/model_config.h
cp user/bemfa_config.h.example user/bemfa_config.h
# Rellenar las claves API en cada archivo
idf.py set-target esp32s3
idf.py build
# Linux:
idf.py -p /dev/ttyUSB0 -b 2000000 flash
# Windows:
idf.py -p COM5 -b 2000000 flash
```

Estructura de la tarjeta SD:
```
/sdcard/
├── rom/     # archivos .gb / .gbc
└── music/   # archivos .wav / .mp3
```

---

## Configuración (sdkconfig.defaults)

| Opción | Valor | Notas |
|--------|-------|-------|
| Frecuencia CPU | 240 MHz | Velocidad máxima |
| Modo Flash | QIO 80 MHz | ~2x vs DIO |
| Tamaño Flash | 16 MB | Placa N16R8 |
| PSRAM | Octal 80 MHz | 8 MB |
| Bluetooth LE | NimBLE | ~40 KB DRAM ahorrados vs Bluedroid |
| ESP-SR | MultiNet7 CN | NSNet2 desactivado para mayor precisión |
| FreeRTOS HZ | 1000 | Tick 1 ms |

---

## Presupuesto de memoria (ESP32-S3 N16R8)

### Flash (16 MB)
| Contenido | Tamaño |
|-----------|--------|
| Firmware completo | ~4,7 MB |
| Partición modelo ESP-SR | 3 MB |
| Partición OTA de respaldo | 6,5 MB |

### PSRAM (8 MB Octal)
| Uso | Tamaño |
|-----|--------|
| Búfer grabación ASR | ~320 KB (liberado tras reconocimiento) |
| Búfer respuesta HTTP | 4–32 KB (liberado tras cada llamada) |
| Datos ROM (durante el juego) | 32 KB – 2 MB |
| Libre | **~7 MB** |

### DRAM interna (~338 KB)
| Uso | Tamaño |
|-----|--------|
| Doble búfer LVGL | ~32 KB |
| RingBuffer altavoz | 64 KB |
| WiFi / LwIP / mbedTLS | ~100 KB |
| Libre | **~107 KB** |

---

## Licencia

Este proyecto se publica bajo la [Licencia MIT](LICENSE).

Los componentes de terceros conservan sus licencias originales (LVGL / Peanut-GB / MiniGB APU / cJSON — MIT; ESP-IDF — Apache 2.0; Helix MP3 — RealNetworks RPSL, gratuito para uso no comercial).
