# Waveshare ESP32-S3-Touch-LCD-4.3 WiFi Clock

This project displays a digital clock on the Waveshare ESP32-S3-LCD-4.3 Touch display, synchronized via WiFi using NTP (Network Time Protocol).

## Features

- **WiFi Connectivity**: Connects to your WiFi network for time synchronization
- **NTP Time Sync**: Automatically synchronizes time from internet time servers
- **Clean Display**: 
  - Black background
  - Large cyan clock digits centered on screen
  - Yellow firmware version in lower left corner

## Get Started

1. Open the project in PlatformIO
2. Edit `src/main.cpp` and update the WiFi credentials:
   ```cpp
   const char* ssid = "YOUR_WIFI_SSID";
   const char* password = "YOUR_WIFI_PASSWORD";
   ```
3. Optionally adjust the timezone settings:
   ```cpp
   const long gmtOffset_sec = 0;        // Adjust for your timezone (seconds offset from GMT)
   const int daylightOffset_sec = 0;    // Daylight saving time offset
   ```
4. Connect your board to USB
5. Make sure to press and hold the Boot button on your board, then press the Reset button, and release the Boot button
6. Click Upload
7. The clock will display after WiFi connection and time synchronization

## Libraries

The libraries are the ones provided on the [Waveshare Wiki](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3): [S3-4.3-libraries.zip](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3/S3-4.3-libraries.zip), except for lvgl, which is added as library dependency in `platformio.ini`.

Additional built-in Arduino ESP32 libraries used:
- WiFi.h - WiFi connectivity
- time.h - Time functions and NTP

## PlatformIO

Check the profile in platformio.ini:

```
[env:esp32s3box]
platform = espressif32
board = esp32s3box
framework = arduino
monitor_speed = 115200
board_upload.flash_size = 8MB
build_flags = 
	-D BOARD_HAS_PSRAM
	-D LV_CONF_INCLUDE_SIMPLE
	-I lib
board_build.arduino.memory_type = qio_opi
board_build.f_flash = 80000000L
board_build.flash_mode = qio
lib_deps = 
	lvgl/lvgl@8.3.8
```

These are the equivalents for settings the [Waveshare Wiki](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3) recommends for the Arduino IDE.

## Caveats

- Make sure your WiFi credentials are correct
- The device needs internet access to sync time via NTP
- Time display format is 24-hour (HH:MM:SS)

Feedback welcome!
