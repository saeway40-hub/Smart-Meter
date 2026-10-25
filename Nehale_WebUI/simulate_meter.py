"""
Pretends to be the ESP32 so you can test the web page without the hardware.
It sends exactly the same requests as the firmware (action=log every few seconds,
action=poll for web top-ups) and runs the same demo-mode clock.

    python simulate_meter.py                     # server on http://localhost:5000
    python simulate_meter.py --url http://192.168.1.20:5000/api/meter --factor 20000

Usage varies a little from day to day so the history and the model have something to learn.
"""
import argparse
import json
import math
import random
import time
import urllib.parse
import urllib.request
from datetime import datetime, timedelta

ap = argparse.ArgumentParser()
ap.add_argument("--url", default="http://localhost:5000/api/meter")
ap.add_argument("--meter", default="NEHALE-01")
ap.add_argument("--factor", type=float, default=20000, help="demo speed: simulated seconds per real second")
ap.add_argument("--log", type=float, default=2.0, help="real seconds between log messages")
ap.add_argument("--start-kwh", type=float, default=3.0, help="first top-up (like token 1245)")
args = ap.parse_args()

balance, total = 0.0, 0.0
meter_t = datetime.now().replace(second=0, microsecond=0)
volts = 12.1


def get(params):
    url = args.url + "?" + urllib.parse.urlencode(params)
    try:
        with urllib.request.urlopen(url, timeout=5) as r:
            return r.read().decode()
    except Exception as e:
        print("  ! server not reachable:", e)
        return ""


def log(event, watts, relay):
    amps = watts / volts if relay else 0
    get({"action": "log", "meter": args.meter, "event": event, "balance": f"{balance:.4f}",
         "total": f"{total:.5f}", "volts": f"{volts:.2f}", "amps": f"{amps:.3f}", "watts": f"{watts:.2f}",
         "relay": int(relay), "simday": 0, "daykwh": 0, "pred": 0, "daysleft": 0, "recnad": 0, "demo": 1,
         "date": meter_t.strftime("%Y-%m-%d %H:%M"), "runout": ""})


def day_watts(t):
    """6 W load with a daily pattern: some hours off, slightly different every day."""
    random.seed(t.toordinal())
    base = 6.0 * random.uniform(0.7, 1.15)
    if t.weekday() >= 5:
        base *= 1.2                          # weekends a bit higher
    hour_on = 0.35 + 0.65 * (math.sin((t.hour - 6) / 24 * 2 * math.pi) > -0.6)
    return base * hour_on


balance = args.start_kwh
log("TOPUP_KEYPAD", 0, 1)
print(f"Simulating {args.meter} -> {args.url}  (1 s = {args.factor:.0f} meter seconds)")
last_poll = 0
while True:
    relay = balance > 0.0005
    w = day_watts(meter_t) * random.uniform(0.97, 1.03) if relay else 0.0
    dt_sim = args.log * args.factor if relay else args.log
    used = min(w * dt_sim / 3_600_000, balance)
    balance -= used
    total += used
    meter_t += timedelta(seconds=dt_sim)
    event = "READING"
    if relay and balance <= 0.0005:
        balance, event = 0.0, "CUTOFF"
    log(event, w, relay)
    print(f"{meter_t:%Y-%m-%d %H:%M}  P {w:5.2f} W  bal {balance:7.4f} kWh  total {total:7.4f}")

    if time.time() - last_poll > 4:
        last_poll = time.time()
        r = get({"action": "poll", "meter": args.meter})
        try:
            kwh = json.loads(r or "{}").get("kwh", 0)
        except ValueError:
            kwh = 0
        if kwh > 0:
            balance += kwh
            print(f"  >> web top-up +{kwh} kWh")
            log("TOPUP_WEB", 0, 1)
    time.sleep(args.log)
