# ESP32-S3 Buzzer Quiz Game

A four-player buzzer quiz game for the ESP32-S3 with a live web scoreboard. Players buzz in on Red, Yellow, Blue or Green buttons; the host marks each answer correct (+1, all buzzers reopen) or wrong (−1, that player is locked out). Any phone, tablet or TV on the same Wi-Fi can show the scoreboard, and the host can run the game from the web page too.

Live demo of the web page (simulated, no hardware needed): https://claude.ai/artifact/56aD6ate3GgspzZPnUsHhi

New to the game? Read the [instruction manual](docs/Buzz_Quiz_Game_Instruction_Manual.pdf).

## Repository layout

| Path | What it is |
| --- | --- |
| `buzzer_game_esp32_web/buzzer_game_esp32_web.ino` | The ESP32-S3 sketch (web page embedded inside) |
| `web/index.html` | The web page on its own, for editing |
| `web/demo.html` | Demo copy that always runs the built-in simulator |
| `original/buzzer_game_lcdV2_Ver0.ino` | Original Arduino Uno/Nano sketch (v0) |
| `docs/ESP32-S3_Buzzer_Quiz_Wiring.docx` | Printable wiring guide: every connection on the ESP32-S3 terminal adapter |
| `docs/Buzz_Quiz_Game_Instruction_Manual.pdf` (and `.docx`) | How to set up and play the game: rounds, scoring, host controls, troubleshooting |

If you edit `web/index.html`, paste it between `R"rawliteral(` and `)rawliteral"` in the sketch.

## Version history

| Version | Changes |
| --- | --- |
| v4 – Buzzer sound per player | Each player's buzzer sound can be picked from files 1–30 on the web page (host bar → ✎ Players), with a ▶ button that plays it on the DY player first. Saved in ESP32 flash with the names. New `/players` and `/play` endpoints; `/state` returns `sounds` and `maxSound`. |
| v3 – ESP32-S3 | Pins remapped for the ESP32-S3 (the old pins 26/32 are wired to the S3's flash chip and caused a watchdog reset loop; 22/23/25 don't exist on the S3). LCD I2C moved to SDA 8 / SCL 9. LCD top row now shows two-letter labels `Rd Yw Bl Gr`. Numbered startup checkpoints in the Serial Monitor and an LCD presence check, so a startup hang shows which step failed. Tested on real hardware. |
| v2 – Player names | Custom name per colour button (up to 16 characters), set from the web page, saved in ESP32 flash and shared with every viewer. Web page shows the name large and the colour small. LCD unchanged. `playerNames` renamed to `colourNames`; new `/names` endpoint; `/state` returns names. |
| v1 – ESP32 port + web scoreboard | Ported to ESP32: SoftwareSerial → hardware Serial2, pins remapped, non-blocking game loop. Wi-Fi (own access point or home network), mDNS, web scoreboard with four large colour buttons and host controls. LCD shows R/Y/B/G and the scoreboard address at startup. |
| v0 – Original | Arduino Uno/Nano, 4 buttons + LEDs, host Correct/Wrong buttons, DY audio player, I2C LCD. |

## Wiring (ESP32-S3)

Made for the 44-pin ESP32-S3 DevKitC-1 (N8R2 / N16R8). All the pins used are on one side of the board. For a printable version with the full terminal-strip layout, see [`docs/ESP32-S3_Buzzer_Quiz_Wiring.docx`](docs/ESP32-S3_Buzzer_Quiz_Wiring.docx).

Buttons go between the GPIO and GND (internal pull-ups). LEDs need a ~220 Ω resistor to GND.

| Function | GPIO |
| --- | --- |
| Red button (Player 1) | 4 |
| Yellow button (Player 2) | 5 |
| Blue button (Player 3) | 6 |
| Green button (Player 4) | 7 |
| Red / Yellow / Blue / Green LED | 10 / 11 / 12 / 15 |
| Host Correct / Wrong button | 13 / 14 |
| LCD SDA / SCL | 8 / 9 |
| DY player TX → ESP32 RX2 | 16 |
| DY player RX ← ESP32 TX2 | 17 |

ESP32-S3 pins to avoid: **26–32** (wired to the flash chip; using them crashes the board), 33–37 (PSRAM on many boards), 19/20 (USB), 0/3/45/46 (boot strapping pins), 38/48 (onboard RGB LED on many boards) and 43/44 (TX/RX of the main serial port). GPIO 22–25 don't exist on the S3.

## Setup

1. Arduino IDE: install the Espressif **esp32** board package.
2. Libraries: [DYPlayer](https://github.com/SnijderC/dyplayer) and **LiquidCrystal I2C** by Frank de Brabander. WiFi, WebServer, ESPmDNS and Preferences come with the ESP32 package.
3. Tools menu settings:

   | Setting | Value |
   | --- | --- |
   | Board | ESP32S3 Dev Module |
   | USB CDC On Boot | Enabled |
   | Flash Size | 16MB (N16R8) or 8MB (N8R2) |
   | PSRAM | OPI PSRAM (N16R8) or QSPI PSRAM (N8R2) |

4. Open `buzzer_game_esp32_web/buzzer_game_esp32_web.ino` and upload. The Serial Monitor (115200) prints numbered startup checkpoints; if the board ever resets in a loop, the last one shows which step stopped.

| Wi-Fi mode | Setting | Open the scoreboard |
| --- | --- | --- |
| Own network (default) | `USE_HOME_WIFI = false` | Join **Buzzer-Game** (password `letsplay123`), open http://192.168.4.1 |
| Home Wi-Fi | `USE_HOME_WIFI = true` + `HOME_SSID` / `HOME_PASS` | http://buzzer.local or the IP on the LCD |

Home Wi-Fi falls back to the ESP32's own network if it can't connect within 15 seconds.

## Sound files

The DY player plays files by number. Name them `00001.mp3`, `00002.mp3` … `00030.mp3` in the root of the SD card or flash.

| File | Used for |
| --- | --- |
| 1 | Intro (at power-up) |
| 2 | Wrong answer |
| 3 | Correct answer |
| 4 | Default buzzer sound |
| 1–30 | Any of these can be picked as a player's buzzer sound on the web page |

To offer more or fewer files, change `maxSoundFile` in the sketch.

## Web API

| Endpoint | Method | Does |
| --- | --- | --- |
| `/` | GET | Scoreboard page |
| `/state` | GET | JSON: `scores`, `active`, `buzzed` (−1 if nobody), `names`, `sounds`, `maxSound` |
| `/correct` | POST | +1 to the buzzed player, reopen all buzzers |
| `/wrong` | POST | −1 to the buzzed player, lock them out |
| `/reopen` | POST | Reopen all buzzers |
| `/resetscores` | POST | Scores to 0, reopen buzzers (names kept) |
| `/players` | POST | Form fields `n0`–`n3` (names) and `s0`–`s3` (sound file numbers); saved to flash |
| `/names` | POST | Older name for `/players`, still works |
| `/play` | POST | Form field `f`: plays that sound file on the DY player (to try a sound) |

## Status

Working on a real ESP32-S3 DevKitC-1 with buttons, LEDs, LCD and DY audio player. The v4 per-player sound picker has been tested in a browser but not yet compiled or run on the ESP32.

`buzzer-quiz-esp32.zip` in the repository root is an older download of the v2 (classic ESP32) files.
