# Nehale Smart Prepaid Meter

DPJ621S · Simon E. Nehale (223019267)

ESP32 prepaid electricity meter: measures the load with an ACS712, deducts kWh from a prepaid balance,
cuts the load at zero credit or on overload, and accepts top-ups by keypad token, SMS (SIM900) or a web page.
A Flask web app shows live readings, history and a usage prediction trained in Google Colab.

| Path | What it is |
|---|---|
| `Nehale_ESP32.ino` | ESP32 firmware (Arduino IDE, board "ESP32 Dev Module") |
| `secrets_example.h` | Template for your WiFi, backend URL and owner phone number |
| `Nehale_WebUI/` | Flask web app, prediction model and Colab notebook - see [its README](Nehale_WebUI/README.md) |

## Firmware setup

1. Copy `secrets_example.h` to `secrets.h` (same folder) and fill in your WiFi name and password,
   the web app address (`http://<laptop IP>:5000/api/meter`) and the owner's phone number.
   `secrets.h` is in `.gitignore`, so it is never uploaded.
2. Libraries: **LiquidCrystal I2C** (Frank de Brabander) and **Keypad** (Mark Stanley).
3. Board: **ESP32 Dev Module**. Upload.

`DEMO_MODE` (top of the sketch) runs time 10 000x faster so a top-up is used up in minutes;
set it to `0` for real time. `START_FROM_ZERO 1` clears the balance at every power-up (testing).

## Web app

```
cd Nehale_WebUI
pip install -r requirements.txt
python app.py
```
Open http://127.0.0.1:5000. The ESP32 must be on the same network as the laptop.
