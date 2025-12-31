# Car Speaker – ESP32 WiFi Audio Player with Android App
This project is a DIY car audio system built around an ESP32, controlled by an Android app, and designed to play WAV audio files directly from an SD card via an I2S amplifier.

The ESP32 acts as a standalone WiFi access point, exposing a small HTTP API.
An Android app connects to this AP and provides full control over playback, file management, and device configuration.

The system is intentionally offline-first: no cloud, no external services, no internet required.

# Android App to controll ESP
***will follow for download on the appstore for a small pricetag for a coffee***

## Key Features
1) ESP32-based audio player
   - Plays PCM 16-bit WAV files from SD card
   - I2S output (tested with MAX98357A)
   - Software volume control
   - Stable playback with status reporting (isPlaying, current, etc.)
2) Android App (native, Kotlin)
   - Browse WAV files by folder (A / B / C / D)
   - Start / stop playback
   - Edit mode with file management:
     - Delete files
     - Move files between folders
     - Drag & drop WAV files into folder targets
   - Upload new WAV files from the phone
   - Device settings dialog:
     - Volume slider
     - WiFi AP configuration (SSID / password)
     - Live connection check to ESP
   - Graceful handling of ESP restarts and WiFi changes
3) Drag & Drop File Management
   - Long-press a WAV file in edit mode
   - Drag it onto a folder drop target (A–D)
   - File is moved on the ESP via HTTP /move endpoint
4) Robust Playback State Handling
   - Android app uses polling with startup grace period
   - Prevents false stop detection during ESP playback startup
   - Clean stop on playback end or user action
5) ESP Configuration via JSON
   - ESP exposes /config endpoint
   - Android app loads, edits, and saves configuration as JSON
   - ESP applies configuration on boot
   - Optional WiFi AP restart when credentials change

## Architecture Overview
1) ESP32
   - Runs as WiFi Access Point
   - Hosts HTTP server (play, stop, list, upload, move, config, status)
   - Handles SD card, WAV parsing, I2S audio output
   - No Bluetooth required for core functionality
2) Android App
   - Connects directly to ESP AP
   - No background services or internet dependency
   - UI driven by ViewModel + StateFlow
   - Designed to recover cleanly from connection loss

## Intended Use
This project is aimed at:
1) DIY car projects
2) Embedded audio experiments
3) ESP32 + I2S learning projects
4) Offline audio systems
5) Anyone who wants a simple, hackable audio player without cloud dependencies
***It is not intended to replace a commercial head unit — it is a learning-focused, extensible platform.***

## Current Status
   - Phase 4 complete
   - Stable playback
   - Drag & drop file management working
   - Device configuration UI functional
   - I2S audio output verified

## Why this project exists
Most ESP32 audio projects focus on streaming, Bluetooth, or internet radio.
This project focuses on local control, determinism, and simplicity:

>You turn it on, connect your phone, and it plays sound.
>No accounts. No pairing. No servers.

***Mainly, because i was bored and wanted to make a little challange, if i'm able to completly code vibe this thing***

#Additional Needings
## Needed electronic parts:
1) ESP32
2) i2s AMP
3) SD Card SPI Adapter
4) 5v stepdown konverter

## Needed additionals:
1) SD Card
2) Speaker
3) Maybe an developer board for the esp32

# Pinlayout:
the layout for the pins will follow in the readme. meanwhile its just available directly in the ESP Code or as Text:
> // =======================<br/>
> // Pins<br/>
> // =======================<br/>
> static const int PIN_STATUS_LED = 2;<br/>
> <br/>
> static const int PIN_SD_CS   = 5;<br/>
> static const int PIN_SD_MOSI = 23;<br/>
> static const int PIN_SD_MISO = 19;<br/>
> static const int PIN_SD_SCK  = 18;<br/>
> <br/>
> // AMP Control<br/>
> static const int PIN_AMP_SD   = 27;  // SD/EN<br/>
> static const int PIN_AMP_GAIN = 26;  // GAIN (optional!)<br/>
> static const bool AMP_SD_ACTIVE_HIGH = true;<br/>
> <br/>
> // I2S<br/>
> static const int PIN_I2S_BCLK  = 14;<br/>
> static const int PIN_I2S_LRCLK = 25;<br/>
> static const int PIN_I2S_DOUT  = 32;<br/>
<br/>

# Making of:
The App was almost completly done by code vibing, without any experiences on .ino files as well as Android App Development.
