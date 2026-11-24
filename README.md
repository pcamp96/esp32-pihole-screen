# ESP32 Pi-hole Screen

An ESP32-S3 (N16R8) that shows Pi-hole v6 stats on an SSD1306 OLED and/or an I2C LCD1602, with a button that pauses blocking for five minutes.

**Install from your browser:** https://pcamp96.github.io/esp32-pihole-screen/

- Wi-Fi and Pi-hole settings are entered on a setup page (`PiholeSetup` Wi-Fi network) after flashing, so the firmware holds no passwords.
- Screens are detected at boot (OLED 0x3C/0x3D, LCD 0x27/0x3F) on SDA GPIO 8 / SCL GPIO 9. Button on GPIO 4 to GND.
- The onboard RGB LED shows status: blue setup, green blocking, amber paused, red error.

## Build it yourself

Arduino IDE with the esp32 core (tested on 4.0.0). Board: ESP32S3 Dev Module, Flash 16MB, PSRAM OPI, Partition "16M Flash (3MB APP/9.9MB FATFS)", USB CDC On Boot enabled.

Libraries: ArduinoJson, Adafruit SSD1306, Adafruit GFX, LiquidCrystal I2C, WiFiManager (tzapu).

The install page in `docs/` uses [ESP Web Tools](https://esphome.github.io/esp-web-tools/) and flashes `docs/firmware/esp32-pihole-screen-s3.bin` (a merged image at offset 0).

Not affiliated with Pi-hole.
