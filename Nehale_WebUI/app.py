"""
NEHALE SMART PREPAID METER - web interface
DPJ621S | Simon E. Nehale (223019267)

Run:   python app.py          then open  http://localhost:5000
The ESP32 talks to  http://<this PC's IP>:5000/api/meter  (set BACKEND_URL in the firmware).

Pages
  /          live readings, prediction / top-up recommendation, remote top-up
  /history   daily usage, balance over time, event log, CSV export
  /model     load the model trained in Google Colab (model.json)

ESP32 protocol (unchanged from the Google Apps Script backend)
  GET /api/meter?action=log&meter=..&event=..&balance=..&total=..&volts=..  -> "OK"
  GET /api/meter?action=poll&meter=..                                       -> {"kwh":3.000}
"""
import csv
import io
import os
import sqlite3
import time
from datetime import datetime, timezone

from flask import (Flask, Response, flash, g, jsonify, redirect, render_template,
                   request, url_for)

import predictor

BASE = os.path.dirname(os.path.abspath(__file__))
DB_PATH = os.environ.get("NEHALE_DB", os.path.join(BASE, "meter.db"))
MODEL_DIR = os.path.join(BASE, "models")

# ---------------------------------------------------------------- settings
METER_ID = os.environ.get("METER_ID", "NEHALE-01")
TARIFF_NAD_PER_KWH = float(os.environ.get("TARIFF_NAD_PER_KWH", "2.0"))   # same as firmware
TARGET_DAYS = 30            # recommendation: credit should last this many days
TOPUP_PIN = os.environ.get("TOPUP_PIN", "1234")   # PIN asked before a web top-up ("" = no PIN)
MAX_TOPUP_KWH = 50.0        # same limit as SMS "ADD"
OFFLINE_AFTER_S = 90        # meter counts as offline if nothing arrived for this long
LAST_SEEN = {}              # meter -> time.time() of its last request (log or poll)

app = Flask(__name__)
app.secret_key = os.environ.get("SECRET_KEY", "nehale-dpj621s")

SCHEMA = """
CREATE TABLE IF NOT EXISTS readings (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  meter TEXT, received_at TEXT, meter_time TEXT, event TEXT,
  balance REAL, total REAL, volts REAL, amps REAL, watts REAL, relay INTEGER,
  simday INTEGER, daykwh REAL, pred REAL, daysleft REAL, recnad REAL, demo INTEGER, runout TEXT
);
CREATE INDEX IF NOT EXISTS ix_readings_meter ON readings(meter, id);
CREATE TABLE IF NOT EXISTS topups (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  meter TEXT, kwh REAL, nad REAL, status TEXT,
  created_at TEXT, sent_at TEXT, confirmed_at TEXT
);
"""


# ---------------------------------------------------------------- database
def db():
    if "db" not in g:
        g.db = sqlite3.connect(DB_PATH)
        g.db.row_factory = sqlite3.Row
    return g.db


@app.teardown_appcontext
def close_db(_exc):
    d = g.pop("db", None)
    if d is not None:
        d.close()


def init_db():
    con = sqlite3.connect(DB_PATH)
    con.executescript(SCHEMA)
    con.close()
    os.makedirs(MODEL_DIR, exist_ok=True)


def utcnow():
    return datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S")


def num(v, default=0.0):
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def current_meter():
    return request.args.get("meter", METER_ID)


def latest_reading(meter):
    return db().execute("SELECT * FROM readings WHERE meter=? ORDER BY id DESC LIMIT 1", (meter,)).fetchone()


def all_readings(meter):
    return db().execute("SELECT meter_time, total, relay FROM readings WHERE meter=? ORDER BY id",
                        (meter,)).fetchall()


def session_readings(meter):
    """Readings since the meter's last BOOT: each start-up restarts its demo calendar and counters."""
    boot = db().execute("SELECT MAX(id) FROM readings WHERE meter=? AND event='BOOT'", (meter,)).fetchone()[0]
    return db().execute("SELECT meter_time, total, relay FROM readings WHERE meter=? AND id>=? ORDER BY id",
                        (meter, boot or 0)).fetchall()


def seconds_since(utc_text):
    t = datetime.strptime(utc_text, "%Y-%m-%d %H:%M:%S").replace(tzinfo=timezone.utc)
    return (datetime.now(timezone.utc) - t).total_seconds()


# ---------------------------------------------------------------- ESP32 endpoint
@app.route("/api/meter", methods=["GET", "POST"])
def meter_api():
    p = request.values
    action = p.get("action", "")
    meter = p.get("meter", METER_ID)
    LAST_SEEN[meter] = time.time()
    con = db()

    if action == "log":
        event = p.get("event", "READING")
        con.execute(
            """INSERT INTO readings (meter, received_at, meter_time, event, balance, total, volts, amps,
               watts, relay, simday, daykwh, pred, daysleft, recnad, demo, runout)
               VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
            (meter, utcnow(), p.get("date", ""), event, num(p.get("balance")), num(p.get("total")),
             num(p.get("volts")), num(p.get("amps")), num(p.get("watts")), int(num(p.get("relay"))),
             int(num(p.get("simday"))), num(p.get("daykwh")), num(p.get("pred")), num(p.get("daysleft")),
             num(p.get("recnad")), int(num(p.get("demo"))), p.get("runout", "")))
        if event == "TOPUP_WEB":   # the meter has applied the web top-up
            con.execute("UPDATE topups SET status='confirmed', confirmed_at=? WHERE meter=? AND status='sent'",
                        (utcnow(), meter))
        con.commit()
        return "OK"

    if action == "poll":
        rows = con.execute("SELECT id, kwh FROM topups WHERE meter=? AND status='queued'", (meter,)).fetchall()
        kwh = sum(r["kwh"] for r in rows)
        if rows:
            con.executemany("UPDATE topups SET status='sent', sent_at=? WHERE id=?",
                            [(utcnow(), r["id"]) for r in rows])
            con.commit()
        # written by hand so the firmware's  indexOf("\"kwh\":")  always finds it
        return Response('{"kwh":%.4f}' % kwh, mimetype="application/json")

    return jsonify(error="unknown action"), 400


# ---------------------------------------------------------------- data for the browser
def prediction_for(meter, last):
    rows = session_readings(meter)
    history = predictor.daily_history(rows)
    now = predictor.parse_meter_time(last["meter_time"]) if last else None
    now = now or datetime.now()
    live_w = last["watts"] if last and last["relay"] else 0.0
    model = predictor.load_model(MODEL_DIR)
    fc = predictor.forecast(last["balance"] if last else 0.0, history, live_w, now, model,
                            TARIFF_NAD_PER_KWH, TARGET_DAYS)
    fc["today_kwh"] = round(predictor.today_usage(rows), 5)
    return fc, history


@app.route("/api/live")
def api_live():
    meter = current_meter()
    last = latest_reading(meter)
    if last is None:
        return jsonify(meter=meter, has_data=False, tariff=TARIFF_NAD_PER_KWH)
    age = seconds_since(last["received_at"])
    if meter in LAST_SEEN:  # a poll also proves the meter is alive
        age = min(age, time.time() - LAST_SEEN[meter])
    recent = db().execute(
        "SELECT received_at, meter_time, watts, balance FROM readings WHERE meter=? ORDER BY id DESC LIMIT 60",
        (meter,)).fetchall()[::-1]
    fc, _ = prediction_for(meter, last)
    # ON only while current actually flows (bulbs switched on); relay closed with nothing drawing = OFF
    if last["balance"] <= 0.0005:
        state = "NO CREDIT"
    else:
        state = "ON" if last["relay"] and last["amps"] > 0 else "OFF"
    if last["event"] == "OVERLOAD":
        state = "OVERLOAD"
    return jsonify(
        meter=meter, has_data=True, tariff=TARIFF_NAD_PER_KWH,
        online=age < OFFLINE_AFTER_S, seconds_ago=round(age),
        reading={k: last[k] for k in last.keys()},
        state=state,
        balance_nad=round(last["balance"] * TARIFF_NAD_PER_KWH, 2),
        recent=[{"t": r["meter_time"] or r["received_at"], "w": r["watts"], "b": r["balance"]} for r in recent],
        meter_estimate={"pred": last["pred"], "daysleft": last["daysleft"], "recnad": last["recnad"],
                        "runout": last["runout"]},
        prediction=fc,
    )


@app.route("/api/topup", methods=["POST"])
def api_topup():
    data = request.get_json(silent=True) or request.form
    meter = data.get("meter", METER_ID)
    if TOPUP_PIN and str(data.get("pin", "")) != TOPUP_PIN:
        return jsonify(ok=False, error="Wrong PIN"), 403
    nad = num(data.get("nad"), 0)
    kwh = num(data.get("kwh"), 0) or nad / TARIFF_NAD_PER_KWH
    nad = nad or kwh * TARIFF_NAD_PER_KWH
    if kwh <= 0 or kwh > MAX_TOPUP_KWH:
        return jsonify(ok=False, error=f"Amount must be between 0 and {MAX_TOPUP_KWH:g} kWh "
                                       f"(N$ {MAX_TOPUP_KWH * TARIFF_NAD_PER_KWH:g})"), 400
    db().execute("INSERT INTO topups (meter, kwh, nad, status, created_at) VALUES (?,?,?,?,?)",
                 (meter, round(kwh, 4), round(nad, 2), "queued", utcnow()))
    db().commit()
    return jsonify(ok=True, kwh=round(kwh, 4), nad=round(nad, 2))


@app.route("/api/topups")
def api_topups():
    rows = db().execute("SELECT * FROM topups WHERE meter=? ORDER BY id DESC LIMIT 10",
                        (current_meter(),)).fetchall()
    return jsonify([{k: r[k] for k in r.keys()} for r in rows])


@app.route("/api/history")
def api_history():
    meter = current_meter()
    days = int(num(request.args.get("days"), 30))
    last = latest_reading(meter)
    fc, history = prediction_for(meter, last)
    # balance curve: at most ~400 points
    n = db().execute("SELECT COUNT(*) FROM readings WHERE meter=?", (meter,)).fetchone()[0]
    step = max(1, n // 400)
    pts = db().execute(
        "SELECT meter_time, balance, total FROM readings WHERE meter=? AND (id % ?)=0 ORDER BY id",
        (meter, step)).fetchall()
    events = db().execute(
        "SELECT received_at, meter_time, event, balance, watts FROM readings "
        "WHERE meter=? AND event!='READING' ORDER BY id DESC LIMIT 50", (meter,)).fetchall()
    return jsonify(
        daily=history[-days:],
        balance=[{"t": r["meter_time"], "b": r["balance"], "total": r["total"]} for r in pts],
        events=[{k: r[k] for k in r.keys()} for r in events],
        prediction=fc,
    )


# ---------------------------------------------------------------- CSV export (for Colab)
@app.route("/export/daily.csv")
def export_daily():
    meter = current_meter()
    history = predictor.daily_history(all_readings(meter))
    buf = io.StringIO()
    w = csv.DictWriter(buf, fieldnames=["date", "kwh_used", "credit_hours", "credit_fraction",
                                        "kwh_per_day", "valid"])
    w.writeheader()
    for d in history:
        w.writerow({**d, "valid": int(d["valid"]), "kwh_per_day": d["kwh_per_day"] or ""})
    return Response(buf.getvalue(), mimetype="text/csv",
                    headers={"Content-Disposition": f"attachment; filename={meter}_daily_usage.csv"})


@app.route("/export/readings.csv")
def export_readings():
    meter = current_meter()
    rows = db().execute("SELECT * FROM readings WHERE meter=? ORDER BY id", (meter,))
    buf = io.StringIO()
    w = csv.writer(buf)
    w.writerow([c[0] for c in rows.description])
    w.writerows(rows)
    return Response(buf.getvalue(), mimetype="text/csv",
                    headers={"Content-Disposition": f"attachment; filename={meter}_readings.csv"})


# ---------------------------------------------------------------- pages
@app.route("/")
def dashboard():
    return render_template("dashboard.html", meter=current_meter(), tariff=TARIFF_NAD_PER_KWH,
                           need_pin=bool(TOPUP_PIN), max_kwh=MAX_TOPUP_KWH, page="dashboard")


@app.route("/history")
def history_page():
    return render_template("history.html", meter=current_meter(), page="history")


@app.route("/model", methods=["GET", "POST"])
def model_page():
    if request.method == "POST":
        f = request.files.get("model")
        if not f or not f.filename:
            flash("Choose the model.json file downloaded from Colab.", "error")
        elif f.filename.endswith(".json"):
            import json
            try:
                info = json.load(f.stream)
                assert len(info["coef"]) == len(info.get("features", predictor.FEATURES))
            except Exception:
                flash("That file is not a model.json exported by the Colab notebook.", "error")
            else:
                with open(os.path.join(MODEL_DIR, "model.json"), "w") as out:
                    json.dump(info, out, indent=2)
                flash("Model loaded: " + info.get("name", "model"), "ok")
        elif f.filename.endswith(".joblib"):
            f.save(os.path.join(MODEL_DIR, "model.joblib"))
            if os.path.exists(os.path.join(MODEL_DIR, "model.json")):
                os.remove(os.path.join(MODEL_DIR, "model.json"))
            m = predictor.load_model(MODEL_DIR)
            flash("Model loaded: " + m.name if m else "Could not open the .joblib file "
                  "(check the scikit-learn version matches Colab).", "ok" if m else "error")
        else:
            flash("Upload model.json (recommended) or model.joblib.", "error")
        return redirect(url_for("model_page"))
    if request.args.get("remove"):
        for fn in ("model.json", "model.joblib"):
            p = os.path.join(MODEL_DIR, fn)
            if os.path.exists(p):
                os.remove(p)
        flash("Model removed - predictions use the 7-day average.", "ok")
        return redirect(url_for("model_page"))
    model = predictor.load_model(MODEL_DIR)
    return render_template("model.html", meter=current_meter(), model=model, page="model",
                           features=predictor.FEATURES)


init_db()

if __name__ == "__main__":
    # host 0.0.0.0 so the ESP32 (and phones) on the same WiFi can reach it
    app.run(host="0.0.0.0", port=int(os.environ.get("PORT", 5000)), debug=False, threaded=True)
