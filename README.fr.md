# Bot — Assistant vocal IA ESP32-S3 & Émulateur Game Boy

> 🌐 Langue : [简体中文](README.md) | [English](README.en.md) | **Français** | [Español](README.es.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

Projet embarqué ESP32-S3 basé sur ESP-IDF + LVGL, intégrant WiFi, météo, chat IA, reconnaissance vocale en ligne/hors ligne, synthèse vocale, émulateur Game Boy avec audio, provisionnement BLE et contrôle musical à distance, lecteur de musique MicroSD (WAV + MP3), contrôle du volume et contrôle d'appareils intelligents Bemfa Cloud. Interface graphique TFT LCD avec encodeur rotatif et matrice de touches.

---

## Matériel

| Composant | Modèle / Spec | Notes |
|-----------|---------------|-------|
| MCU | ESP32-S3-DevKitC-1 N16R8 | 16 Mo Flash + 8 Mo PSRAM Octal |
| Écran | TFT LCD 2,4", 240x320, ILI9341 | SPI, 80 MHz |
| Microphone | INMP441 | Micro numérique I2S, I2S_NUM_0 |
| Haut-parleur | MAX98357A + HP | Ampli classe D I2S, I2S_NUM_1 |
| Entrée | Encodeur rotatif (A/B/SW) | Décodage matériel PCNT + polling GPIO |
| Boutons | Matrice 3x3 (9 touches) | D-pad jeu + A/B/START/SELECT/EXIT |
| Stockage | Carte MicroSD (mode SPI) | SPI3_HOST, FAT32 |

---

## Brochage

### LCD (SPI2, ILI9341)

| Fonction | Broche ESP32-S3 |
|----------|-----------------|
| MOSI | GPIO11 |
| SCLK | GPIO12 |
| RES  | GPIO10 |
| DC   | GPIO9  |
| BLK  | GPIO46 |

### Microphone INMP441 (I2S_NUM_0)

| Fonction | ESP32-S3 | INMP441 |
|----------|----------|---------|
| BCLK | GPIO38 | SCK |
| LRCK | GPIO39 | WS |
| DATA | GPIO40 | SD |

### Amplificateur MAX98357A (I2S_NUM_1)

| Fonction | ESP32-S3 | MAX98357A |
|----------|----------|-----------|
| BCLK | GPIO15 | BCLK |
| LRCK | GPIO16 | LRC |
| DATA | GPIO17 | DIN |

### Encodeur rotatif

| Fonction | Broche |
|----------|--------|
| A | GPIO4 |
| B | GPIO5 |
| SW | GPIO6 |

### Matrice de touches 3x3

| Ligne/Col | GPIO41 (C0) | GPIO42 (C1) | GPIO47 (C2) |
|-----------|-------------|-------------|-------------|
| GPIO1 (R0)  | B | HAUT | A |
| GPIO2 (R1)  | GAUCHE | EXIT | DROITE |
| GPIO14 (R2) | SELECT | BAS | START |

### MPU6050 (I2C_NUM_0)

| Fonction | ESP32-S3 | MPU6050 |
|----------|----------|---------|
| SDA | GPIO20 | SDA |
| SCL | GPIO7  | SCL |

I2C 400 kHz, plage ±2 g, DLPF 44 Hz. Initialisé uniquement pour le jeu 2048, libéré à la sortie. Détection d'inclinaison avec hystérésis (ENTER 0,30 g, EXIT 0,15 g) + verrouillage de direction.

### Carte MicroSD (SPI3_HOST)

| Fonction | ESP32-S3 |
|----------|----------|
| CS   | GPIO0  |
| MOSI | GPIO8  |
| SCK  | GPIO18 |
| MISO | GPIO21 |

> Note : CS utilise GPIO0 (bouton Boot) ; ne pas maintenir appuyé au démarrage.

---

## Fonctionnalités

### 1. Menu principal

Au démarrage, la liste des fonctions s'affiche. Navigation par encodeur rotatif, confirmation par pression :

- **Surveillance environnementale** — Espace réservé (en développement)
- **Météo & date** — Récupération météo HTTP en temps réel
- **Jeux** — 2048 intégré + émulateur Game Boy (ROMs depuis `/sdcard/rom/`)
- **Assistant chat** — Clavier à l'écran, appel API LLM, historique défilant
- **Assistant vocal** — Enregistrement → Baidu ASR → LLM → Baidu TTS → lecture
- **Commande vocale** — Reconnaissance hors ligne ESP-SR (bascule on/off, pas d'écran dédié)
- **Bluetooth** — Provisionnement WiFi via BLE + contrôle musical à distance
- **Musique** — Lecture WAV/MP3 depuis `/sdcard/music/`
- **Volume** — Curseur 0–100 %, courbe logarithmique, persisté en NVS
- **Appareils intelligents** — Contrôle Bemfa Cloud : liste, envoi on/off explicite

### 2. Émulateur Game Boy

- Basé sur Walnut-CGB (réécriture haute performance de Peanut-GB), supporte DMG + CGB
- Résolution native 160x144, mise à l'échelle 1,5x vers 240x216
- Rendu DMA asynchrone double tampon à 80 MHz SPI
- ROMs chargées depuis la carte SD vers PSRAM (jusqu'à 4 Mo)
- 2048 intégré toujours visible en tête de liste
- Contrôle par inclinaison MPU6050 pour 2048 (coexiste avec les touches)
- Entrée jeu complète via matrice 3x3 (A/B/directions/START/SELECT/EXIT)
- WiFi et BLE suspendus à l'entrée du jeu, restaurés à la sortie

### 3. Assistant vocal (en ligne)

```
Appui touche → enregistrement INMP441 (16 kHz, 16 bits mono, max 10 s, PSRAM)
    ↓
API Baidu ASR (mandarin)
    ↓
Texte reconnu → LLM (compatible OpenAI, max_tokens=128)
    ↓
API Baidu TTS (PCM WAV en streaming)
    ↓
Lecture MAX98357A (RingBuffer + I2S DMA)
```

### 4. Commande vocale (ESP-SR hors ligne)

MultiNet7 ESP-SR, 9 commandes préfixées "打开" (ouvrir) :

| ID | Commande | Fonction | WiFi requis |
|----|----------|----------|-------------|
| 1 | Ouvrir surveillance env. | Surveillance | Non |
| 2 | Ouvrir météo et date | Météo | Oui |
| 3 | Ouvrir jeux | Jeux | Non |
| 4 | Ouvrir assistant chat | Chat | Oui |
| 5 | Ouvrir assistant vocal | ASR+LLM+TTS | Oui |
| 6 | Ouvrir Bluetooth | BLE | Non |
| 7 | Ouvrir musique | Musique | Non |
| 8 | Ouvrir volume | Volume | Non |
| 9 | Ouvrir appareils intelligents | Bemfa | Oui |

Machine à états à 5 états (IDLE / STARTING / ACTIVE / DISPATCH / STOPPING). Toutes les transitions d'état se font sur le thread LVGL.

### 5. Provisionnement BLE & contrôle musical

- Connexion BLE à "ESP32-Bot" (Service 0xFFE0, Caractéristique 0xFFE1)
- Envoi `SSID_nom password_mdp` pour configurer le WiFi
- Commandes musicales : `/music on`, `/music off`, `/<index>`

### 6. Appareils intelligents (Bemfa Cloud)

- **Liste** : GET `/vb/api/v2/allTopic`
- **État unitaire** : GET `/vb/api/v2/topicInfo` (< 200 o, plus rapide que la liste complète)
- **Envoi** : POST `/va/postJsonMsg` avec `on` / `off`
- **UX** : clic sur un appareil → dialogue "Ouvrir / Fermer / Annuler" → mise à jour optimiste immédiate → vérification d'état après 1,2 s

---

## Architecture système

### Tâches principales

| Tâche | Priorité | Pile | Notes |
|-------|----------|------|-------|
| `lv_tick` | 5 | 2048 o | Horloge LVGL (5 ms) |
| `lv_task` | 4 | 16384 o (DRAM) | Rendu LVGL |
| `spk_tx` | 3 | 4096 o | RingBuffer → I2S DMA |
| `wifi_guard` | 4 | 3072 o | Reconnexion automatique |
| `game_run` | 10 | 12288 o (DRAM) | Boucle principale émulateur (Core 1) |
| `apu_task` | 5 | 4096 o | Synthèse audio GB (Core 0) |
| `esp_sr_read/feed/detect` | 6/5/5 | 5120/5120/6144 o | Pipeline ESP-SR |
| `bemfa_list/send/info` | 3 | 8192 o (PSRAM) | Requêtes HTTPS Bemfa |
| `health` | 1 | 2048 o | Surveillance mémoire (60 s) |

---

## Démarrage rapide

### Prérequis

- ESP-IDF v5.4+ (v5.4.3 recommandé)
- Cible ESP32-S3
- Carte MicroSD (FAT32)

### Étapes

```bash
git clone <repo-url>
cd bot
cp user/asr_config.h.example user/asr_config.h
cp user/model_config.h.example user/model_config.h
cp user/bemfa_config.h.example user/bemfa_config.h
# Remplir les clés API dans chaque fichier
idf.py set-target esp32s3
idf.py build
# Linux :
idf.py -p /dev/ttyUSB0 -b 2000000 flash
# Windows :
idf.py -p COM5 -b 2000000 flash
```

Structure de la carte SD :
```
/sdcard/
├── rom/     # fichiers .gb / .gbc
└── music/   # fichiers .wav / .mp3
```

---

## Configuration (sdkconfig.defaults)

| Option | Valeur | Notes |
|--------|--------|-------|
| Fréquence CPU | 240 MHz | Pleine vitesse |
| Mode Flash | QIO 80 MHz | ~2x vs DIO |
| Taille Flash | 16 Mo | Carte N16R8 |
| PSRAM | Octal 80 MHz | 8 Mo |
| Bluetooth LE | NimBLE | ~40 Ko DRAM économisés vs Bluedroid |
| ESP-SR | MultiNet7 CN | NSNet2 désactivé pour meilleure précision |
| FreeRTOS HZ | 1000 | Tick 1 ms |

---

## Budget mémoire (ESP32-S3 N16R8)

### Flash (16 Mo)
| Contenu | Taille |
|---------|--------|
| Firmware complet | ~4,7 Mo |
| Partition modèle ESP-SR | 3 Mo |
| Partition OTA de secours | 6,5 Mo |

### PSRAM (8 Mo Octal)
| Usage | Taille |
|-------|--------|
| Tampon enregistrement ASR | ~320 Ko (libéré après reconnaissance) |
| Tampon réponse HTTP | 4–32 Ko (libéré après chaque appel) |
| Données ROM (pendant le jeu) | 32 Ko – 2 Mo |
| Libre | **~7 Mo** |

### DRAM interne (~338 Ko)
| Usage | Taille |
|-------|--------|
| Double tampon LVGL | ~32 Ko |
| RingBuffer haut-parleur | 64 Ko |
| WiFi / LwIP / mbedTLS | ~100 Ko |
| Libre | **~107 Ko** |
