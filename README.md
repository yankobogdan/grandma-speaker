# grandma-speaker

A talking AI companion built on the Waveshare **ESP32-S3-Touch-LCD-1.85C (V2)**
round-display board and the Gemini Live API.

Tap the screen, speak Ukrainian, and it answers out loud in Ukrainian through the
onboard speaker. Built as a simple, friendly device for an elderly relative: one
big button, large text, nothing to configure day to day.

<img src="https://img.shields.io/badge/board-ESP32--S3--Touch--LCD--1.85C-blue" alt="board">
<img src="https://img.shields.io/badge/revision-V2%20only-orange" alt="revision">
<img src="https://img.shields.io/badge/language-Ukrainian-yellow" alt="language">

---

## ⚠️ Board revision matters — V2 only

Waveshare shipped **two incompatible revisions** of this board under the same name.
This firmware targets **V2**. Check yours before flashing:

| | V1 | **V2 (this firmware)** |
|---|---|---|
| Microphone | MEMS digital, wired straight to I2S | Analog mic behind an **ES7210** codec |
| Speaker DAC | PCM5101A | **ES8311** codec |
| Amplifier | NS8002 | NS4150B (enable on GPIO15) |
| GPIO2 | MIC_WS | **I2S_MCLK** |
| GPIO10 / GPIO11 | unused | **I2C SCL / SDA** (codec control) |
| GPIO15 | MIC_SCK | **PA_CTRL** (amp enable) |

**How to tell:** V2 has a `Rev2.0` silkscreen on the PCB and a `V2` sticker on the case.

The vendor demo code targets V1. On a V2 board its mic returns **pure digital silence**
no matter what I2S settings you try, because the ES7210 needs I2C configuration before
it outputs anything at all. That was the single most time-consuming discovery in this
project — see [Things that will bite you](#things-that-will-bite-you).

---

## What it does

- **Push to talk** — tap the screen (or the physical BOOT button) and speak
- **Answers aloud in Ukrainian**, and shows the reply as scrollable text
- **Remembers the conversation** within a session; starts fresh on reboot
- **WiFi setup on the screen** — pick a network, type the password, no reflashing
- **Settings** — volume, screen brightness, spoken replies on/off, talk-button mode
- **Status at a glance** — WiFi name, battery percentage, and clear error messages

---

## Getting started

### 1. Hardware

- Waveshare ESP32-S3-Touch-LCD-1.85C (**V2**), 16MB flash, 8MB PSRAM
- USB-C cable
- Optional: speaker and Li-ion battery for untethered use

### 2. Get a Gemini API key

Create one at [aistudio.google.com/apikey](https://aistudio.google.com/apikey).

The default model is `gemini-2.5-flash-native-audio-latest`, set in
`GranVoice_Config.h`. Gemini 3.x Live models work too, but their Google Search
grounding draws from a monthly pool that runs out faster than the 2.5 daily one —
see [Things that will bite you](#things-that-will-bite-you).

### 3. Configure

```bash
cd DEMO/Arduino/examples/LVGL_Arduino
cp GranVoice_Config.h.example GranVoice_Config.h
```

Edit `GranVoice_Config.h` and fill in your API key, and optionally a default WiFi
network. **This file is gitignored** — your key and WiFi password never leave your machine.

You can also change `GRANVOICE_SYSTEM_PROMPT` here — that's the personality. It's
currently written to be warm, patient, and to always reply in Ukrainian in short
sentences.

### 4. Install dependencies

Install the ESP32 core (**3.3.11**, not the 4.x alpha — see gotchas):

```bash
arduino-cli core install esp32:esp32@3.3.11
```

Two large, unmodified vendor libraries are **not** included here (they'd add ~200MB
of code this project doesn't change). Download them from Waveshare's demo package
for this board and place them in `DEMO/Arduino/libraries/`:

| Library | Version | Source |
|---|---|---|
| `lvgl` | 8.3.x | Waveshare demo, or [lvgl/lvgl](https://github.com/lvgl/lvgl/tree/release/v8.3) |
| `ESP32-audioI2S-master` | 2.0.0 | Waveshare demo |

> `lv_conf.h` lives in the sketch folder and is already configured — don't replace it.

**Included** (don't install separately): `arduinoWebSockets` is bundled because it's
**patched** — the stock version's 15KB frame cap silently truncates Gemini's audio.
`ArduinoJson` is bundled for convenience.

### 5. Build and flash

Then from `DEMO/Arduino/examples/LVGL_Arduino`:

```bash
arduino-cli compile \
  -b "esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB" \
  --libraries ../../libraries \
  --upload -p /dev/cu.usbmodemXXXX .
```

Those board options are **not optional**: PSRAM must be `opi`, and the default 4MB
partition scheme is too small for this sketch.

> **Tip:** if you have unrelated Arduino libraries installed globally, they can shadow
> the bundled ones. Point `arduino-cli` at a scratch sketchbook with `--config-file`
> to build in isolation.

### 6. First boot

The device connects to WiFi (using your configured default, if any), then to Gemini.
If WiFi fails it opens the network picker on screen automatically. Once the button
turns teal, tap and talk.

---

## How it works

```
   ┌──────────┐  16kHz PCM   ┌──────────┐   WebSocket over TLS   ┌────────────┐
   │  ES7210  │─────────────▶│          │───────────────────────▶│   Gemini   │
   │   mic    │              │ ESP32-S3 │                        │  Live API  │
   │  ES8311  │◀─────────────│          │◀───────────────────────│            │
   └──────────┘  24kHz→16kHz └──────────┘   audio + transcripts  └────────────┘
                              LVGL on a
                            360×360 round
                              display
```

Both codecs share **one full-duplex I2S bus** at 16kHz and are configured over I2C.
Gemini streams replies at 24kHz, so playback is resampled down to 16kHz to match the
shared bus clock.

Turn-taking uses Gemini's **server-side voice activity detection** — the device streams
audio while listening and Gemini decides when you've stopped speaking. There's a 15s
client-side timeout as a safety net.

### Source layout

| File | Purpose |
|---|---|
| `GeminiLive.*` | WebSocket client, setup message, audio framing, session resumption |
| `GranVoice_Audio.*` | Mic capture, playback, resampling, volume, UI click |
| `GranVoice_UI.*` | All screens: talk, settings, WiFi picker, password entry |
| `ES7210.*` / `ES8311.*` | Codec drivers (ported from Espressif's `esp-bsp`) |
| `MIC_MSM.*` | Shared I2S bus setup and pin map |
| `GranVoice_Config.h` | Your secrets and persona (gitignored) |

---

## Things that will bite you

Hard-won notes. If you're building on this board, these will save you days.

**The mic reads perfect silence (V2 boards).**
Not a wiring fault. The ES7210 codec must be configured over I2C before it emits
anything — no I2S configuration will help. Every sample reads exactly `0x00000000`,
which is the giveaway: real mic noise is never *exactly* zero.

**All display text renders as garbage.**
`lv_font_conv` RLE-compresses glyphs by default, but this project's `lv_conf.h` sets
`LV_USE_FONT_COMPRESSED 0`. Regenerate with `--no-compress`. Also declare the font
`extern "C" const lv_font_t` — the generated file defines it `const`.

**Cyrillic works but icons are boxes.**
`LV_SYMBOL_*` are FontAwesome glyphs in the U+F000 private-use range. A custom font
covering only ASCII + Cyrillic won't have them. Use `--lv-fallback lv_font_montserrat_16`.

**Replies cut off mid-sentence into noise.**
Three separate causes, all needed fixing: the WebSocket library's **15KB frame cap**
(Gemini's audio deltas exceed it and the socket closes with code 1009 *before* your
callback ever sees the frame); a mutex shared between the blocking mic read and
playback, starving the TX DMA; and a resampler that dropped a sample at every chunk
boundary.

**TLS fails with "SSL - Memory allocation failed".**
mbedTLS needs contiguous *internal* RAM, which PSRAM doesn't satisfy. LVGL's draw
buffers are ~50KB of internal RAM by default — move them to PSRAM with
`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`. Also bring WiFi up **before** initialising
audio, or the WiFi driver can't get its RX buffers.

**Colourful stripes on boot.**
The vendor's `Backlight_Init()` lights the panel before the display is initialised.
Set brightness to 0 during init and raise it after the first `lv_refr_now()`.

**"You exceeded your current quota" — but AI Studio says you're fine.**
Search grounding bills against a **separate** quota, split by model generation
(3.x models share a monthly pool; 2.5 models use a daily one). The error message is
generic and doesn't say which quota. Diagnose with an A/B: send the same prompt with
and without `{"googleSearch":{}}` — if only the tool version 429s, it's the grounding
quota, not your account.

**Silent connection failures in general.**
The WebSocket close frame carries the real reason, but most libraries only log it in
debug builds. Surfacing it turned three separate mysteries into one-line diagnoses.

---

## Status

Working: voice conversation, mic, speaker, display, WiFi provisioning, settings,
battery, conversation memory.

Rough edges:
- **Search grounding is quota-limited.** When the allowance runs out, Gemini can't
  look things up. Worse, the failure kills the whole Live session mid-turn rather
  than degrading gracefully. Routing search through a third-party API via function
  calling would fix both.
- Ukrainian font covers ASCII + Cyrillic only.
- Latency is roughly a second — fine for conversation, not instant.

---

## Credits

Built on Waveshare's ESP32-S3-Touch-LCD-1.85 demo. Codec drivers ported from
[espressif/esp-bsp](https://github.com/espressif/esp-bsp). Uses
[LVGL](https://lvgl.io), [ArduinoJson](https://arduinojson.org), and
[arduinoWebSockets](https://github.com/Links2004/arduinoWebSockets) (patched — see
`DEMO/Arduino/libraries/`).

## License

Vendor demo code and bundled libraries retain their original licenses (LVGL: MIT,
ESP32 core: LGPL-2.1, esp-bsp components: Apache-2.0). Project-specific code is
provided as-is.
