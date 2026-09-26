#!/usr/bin/env python3
"""Extract GVB departures from the Dutch GTFS feed (gtfs.ovapi.nl).

Usage:  python3 ferry_timetable.py [gtfs-nl.zip] [days]
Writes departures.json:
  {"generated": "...",
   "ferry": {"YYYYMMDD": ["HH:MM F7", ...], ...},
   "bus36": {"YYYYMMDD": ["HH:MM 36", ...], ...}}

Edit QUERIES below to add or change routes. Standard library only.
"""
import csv, io, json, sys, zipfile, datetime, unicodedata
from collections import defaultdict

# name: key in the output JSON
# lines: GVB line numbers, or None = all GVB ferries
# from / to: parts of stop names (case and accents ignored).
#   A trip counts if it stops at "from" and LATER at "to" (so direction is right).
QUERIES = [
    {"name": "ferry", "lines": None,   "from": "NDSM",    "to": "Centraal"},
    {"name": "bus36", "lines": ["36"], "from": "Ataturk", "to": "Olof Palme"},
]

ZIP = sys.argv[1] if len(sys.argv) > 1 else "gtfs-nl.zip"
DAYS = int(sys.argv[2]) if len(sys.argv) > 2 else 7
FERRY_TYPES = {"4", "1000", "1200"}

def norm(s):
    return unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode().lower()

def log(*a):
    print(*a, file=sys.stderr, flush=True)

z = zipfile.ZipFile(ZIP)

def rows(name):
    with z.open(name) as f:
        yield from csv.DictReader(io.TextIOWrapper(f, encoding="utf-8-sig"))

# 1. GVB routes wanted by any query
gvb = {a["agency_id"] for a in rows("agency.txt")
       if "gvb" in norm(a["agency_id"] + a.get("agency_name", ""))}
log(f"GVB agency ids: {sorted(gvb)}")

route_queries = defaultdict(list)   # route_id -> [query index]
route_name = {}
for r in rows("routes.txt"):
    if r.get("agency_id") not in gvb:
        continue
    short = r.get("route_short_name", "")
    for i, q in enumerate(QUERIES):
        ok = (r.get("route_type") in FERRY_TYPES or short.startswith("F")) if q["lines"] is None \
             else short in q["lines"]
        if ok:
            route_queries[r["route_id"]].append(i)
            route_name[r["route_id"]] = short
for i, q in enumerate(QUERIES):
    found = sorted({route_name[rid] for rid, qs in route_queries.items() if i in qs})
    log(f"[{q['name']}] routes: {found}")

# 2. trips
trips = {t["trip_id"]: (t["route_id"], t["service_id"])
         for t in rows("trips.txt") if t["route_id"] in route_queries}
log(f"Trips: {len(trips)}")

# 3. stop names
stops = {s["stop_id"]: norm(s["stop_name"]) for s in rows("stops.txt")}

# 4. stop times (big file, takes a minute or two)
log("Reading stop_times.txt ...")
seqs = defaultdict(list)
for r in rows("stop_times.txt"):
    if r["trip_id"] in trips:
        seqs[r["trip_id"]].append((int(r["stop_sequence"]), r["stop_id"], r["departure_time"]))

# 5. service dates, limited to the next DAYS days
today = datetime.date.today()
keep = {(today + datetime.timedelta(days=n)).strftime("%Y%m%d") for n in range(-1, DAYS)}
dates = defaultdict(set)
names = z.namelist()
if "calendar.txt" in names:
    wd = ["monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"]
    for c in rows("calendar.txt"):
        d = datetime.datetime.strptime(c["start_date"], "%Y%m%d").date()
        end = datetime.datetime.strptime(c["end_date"], "%Y%m%d").date()
        while d <= end:
            ds = d.strftime("%Y%m%d")
            if ds in keep and c[wd[d.weekday()]] == "1":
                dates[c["service_id"]].add(ds)
            d += datetime.timedelta(days=1)
if "calendar_dates.txt" in names:
    for c in rows("calendar_dates.txt"):
        if c["date"] not in keep:
            continue
        if c["exception_type"] == "1":
            dates[c["service_id"]].add(c["date"])
        elif c["exception_type"] == "2":
            dates[c["service_id"]].discard(c["date"])

# 6. departures per query
out = {q["name"]: defaultdict(list) for q in QUERIES}
for i, q in enumerate(QUERIES):
    f, t = norm(q["from"]), norm(q["to"])
    stops_seen = set()
    for tid, seq in seqs.items():
        rid, sid = trips[tid]
        if i not in route_queries[rid]:
            continue
        seq.sort()
        for j, (_, s, dep) in enumerate(seq):
            if f in stops.get(s, "") and any(t in stops.get(x[1], "") for x in seq[j + 1:]):
                stops_seen.add(stops.get(s))
                for d in dates[sid]:
                    out[q["name"]][d].append(f"{dep[:5]} {route_name[rid]}")
                break
    log(f"[{q['name']}] departure stops matched: {sorted(stops_seen)}")

result = {"generated": datetime.datetime.now().isoformat(timespec="minutes")}
for name, days in out.items():
    result[name] = {d: sorted(v) for d, v in sorted(days.items())}
    log(f"[{name}] {len(days)} days, {sum(map(len, days.values()))} departures")

with open("departures.json", "w") as fh:
    json.dump(result, fh, separators=(",", ":"))
log("Wrote departures.json")
