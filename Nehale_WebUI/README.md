# Nehale Smart Prepaid Meter — Web UI

Python (Flask) web app for the DPJ621S smart prepaid meter.

| Page | What it does |
|---|---|
| **Live** (`/`) | Live balance, power, voltage, current, today's use (refreshes every 2 s) · days of units left, run-out date, "top up by" date, recommended top-up in N$ · remote top-up with PIN · next 7 days forecast |
| **History** (`/history`) | Daily kWh chart (measured + forecast), balance over time, daily table, event log (top-ups, low balance, cut-offs, overloads), CSV export for Colab |
| **Model** (`/model`) | Upload the `model.json` trained in Google Colab; shows its equation and test accuracy |

## 1. Run it on your laptop

```
cd Nehale_WebUI
pip install -r requirements.txt
python app.py
```
Open http://localhost:5000. Phones on the same WiFi can open `http://<laptop IP>:5000`
(find the IP with `ipconfig` → "IPv4 Address"). Allow Python through Windows Firewall when asked.

Settings are at the top of `app.py`: `TARIFF_NAD_PER_KWH` (2.0), `TOPUP_PIN` (1234 — change it!), `METER_ID` (NEHALE-01), `TARGET_DAYS` (30).

### Test without the hardware
In a second terminal: `python simulate_meter.py` — it sends exactly the same requests as the ESP32.

### Hosted demo on Vercel
The same app runs on Vercel (Root Directory `Nehale_WebUI`, framework Flask). Vercel's disk is
read-only and nothing written to it is kept, so:

* **Database** - in the Vercel project: *Storage → Create → Neon (Postgres)* and connect it to the project.
  This sets `DATABASE_URL`; the app then uses Postgres instead of `meter.db`. Without it the app still
  runs but readings vanish whenever Vercel restarts the function.
* **Open demo** - on Vercel there is no sign-in (`PUBLIC_DEMO=1`); web top-up still needs the PIN.
  Set your own **`TOPUP_PIN`** in *Settings → Environment Variables* (the default 1234 is public on GitHub).
* **Meter** - in `secrets.h` set `SECRET_BACKEND_URL` to `https://<your-app>.vercel.app/api/meter`.
  With an `https://` address the firmware sends a reading every 15 s and checks for top-ups every 10 s.
* Model uploads are disabled on Vercel; change `models/model.json` in the repository instead.
* Optional: `TZ_OFFSET_HOURS` (default 2, Namibia) for the run-out dates, `SECRET_KEY`.

## 2. Connect the ESP32

The web app speaks the same protocol as the Google Apps Script backend the firmware already uses
(`?action=log` for readings, `?action=poll` for web top-ups → `{"kwh":3.0}`), so only two changes are needed in `Nehale_ESP32.ino`:

**a) Point the backend at your laptop** (section 1):
```cpp
const char* BACKEND_URL = "http://192.168.1.20:5000/api/meter";   // your laptop's IP
```

**b) Allow plain `http://`** — `httpGet()` uses `WiFiClientSecure`, which only works for `https://`. Replace the start of `httpGet()` with:
```cpp
String httpGet(const String& query) {
  if (!wifiReady()) return "";
  WiFiClientSecure sclient; sclient.setInsecure();
  WiFiClient pclient;
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(6000);
  String url = String(BACKEND_URL) + "?" + query;
  String body = "";
  bool ok = url.startsWith("https") ? http.begin(sclient, url) : http.begin(pclient, url);
  if (ok) {
    int code = http.GET();
    if (code == 200) body = http.getString();
    else { Serial.print("[HTTP] error "); Serial.println(code); }
    http.end();
  }
  return body;
}
```
Optional: for a more "real-time" page, lower `LOG_MS` (e.g. `DEMO_MODE ? 3000UL : 10000UL`). The meter and laptop must be on the same WiFi/hotspot.

## 3. Train the model in Google Colab

1. Upload `colab/Nehale_Usage_Model.ipynb` to https://colab.research.google.com (File → Upload notebook).
2. In the first cell choose `DATA_SOURCE`:
   * `"uci"` — public UCI household power dataset (4 years, downloaded automatically) — start here;
   * `"own"` — the **Daily CSV (for Colab)** from the History page, once the meter has ≥ 30 days;
   * `"synthetic"` — quick test without downloading.
3. Runtime → Run all. The notebook compares the model with the 7-day average, checks days-left accuracy, and downloads `model.json`.
4. Upload `model.json` on the web app's **Model** page. It also prints `MB0…MB4` to paste into the firmware.

The model is `kWh/day = c1·yesterday + c2·2 days ago + c3·3 days ago + c4·mean of last 7 days` with no intercept,
so a model trained on a big household also works for the 6 W prototype load.

## How the prediction works

* **Daily history**: energy between two readings is shared over the meter-calendar time between them (works in DEMO_MODE too). Days with < 6 h of credit are ignored and partial days are scaled to 24 h — same rule as the firmware.
* **Forecast**: the model predicts tomorrow, that prediction feeds the next day, and so on. The balance is walked down day by day → **days left** and **run-out date**. "Top up by" = run-out minus 2 days.
* **Recommended top-up** = forecast use for the next 30 days − balance, × tariff, rounded up to whole N$.
* Before 7 finished days the average of the available days is used; before any finished day, the present load × 24 h.

## Files

```
app.py               Flask server + ESP32 endpoint + SQLite (meter.db is created automatically)
predictor.py         daily history, model loading, forecast and recommendation
simulate_meter.py    fake ESP32 for testing
templates/, static/  web pages (Chart.js is stored locally - works without internet)
colab/Nehale_Usage_Model.ipynb   training notebook
models/              model.json goes here (uploaded from the Model page)
```
