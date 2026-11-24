# ESP32 Pi-hole Screen

An ESP32-S3 (N16R8) that shows Pi-hole v6 stats on an SSD1306 OLED and/or an I2C LCD1602, with a button that pauses blocking for five minutes.

**Install from your browser:** https://pcamp96.github.io/esp32-pihole-screen/

- Enter your Wi-Fi and Pi-hole settings on the install page. The page writes them into a copy of the firmware in your browser (`docs/patch.js`, which also fixes the image checksum and SHA-256) and flashes it over USB. Nothing is uploaded anywhere.
- If Wi-Fi fails, or the button is held at power-on, the board opens a `PiholeSetup` Wi-Fi network with a setup page at 192.168.4.1.
- Wi-Fi transmit power is set to 8.5 dBm, which made the setup network visible on a clone N16R8 board. Change `WIFI_TX_POWER` if yours has a good antenna.
- Screens are detected at boot (OLED 0x3C/0x3D, LCD 0x27/0x3F) on SDA GPIO 8 / SCL GPIO 9. Button on GPIO 4 to GND. Power both screens from 3V3: on many clone S3 boards the "5Vin" pin is an input only.
- The onboard RGB LED shows status: blue setup, green blocking, amber paused, red error.

## Build it yourself

Arduino IDE with the esp32 core (tested on 4.0.0). Board: ESP32S3 Dev Module, Flash 16MB, PSRAM OPI, Partition "16M Flash (3MB APP/9.9MB FATFS)", USB CDC On Boot enabled.

Libraries: ArduinoJson, Adafruit SSD1306, Adafruit GFX, LiquidCrystal I2C, WiFiManager (tzapu).

The install page in `docs/` uses [ESP Web Tools](https://esphome.github.io/esp-web-tools/) with a manifest generated in the browser. It patches `docs/firmware/esp32-pihole-screen-s3.bin` (a merged image at offset 0). After rebuilding, regenerate that file from the `.merged.bin` build output.

Not affiliated with Pi-hole.
