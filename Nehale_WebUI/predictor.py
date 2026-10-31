"""
Usage history + prediction for the Nehale smart prepaid meter.

* daily_history()  turns the raw readings the ESP32 logs into kWh per calendar day
                   (same rules as the firmware: a day needs >= 25 % of its time with
                   credit, and is scaled up to a full 24 h day).
* load_model()     loads the model exported from the Google Colab notebook
                   (models/model.json, or a scikit-learn models/model.joblib).
* forecast()       predicts kWh/day, days left, run-out date and the recommended top-up.

The model uses the same 4 features as the firmware:
    lag1 = yesterday, lag2 = 2 days ago, lag3 = 3 days ago, mean7 = mean of the last 7 days
"""
import json
import math
import os
from datetime import datetime, timedelta

FEATURES = ["lag1", "lag2", "lag3", "mean7"]
MIN_DAY_FRACTION = 0.25          # same as the firmware
DAY_S = 86400.0


# ---------------------------------------------------------------- history
def parse_meter_time(s):
    """Meter sends 'YYYY-MM-DD HH:MM' (Namibia time). Returns a naive datetime or None."""
    if not s or s == "--":
        return None
    for fmt in ("%Y-%m-%d %H:%M", "%Y-%m-%d %H:%M:%S", "%Y-%m-%d"):
        try:
            return datetime.strptime(s.strip(), fmt)
        except ValueError:
            pass
    return None


def _split_over_days(t0, t1):
    """Yields (date, seconds) for the interval t0..t1, cut at every midnight."""
    t = t0
    while t < t1:
        midnight = datetime(t.year, t.month, t.day) + timedelta(days=1)
        end = min(midnight, t1)
        yield t.date(), (end - t).total_seconds()
        t = end


def daily_history(readings):
    """
    readings: rows ordered oldest -> newest, each with keys meter_time, total, relay.
    Energy used between two readings is shared over the meter-calendar time between them
    (in DEMO_MODE one 10 s log interval can cover several meter days).
    Returns a list of dicts (oldest first) for every finished day:
        date, kwh_used, credit_hours, credit_fraction, kwh_per_day (None if not valid), valid
    """
    days = {}
    prev = None
    prev_t = None
    last_date = None
    for r in readings:
        t = parse_meter_time(r["meter_time"])
        if t is None:
            continue
        last_date = t.date()
        if prev is None:
            prev, prev_t = r, t
            days.setdefault(t.date(), [0.0, 0.0])
            continue
        inc = (r["total"] or 0) - (prev["total"] or 0)
        if inc < -1e-9:                       # meter restarted (START_FROM_ZERO): total went back to 0
            inc = max(r["total"] or 0, 0.0)
        credit = bool(prev["relay"]) or inc > 1e-9
        if t > prev_t:
            span = (t - prev_t).total_seconds()
            for d, sec in _split_over_days(prev_t, t):
                cell = days.setdefault(d, [0.0, 0.0])
                cell[0] += inc * sec / span
                if credit:
                    cell[1] += sec
        else:                                 # clock went backwards (reboot in demo mode)
            days.setdefault(t.date(), [0.0, 0.0])[0] += inc
        prev, prev_t = r, t

    out = []
    for d in sorted(days):
        if d == last_date:                    # today is not finished yet
            continue
        used, credit_s = days[d]
        frac = min(credit_s / DAY_S, 1.0)
        valid = frac >= MIN_DAY_FRACTION
        out.append({
            "date": d.isoformat(),
            "kwh_used": round(used, 6),
            "credit_hours": round(credit_s / 3600.0, 2),
            "credit_fraction": round(frac, 3),
            "kwh_per_day": round(used / frac, 6) if valid else None,
            "valid": valid,
        })
    return out


def today_usage(readings):
    """kWh used so far on the meter's current date."""
    if not readings:
        return 0.0
    last_t = parse_meter_time(readings[-1]["meter_time"])
    if last_t is None:
        return 0.0
    used, prev, prev_t = 0.0, None, None
    for r in readings:
        t = parse_meter_time(r["meter_time"])
        if t is None:
            continue
        if prev is not None:
            inc = (r["total"] or 0) - (prev["total"] or 0)
            if inc < -1e-9:
                inc = max(r["total"] or 0, 0.0)
            if t > prev_t:
                span = (t - prev_t).total_seconds()
                for d, sec in _split_over_days(prev_t, t):
                    if d == last_t.date():
                        used += inc * sec / span
            elif t.date() == last_t.date():
                used += inc
        prev, prev_t = r, t
    return used


# ---------------------------------------------------------------- model
class Model:
    """Wraps the model trained in Google Colab."""

    def __init__(self, info, sk_model=None):
        self.info = info
        self.sk = sk_model

    @property
    def name(self):
        return self.info.get("name", "Colab model")

    def predict(self, feats):
        x = [feats[f] for f in self.info.get("features", FEATURES)]
        if self.sk is not None:
            y = float(self.sk.predict([x])[0])
        else:
            y = self.info.get("intercept", 0.0) + sum(c * v for c, v in zip(self.info["coef"], x))
        return max(y, 0.0)


def load_model(model_dir):
    """Prefers models/model.json (portable). Falls back to models/model.joblib (needs scikit-learn)."""
    jpath = os.path.join(model_dir, "model.json")
    if os.path.exists(jpath):
        with open(jpath) as f:
            info = json.load(f)
        if "coef" in info and len(info["coef"]) == len(info.get("features", FEATURES)):
            info["file"] = "model.json"
            return Model(info)
    kpath = os.path.join(model_dir, "model.joblib")
    if os.path.exists(kpath):
        try:
            import joblib
            obj = joblib.load(kpath)
            if isinstance(obj, dict):
                info = {k: v for k, v in obj.items() if k != "model"}
                sk = obj["model"]
            else:
                info, sk = {}, obj
            info.setdefault("name", type(sk).__name__)
            info.setdefault("features", FEATURES)
            info["file"] = "model.joblib"
            return Model(info, sk)
        except Exception as e:                # wrong scikit-learn version, etc.
            print("[model] could not load model.joblib:", e)
    return None


# ---------------------------------------------------------------- forecast
def _mean(v):
    return sum(v) / len(v)


def all_features(hist):
    """hist: kWh/day values, oldest first (at least 7). Every feature a model may use;
    a model reads only the ones listed in its model.json. lag7 = same weekday last week."""
    n = len(hist)
    f = {f"lag{k}": hist[-k] if n >= k else hist[0] for k in range(1, 8)}
    f["mean3"] = _mean(hist[-3:])
    f["mean7"] = _mean(hist[-7:])
    f["mean14"] = _mean(hist[-14:])
    return f


def features_from(hist):
    """Kept for compatibility (firmware features: lag1, lag2, lag3, mean7)."""
    return all_features(hist)


def model_min_history(model):
    return int(model.info.get("min_history", 7)) if model is not None else 7


def next_day(hist, model):
    if model is not None and len(hist) >= model_min_history(model):
        return model.predict(all_features(hist)), "model"
    if hist:
        last = hist[-7:]
        return sum(last) / len(last), "average"
    return 0.0, "none"


def forecast(balance_kwh, history, live_watts, now, model, tariff, target_days=30, horizon=7, load_on=True):
    """
    balance_kwh : current balance
    history     : output of daily_history()
    live_watts  : power the load is drawing right now
    now         : date/time the run-out date is counted from (the PC's real clock)
    load_on     : False = nothing is drawing current -> no units are used: days left "--", no top-up needed
    Returns a dict that the web page shows.
    """
    hist = [d["kwh_per_day"] for d in history if d["valid"]]
    live_kwh_day = max(live_watts, 0.0) * 24.0 / 1000.0

    # 1. predicted kWh for the next days (recursive: each prediction feeds the next one)
    h = list(hist)
    rates = []
    method = "none"
    for _ in range(max(horizon, int(target_days), 1)):
        p, m = next_day(h, model)
        if m == "none":
            break
        method = m
        rates.append(p)
        h.append(p)

    if not rates and live_kwh_day > 0:        # no history yet: assume the present load keeps running
        method = "live"
        rates = [live_kwh_day] * max(horizon, int(target_days))

    rate_next = rates[0] if rates else 0.0

    # 2. days left: walk the balance down day by day
    days_left = None
    if balance_kwh <= 0:
        days_left = 0.0
    elif rates and rate_next > 1e-6:
        bal, d, i, hh = balance_kwh, 0.0, 0, list(h)
        while d < 999:
            if i < len(rates):
                p = rates[i]
            else:
                p, _ = next_day(hh, model) if method in ("model", "average") else (rate_next, None)
                hh.append(p)
            if p <= 1e-9:
                d = 999
                break
            if bal <= p:
                d += bal / p
                break
            bal -= p
            d += 1
            i += 1
        days_left = round(d, 1)

    days_left_live = round(balance_kwh / live_kwh_day, 1) if live_kwh_day > 1e-6 else None

    # load switched off: the balance is not going down, so there is no run-out to show ("--")
    load_off = balance_kwh > 0 and not load_on
    if load_off:
        days_left = None

    run_out = (now + timedelta(days=days_left) if days_left is not None and balance_kwh > 0 and not load_off
               else None)
    topup_by = None
    if run_out is not None:
        topup_by = max(now, run_out - timedelta(days=2))  # 2-day safety margin

    # 3. recommended top-up: enough for target_days of forecast use, minus what is left
    need_kwh = sum(rates[:int(target_days)]) - balance_kwh if rates and not load_off else 0.0
    need_kwh = max(need_kwh, 0.0)
    rec_nad = math.ceil(need_kwh * tariff) if need_kwh > 1e-6 else 0

    # 4. status message
    if balance_kwh <= 0:
        status, msg = "empty", "No credit - the load is switched off. Top up now."
    elif load_off:
        status, msg = "ok", "The load is off - no units are being used."
    elif days_left is None:
        status, msg = "unknown", "Not enough data yet - switch the load on so the meter can learn your usage."
    elif days_left < 3:
        status, msg = "urgent", "Your units are almost finished. Top up today."
    elif days_left < 7:
        status, msg = "soon", "Plan a top-up this week."
    else:
        status, msg = "ok", "You have enough units for now."

    # 5. day-by-day table
    table, bal = [], balance_kwh
    for i in range(horizon):
        p = rates[i] if i < len(rates) and not load_off else 0.0
        bal = max(bal - p, 0.0)
        day = (now + timedelta(days=i + 1)).date()
        table.append({"date": day.isoformat(), "label": day.strftime("%a %d %b"),
                      "use_kwh": round(p, 4), "balance_kwh": round(bal, 4)})

    method_label = {
        "model": f"{model.name if model else 'Model'} (Google Colab)",
        "average": "Average of the last %d days (model needs %d days of history)" % (min(len(hist), 7),
                                                                                 model_min_history(model))
                   if model else "Average of the last %d days (no Colab model loaded)" % min(len(hist), 7),
        "live": "Present load x 24 h (no daily history yet)",
        "none": "No data yet",
    }[method]

    return {
        "method": method,
        "method_label": method_label,
        "history_days": len(hist),
        "pred_kwh_day": round(rate_next, 5),
        "live_kwh_day": round(live_kwh_day, 5),
        "days_left": days_left,
        "days_left_live": days_left_live,
        "run_out": run_out.strftime("%a %d %b %Y") if run_out else None,
        "run_out_iso": run_out.date().isoformat() if run_out else None,
        "topup_by": topup_by.strftime("%a %d %b %Y") if topup_by else None,
        "rec_kwh": round(need_kwh, 3),
        "rec_nad": rec_nad,
        "target_days": target_days,
        "status": status,
        "message": msg,
        "table": table,
    }
