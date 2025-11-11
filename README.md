# Multifunction Speaker (ESP32 + ESP-ADF)

![multifunction speaker](https://github.com/michalizn/multifunction_speaker/blob/main/images/app_bms.PNG)

A small ESP32 audio application that connects to Wi-Fi and plays an MP3 file from an HTTP URL using the Espressif Audio Development Framework (ESP-ADF). The project is structured to make it easy to extend with extra audio sources and UI, and it ships with an optional flash-tone component for short system sounds (beeps/alerts).

Tested on ESP32 audio dev boards via ADF’s Audio HAL (e.g., ESP32-LyraT-Mini).

![esp32-lyrat-mini](https://github.com/michalizn/multifunction_speaker/blob/main/images/esp32-lyrat-mini-v1.2-layout.png)

---

## Table of Contents

- Features  
- Hardware  
- Architecture  
- Getting Started  
  - Prerequisites  
  - Clone & Configure  
  - Build, Flash, Monitor  
- Configuration  
- Project Layout  
- Flash Tones (optional)  
- Troubleshooting  
- Roadmap  
- Contributing  
- License  
- Acknowledgements

---

## Features

- HTTP MP3 playback using ESP-ADF’s audio pipeline  
- Wi-Fi connectivity configured from menuconfig  
- Finite-state flow (connect → play) with a state diagram asset under images/  
- Optional flash tones for UI feedback (startup chime, error beep, etc.)

This repo started from the ADF “play MP3 from HTTP” pattern and keeps its workflow so you can extend it easily.

![esp32-lyrat-mini](https://github.com/michalizn/multifunction_speaker/blob/main/images/state_diagram.PNG)

---

## Hardware

Any ESP-ADF supported audio board should work when selected in Audio HAL. Typical boards:

- ESP32-LyraT-Mini (mono, ES8311 codec)  
- ESP32-LyraT, ESP32-LyraTD-MSC  
- ESP32-Korvo-DU1906  
- ESP32-S2 Kaluga-1 kit

You’ll need:

- An ESP32 audio dev board (e.g., ESP32-LyraT-Mini)  
- A small speaker (4 Ω/3 W recommended) or headphones  
- 2× USB cables (power + UART, depending on board)  
- A 2.4 GHz Wi-Fi network

---

## Architecture

Default audio pipeline:

[http_server] → http_stream → mp3_decoder → i2s_stream → [audio codec / speaker]

- http_stream pulls audio frames from a URL  
- mp3_decoder decodes to PCM  
- i2s_stream delivers PCM to the board codec via I²S  
- Audio HAL abstracts the codec/board settings

You can add sources (SD card, HTTP HLS, BT A2DP, etc.) or effects (EQ/downmixer) using other ADF elements later.

---

## Getting Started

### Prerequisites

- ESP-IDF installed and exported  
- ESP-ADF cloned and exported; set ADF_PATH and IDF_PATH  
- Python 3.x for IDF tools

The project’s CMake includes both ADF and IDF CMake files via those environment variables.

### Clone & Configure

git clone https://github.com/michalizn/multifunction-speaker
cd multifunction-speaker

idf.py set-target esp32
idf.py menuconfig

In menuconfig:

- Audio HAL → select your board (e.g., ESP32-LyraT-Mini)  
- Example Configuration → set WiFi SSID and WiFi Password (and audio URL if present)

### Build, Flash, Monitor

idf.py build
idf.py -p PORT flash
idf.py -p PORT monitor

On boot, the device connects to Wi-Fi and starts playing automatically.

---

## Configuration

All runtime configuration lives in idf.py menuconfig:

- Audio HAL (board selection)  
- Example Configuration  
  - WiFi SSID / WiFi Password  
  - (Optional) Audio URL for the HTTP stream

You can also define CONFIG_* symbols in Kconfig.projbuild for defaults.

---

## Project Layout

.
├─ components/
│  └─ audio_flash_tone/        # Flash tone URIs / indexes for beeps/chimes
├─ images/                     # Photos, board image, state diagram
├─ main/                       # Application source (entry, pipeline, FSM)
├─ tools/                      # Helper scripts
├─ CMakeLists.txt              # Includes ADF + IDF cmake
├─ Makefile                    # Legacy build option
├─ partitions.csv              # Custom partition layout
├─ sdkconfig                   # Saved config after menuconfig
└─ dependencies.lock           # (If present) Component Manager lockfile

The state diagram image shown on the repo front page lives under images/.

---

## Flash Tones (optional)

The `components/audio_flash_tone` component provides tone URIs in flash.  
You can trigger short sounds (startup, success, error) without storage or streaming.  

Typical usage:
- Include the tone URI header in your code  
- Play the tone URI through the pipeline  

See that folder and the ADF “audio_flash_tone” example for details.

---

## Troubleshooting

**AUDIO_HAL: codec init failed!**  
→ Select the correct board under Audio HAL in menuconfig.

**No “Audio HAL” menu**  
→ Make sure ADF_PATH is exported and you’re using a compatible IDF.

**No audio output**  
→ Check speaker connections, volume, and PA enable pin.

If issues persist:  
`idf.py fullclean` → reconfigure → rebuild → reflash.

---

## Roadmap

- Add more sources (SD card, HLS, BT A2DP)  
- Buttons for play/pause/next, volume  
- Web UI for Wi-Fi and URLs  
- OTA updates  
- Unit tests for pipeline

---

## Contributing

PRs and issues welcome!  
Include:
- Board used + sdkconfig diff  
- Logs from idf.py monitor  
- Steps to reproduce

---

## License

No license file yet.  
Consider adding MIT, Apache-2.0, or BSD-3-Clause.

---

## Acknowledgements

- Espressif ESP-IDF and ESP-ADF  
- Original ADF HTTP-MP3 example pattern this project builds upon

