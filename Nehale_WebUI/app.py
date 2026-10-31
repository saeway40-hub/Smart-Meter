"""
NEHALE SMART PREPAID METER - web interface
DPJ621S | Simon E. Nehale (223019267)

Run:   python app.py          then open  http://localhost:5000
The ESP32 talks to  http://<this PC's IP>:5000/api/meter  (set BACKEND_URL in the firmware).

Pages (sign-in required; the first visit from this PC creates the admin account)
  /login     sign in            /setup  create the admin account (only from this PC)
  /          live readings, prediction / top-up recommendation, remote top-up
  /history   daily usage, balance over time, event log, CSV export
  /model     load the model trained in Google Colab (model.json)

ESP32 protocol (unchanged from the Google Apps Script backend)
  GET /api/meter?action=log&meter=..&event=..&balance=..&total=..&volts=..  -> "OK"
  GET /api/meter?action=poll&meter=..                                       -> {"kwh":3.000}
"""
import csv
import io
import json
import os
import secrets
import sqlite3
import time
from datetime import datetime, timedelta, timezone

from flask import (Flask, Response, flash, g, jsonify, redirect, render_template,
                   request, session, url_for)
from werkzeug.security import check_password_hash, generate_password_hash
 
import predictor

BASE = os.path.dirname(os.path.abspath(__file__))
MODEL_DIR = os.path.join(BASE, "models")

# ---------------------------------------------------------------- hosting
# Laptop: SQLite file + sign-in.  Vercel (read-only disk, nothing persists): Postgres from
# DATABASE_URL (Vercel Storage -> Neon), settings from environment variables, open demo.
ON_VERCEL = bool(os.environ.get("VERCEL"))
DATABASE_URL = os.environ.get("DATABASE_URL") or os.environ.get("POSTGRES_URL") or ""
USE_PG = DATABASE_URL.startswith(("postgres://", "postgresql://"))
DB_PATH = os.environ.get("NEHALE_DB", "/tmp/meter.db" if ON_VERCEL else os.path.join(BASE, "meter.db"))
PUBLIC_DEMO = os.environ.get("PUBLIC_DEMO", "1" if ON_VERCEL else "0") == "1"   # no sign-in
LOCAL_TZ = timezone(timedelta(hours=float(os.environ.get("TZ_OFFSET_HOURS", "2"))))  # Namibia = UTC+2

# ---------------------------------------------------------------- settings
METER_ID = os.environ.get("METER_ID", "NEHALE-01")
TARIFF_NAD_PER_KWH = float(os.environ.get("TARIFF_NAD_PER_KWH", "2.0"))   # same as firmware
TARGET_DAYS = 30            # recommendation: credit should last this many days
TOPUP_PIN = os.environ.get("TOPUP_PIN", "1234")   # PIN asked before a web top-up ("" = no PIN)
MAX_TOPUP_KWH = 50.0        # same limit as SMS "ADD"
OFFLINE_AFTER_S = 90        # meter counts as offline if nothing arrived for this long
LAST_SEEN = {}              # meter -> time.time() of its last request (log or poll)

AUTH_FILE = os.path.join(BASE, "auth.json")        # admin account (password hashed) - not in git
SECRET_FILE = os.path.join(BASE, "secret_key")     # signs the session cookie - not in git


def load_secret_key():
    """SECRET_KEY env var, else a random key generated once and kept in SECRET_FILE
    (on Vercel the disk is read-only: a random in-memory key, fine without sign-in)."""
    if os.environ.get("SECRET_KEY"):
        return os.environ["SECRET_KEY"]
    if ON_VERCEL:
        return secrets.token_hex(32)
    if not os.path.exists(SECRET_FILE):
        with open(SECRET_FILE, "w") as f:
            f.write(secrets.token_hex(32))
    with open(SECRET_FILE) as f:
        return f.read().strip()


app = Flask(__name__)
app.secret_key = load_secret_key()
app.permanent_session_lifetime = timedelta(hours=12)

SCHEMA = """
CREATE TABLE IF NOT EXISTS readings (
  id {pk},
  meter TEXT, received_at TEXT, meter_time TEXT, event TEXT,
  balance REAL, total REAL, volts REAL, amps REAL, watts REAL, relay INTEGER,
  simday INTEGER, daykwh REAL, pred REAL, daysleft REAL, recnad REAL, demo INTEGER, runout TEXT
);
CREATE INDEX IF NOT EXISTS ix_readings_meter ON readings(meter, id);
CREATE TABLE IF NOT EXISTS topups (
  id {pk},
  meter TEXT, kwh REAL, nad REAL, status TEXT,
  created_at TEXT, sent_at TEXT, confirmed_at TEXT
);
"""


# ---------------------------------------------------------------- database
# The queries are written for SQLite ("?" placeholders, rows readable by name or index).
# For Postgres a thin wrapper gives the same interface, so the rest of the app is unchanged.
class PgRow:
    """Row readable as r["col"], r[0], r.keys(), and iterable over its values (like sqlite3.Row)."""
    __slots__ = ("_names", "_values", "_index")

    def __init__(self, names, values):
        self._names, self._values = names, values
        self._index = {n: i for i, n in enumerate(names)}

    def __getitem__(self, k):
        return self._values[k] if isinstance(k, int) else self._values[self._index[k]]

    def keys(self):
        return list(self._names)

    def __iter__(self):
        return iter(self._values)

    def __len__(self):
        return len(self._values)


class PgDB:
    def __init__(self, url):
        import psycopg
        # prepare_threshold=None: Neon's pooled (PgBouncer) URL does not keep prepared statements
        self.con = psycopg.connect(url, prepare_threshold=None, connect_timeout=10)

    @staticmethod
    def _sql(sql):
        return sql.replace("?", "%s").replace("(id % %s)", "(id %% %s)")

    def _cursor(self):
        def factory(c):
            names = [d.name for d in (c.description or [])]       # INSERT/UPDATE: no columns
            return lambda vals: PgRow(names, vals)
        return self.con.cursor(row_factory=factory)

    def execute(self, sql, params=()):
        cur = self._cursor()
        cur.execute(self._sql(sql), params)
        return cur

    def executemany(self, sql, seq):
        cur = self._cursor()
        cur.executemany(self._sql(sql), seq)
        return cur

    def commit(self):
        self.con.commit()

    def close(self):
        self.con.close()


def connect_db():
    if USE_PG:
        return PgDB(DATABASE_URL)
    con = sqlite3.connect(DB_PATH)
    con.row_factory = sqlite3.Row
    return con


def db():
    if "db" not in g:
        g.db = connect_db()
    return g.db


@app.teardown_appcontext
def close_db(_exc):
    d = g.pop("db", None)
    if d is not None:
        d.close()


def init_db():
    con = connect_db()
    if USE_PG:
        for stmt in SCHEMA.format(pk="BIGSERIAL PRIMARY KEY").split(";"):
            if stmt.strip():
                con.execute(stmt)
        con.commit()
    else:
        con.executescript(SCHEMA.format(pk="INTEGER PRIMARY KEY AUTOINCREMENT"))
    con.close()
    if not ON_VERCEL:
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


# ---------------------------------------------------------------- authentication
# Every browser page and browser API needs a signed-in session. /api/meter stays open so the
# ESP32 (which has no login) keeps working. The account is created once, from this PC only.
OPEN_ENDPOINTS = {"login", "setup", "logout", "static", "meter_api"}
LOCAL_ADDRS = {"127.0.0.1", "::1"}


def load_account():
    if not os.path.exists(AUTH_FILE):
        return None
    with open(AUTH_FILE) as f:
        return json.load(f)


def save_account(username, password):
    with open(AUTH_FILE, "w") as f:
        json.dump({"username": username, "password_hash": generate_password_hash(password),
                   "created_at": utcnow()}, f, indent=2)


@app.before_request
def require_login():
    if PUBLIC_DEMO or request.endpoint in OPEN_ENDPOINTS or session.get("user"):
        return None
    if request.path.startswith(("/api/", "/export/")):          # page scripts: tell them to re-login
        return jsonify(error="login required"), 401
    if load_account() is None:
        return redirect(url_for("setup"))
    return redirect(url_for("login", next=request.full_path if request.query_string else request.path))


PROXY_HEADERS = ("Cf-Connecting-Ip", "Cf-Ray", "X-Forwarded-For", "X-Real-Ip", "Forwarded")


def is_local_request():
    """True only for a browser on this PC. A tunnel (Cloudflare, ngrok) also connects from
    127.0.0.1, but adds forwarding headers - those visitors are NOT local."""
    return request.remote_addr in LOCAL_ADDRS and not any(h in request.headers for h in PROXY_HEADERS)


def safe_next(target):
    """Only redirect back to a path on this site (no external redirects)."""
    return target if target and target.startswith("/") and not target.startswith("//") else url_for("dashboard")


@app.route("/login", methods=["GET", "POST"])
def login():
    if PUBLIC_DEMO:
        return redirect(url_for("dashboard"))
    account = load_account()
    if account is None:
        return redirect(url_for("setup"))
    error = None
    if request.method == "POST":
        user = request.form.get("username", "").strip()
        pw = request.form.get("password", "")
        if user == account["username"] and check_password_hash(account["password_hash"], pw):
            session.clear()
            session.permanent = True
            session["user"] = user
            return redirect(safe_next(request.args.get("next")))
        time.sleep(1)                                            # slows down password guessing
        error = "Wrong username or password."
    return render_template("login.html", mode="login", error=error)


@app.route("/setup", methods=["GET", "POST"])
def setup():
    if PUBLIC_DEMO:
        return redirect(url_for("dashboard"))
    if load_account() is not None:
        return redirect(url_for("login"))
    local = is_local_request()
    error = None
    if request.method == "POST" and local:
        user = request.form.get("username", "").strip()
        pw = request.form.get("password", "")
        if len(user) < 3:
            error = "Username must have at least 3 characters."
        elif len(pw) < 8:
            error = "Password must have at least 8 characters."
        elif pw != request.form.get("password2", ""):
            error = "The two passwords do not match."
        else:
            save_account(user, pw)
            session.clear()
            session.permanent = True
            session["user"] = user
            return redirect(url_for("dashboard"))
    return render_template("login.html", mode="setup", error=error, local=local)


@app.route("/logout")
def logout():
    session.clear()
    return redirect(url_for("login"))


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
        # The web app's prediction goes back too, so the meter's LCD shows exactly the same
        # days left / recommended top-up as the web page (days -1 = not known yet).
        last = latest_reading(meter)
        days, rec, pred = -1.0, 0, 0.0
        if last is not None:
            fc, _ = prediction_for(meter, last)
            days = fc["days_left"] if fc["days_left"] is not None else -1.0
            rec, pred = fc["rec_nad"], fc["pred_kwh_day"]
        # written by hand so the firmware's  indexOf("\"kwh\":")  always finds it
        return Response('{"kwh":%.4f,"days":%.1f,"rec":%d,"pred":%.5f}' % (kwh, days, rec, pred),
                        mimetype="application/json")

    return jsonify(error="unknown action"), 400


# ---------------------------------------------------------------- data for the browser
def prediction_for(meter, last):
    rows = session_readings(meter)
    history = predictor.daily_history(rows)
    # run-out / top-up-by dates count from the PC's real clock, not the meter's (fast) demo calendar
    now = datetime.now(LOCAL_TZ).replace(tzinfo=None)               # Vercel's servers run on UTC
    live_w = last["watts"] if last and last["relay"] else 0.0
    load_on = bool(last and last["relay"] and last["amps"] > 0)     # same rule as the "Load: ON" status
    model = predictor.load_model(MODEL_DIR)
    fc = predictor.forecast(last["balance"] if last else 0.0, history, live_w, now, model,
                            TARIFF_NAD_PER_KWH, TARGET_DAYS, load_on=load_on)
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
    if ON_VERCEL and (request.method == "POST" or request.args.get("remove")):
        flash("The hosted demo is read-only: the model is changed by updating models/model.json in the "
              "GitHub repository (or on the laptop version).", "error")
        return redirect(url_for("model_page"))
    if request.method == "POST":
        f = request.files.get("model")
        if not f or not f.filename:
            flash("Choose the model.json file downloaded from Colab.", "error")
        elif f.filename.endswith(".json"):
            import json
            try:
                info = json.load(f.stream)
                assert len(info["coef"]) == len(info.get("features", predictor.FEATURES))
                known = predictor.all_features([1.0] * 14)               # every input the web app can compute
                assert all(f in known for f in info.get("features", predictor.FEATURES))
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
                           features=model.info.get("features", predictor.FEATURES) if model else predictor.FEATURES,
                           min_history=predictor.model_min_history(model))


init_db()

if __name__ == "__main__":
    # host 0.0.0.0 so the ESP32 (and phones) on the same WiFi can reach it
    app.run(host="0.0.0.0", port=int(os.environ.get("PORT", 5000)), debug=False, threaded=True)
