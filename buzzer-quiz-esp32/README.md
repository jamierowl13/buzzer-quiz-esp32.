# ESP32 Buzzer Quiz Game

A four-player buzzer quiz game for the ESP32 with a live web scoreboard. Players buzz in on Red, Yellow, Blue or Green buttons; the host marks each answer correct (+1, all buzzers reopen) or wrong (−1, that player is locked out). Any phone, tablet or TV on the same Wi-Fi can show the scoreboard, and the host can run the game from the web page too.

Live demo of the web page (simulated, no hardware needed): https://claude.ai/artifact/56aD6ate3GgspzZPnUsHhi

## Repository layout

| Path | What it is |
| --- | --- |
| `buzzer_game_esp32_web/buzzer_game_esp32_web.ino` | The ESP32 sketch (web page embedded inside) |
| `web/index.html` | The web page on its own, for editing |
| `web/demo.html` | Demo copy that always runs the built-in simulator |
| `original/buzzer_game_lcdV2_Ver0.ino` | Original Arduino Uno/Nano sketch (v0) |

If you edit `web/index.html`, paste it between `R"rawliteral(` and `)rawliteral"` in the sketch.

## Version history

| Version | Changes |
| --- | --- |
| v2 – Player names | Custom name per colour button (up to 16 characters), set from the web page, saved in ESP32 flash and shared with every viewer. Web page shows the name large and the colour small. LCD unchanged. `playerNames` renamed to `colourNames`; new `/names` endpoint; `/state` returns names. |
| v1 – ESP32 port + web scoreboard | Ported to ESP32: SoftwareSerial → hardware Serial2, pins remapped, non-blocking game loop. Wi-Fi (own access point or home network), mDNS, web scoreboard with four large colour buttons and host controls. LCD shows R/Y/B/G and the scoreboard address at startup. |
| v0 – Original | Arduino Uno/Nano, 4 buttons + LEDs, host Correct/Wrong buttons, DY audio player, I2C LCD. |

## Wiring (ESP32)

Buttons go between the GPIO and GND (internal pull-ups). LEDs need a ~220 Ω resistor to GND.

| Function | GPIO |
| --- | --- |
| Red button (Player 1) | 32 |
| Yellow button (Player 2) | 33 |
| Blue button (Player 3) | 25 |
| Green button (Player 4) | 26 |
| Red / Yellow / Blue / Green LED | 18 / 19 / 23 / 27 |
| Host Correct / Wrong button | 13 / 14 |
| LCD SDA / SCL | 21 / 22 |
| DY player TX → ESP32 RX2 | 16 |
| DY player RX ← ESP32 TX2 | 17 |

Avoid GPIO 34–39 for buttons (no pull-ups) and GPIO 0, 2, 12, 15 (boot strapping pins).

## Setup

1. Arduino IDE: install the Espressif **esp32** board package.
2. Libraries: [DYPlayer](https://github.com/SnijderC/dyplayer) and **LiquidCrystal I2C** by Frank de Brabander. WiFi, WebServer, ESPmDNS and Preferences come with the ESP32 package.
3. Open `buzzer_game_esp32_web/buzzer_game_esp32_web.ino`, choose your ESP32 board and upload.

| Wi-Fi mode | Setting | Open the scoreboard |
| --- | --- | --- |
| Own network (default) | `USE_HOME_WIFI = false` | Join **Buzzer-Game** (password `letsplay123`), open http://192.168.4.1 |
| Home Wi-Fi | `USE_HOME_WIFI = true` + `HOME_SSID` / `HOME_PASS` | http://buzzer.local or the IP on the LCD |

Home Wi-Fi falls back to the ESP32's own network if it can't connect within 15 seconds.

## Web API

| Endpoint | Method | Does |
| --- | --- | --- |
| `/` | GET | Scoreboard page |
| `/state` | GET | JSON: `scores`, `active`, `buzzed` (−1 if nobody), `names` |
| `/correct` | POST | +1 to the buzzed player, reopen all buzzers |
| `/wrong` | POST | −1 to the buzzed player, lock them out |
| `/reopen` | POST | Reopen all buzzers |
| `/resetscores` | POST | Scores to 0, reopen buzzers (names kept) |
| `/names` | POST | Form fields `n0`–`n3`; saved to flash |

## Status

The sketch passes a syntax check but has not yet been compiled for or tested on a real ESP32.
