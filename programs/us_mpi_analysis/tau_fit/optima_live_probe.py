#!/usr/bin/env python3
"""Read-only probe of the Optima AUC live-data WebSocket.

The Optima web interface (LiveData.js) requests live scans over the
WebSocket ws://<optima>/OptimaAUC//websocket//graph: it sends a
LiveDataParameters JSON object and receives [parameterData, graphData], one
graphData element per scan line (positions, v1_Array, scanSeqNum, stageNum,
wavelength, radialPath, replicate, ...).  The page resends the returned
parameterData on every refresh.  This probe does the same, logs every frame
with its receive time, and summarises when each scan line appears, to learn
the scan order of the channels and wavelengths and the time a scan takes.

  probe      poll the instrument and log the frames (JSON lines)
  summarize  evaluate a log: fields sent, scan lines, appearance times

It only sends the requests the live-data page sends; keep the interval at
1 s or more (the server runs on the instrument computer).

    pip install websocket-client
    optima_live_probe.py probe --host 192.168.1.10 --cell 4 --wavelength 220 \\
        --wavelength 230 --minutes 30 --log cell4.jsonl
    optima_live_probe.py summarize cell4.jsonl

If the server needs the browser session, copy its cookie from the browser
developer tools (Network, the request of the page, header "Cookie") and pass
it with --cookie "JSESSIONID=...".  If the request is refused, copy an
outgoing frame of the page (developer tools, Network, WS, graph, Messages)
into a file and pass it with --template request.json.
"""

import argparse
import json
import sys
import time
from collections import OrderedDict

ARRAY_FIELDS = ("positions", "v1_Array", "values", "wavelengths")


def default_request(cell, wavelength, graph_no, sub_type, scans):
    """A LiveDataParameters object as LiveData.js builds it."""
    return OrderedDict([
        ("displayAll", False), ("cellNumber", str(cell)), ("runId", 0), ("runIdChanged", False),
        ("browserId", "us3-live-probe"), ("sendAllData", True), ("stopFetching", False),
        ("stageChanged", False), ("stageNum", 0), ("xMin", 0), ("xMax", 0), ("yMin", 0), ("yMax", 0),
        ("dataChanged", False), ("startId", 0), ("endId", -1),
        ("currentGraphId", "livechartTdId%d" % graph_no),
        ("expType", "Abs"), ("expSubType", sub_type),
        ("includeEvery", False), ("includeRecent", True), ("scans", str(scans)),
        ("includeIntensityImages", False), ("waveLength", [str(wavelength)]),
        ("absAndIntensity", True), ("absorbanceRadioBtnSelected", sub_type == "Abs"),
    ])


def probe(a):
    try:
        import websocket  # websocket-client
    except ImportError:
        sys.exit("needs the websocket-client package: pip install websocket-client")

    url = a.url or "ws://%s/OptimaAUC//websocket//graph" % a.host
    if a.template:
        with open(a.template) as f:
            base = json.load(f, object_pairs_hook=OrderedDict)
        requests = []
        for k, wl in enumerate(a.wavelength or [None]):
            req = OrderedDict(base)
            if a.cell:
                req["cellNumber"] = str(a.cell)
            if wl is not None:
                req["waveLength"] = [str(wl)]
            req["currentGraphId"] = "livechartTdId%d" % (k + 1)
            requests.append(req)
    else:
        if not a.cell or not a.wavelength:
            sys.exit("give --cell and --wavelength (or --template)")
        requests = [default_request(a.cell, wl, k + 1, a.sub_type, a.scans) for k, wl in enumerate(a.wavelength)]

    headers = ["Cookie: %s" % a.cookie] if a.cookie else None
    print("connecting to %s" % url)
    ws = websocket.create_connection(url, timeout=a.timeout, header=headers)
    t_end = time.time() + 60.0 * a.minutes
    nframes = 0
    with open(a.log, "a") as log:
        while time.time() < t_end:
            for k, req in enumerate(requests):
                t_send = time.time()
                ws.send(json.dumps(req))
                try:
                    text = ws.recv()
                except websocket.WebSocketTimeoutException:
                    print("%s  request %d: no answer within %g s" % (time.strftime("%H:%M:%S"), k, a.timeout))
                    continue
                t_recv = time.time()
                log.write(json.dumps({"t_send": t_send, "t_recv": t_recv, "request": k, "frame": text}) + "\n")
                log.flush()
                nframes += 1
                try:
                    data = json.loads(text)
                    params, lines = data[0], data[1]
                except Exception:
                    print("%s  request %d: unexpected frame (%d characters) logged" %
                          (time.strftime("%H:%M:%S"), k, len(text)))
                    continue
                # The page keeps the returned parameters and resends them
                if isinstance(params, dict):
                    params["currentGraphId"] = req["currentGraphId"]
                    params["runIdChanged"] = False
                    requests[k] = OrderedDict(params)
                desc = []
                for ln in lines if isinstance(lines, list) else []:
                    if isinstance(ln, dict):
                        desc.append("%s/%s/%s/%s n=%d" % (ln.get("stageNum"), ln.get("scanSeqNum"), ln.get("wavelength"),
                                                          ln.get("radialPath"), len(ln.get("positions") or [])))
                print("%s  request %d: %d line(s) %s" % (time.strftime("%H:%M:%S"), k, len(desc), "; ".join(desc[:6])))
            time.sleep(max(1.0, a.interval))
    ws.close()
    print("%d frames logged to %s" % (nframes, a.log))


def summarize(a):
    fields = OrderedDict()
    seen = OrderedDict()   # line key -> dict(first, last_change, counts, ...)
    param_fields = OrderedDict()
    nframes = 0
    with open(a.log) as f:
        for row in f:
            row = row.strip()
            if not row:
                continue
            entry = json.loads(row)
            t = entry["t_recv"]
            try:
                data = json.loads(entry["frame"])
                params, lines = data[0], data[1]
            except Exception:
                continue
            nframes += 1
            if isinstance(params, dict):
                for key, val in params.items():
                    param_fields.setdefault(key, val)
            for ln in lines if isinstance(lines, list) else []:
                if not isinstance(ln, dict):
                    continue
                for key, val in ln.items():
                    if key not in fields:
                        fields[key] = ("array[%d]" % len(val)) if isinstance(val, list) else repr(val)[:60]
                key = (ln.get("stageNum"), ln.get("scanSeqNum"), str(ln.get("wavelength")), ln.get("radialPath"),
                       ln.get("replicate"))
                pos = ln.get("positions") or []
                n = len(pos)
                rec = seen.get(key)
                if rec is None:
                    rec = seen[key] = dict(first=t, last=t, changed=t, n_first=n, n_last=n, sizes=[n],
                                           rmin=min(pos) if pos else None, rmax=max(pos) if pos else None,
                                           extra={k2: v for k2, v in ln.items() if k2 not in ARRAY_FIELDS})
                else:
                    rec["last"] = t
                    if n != rec["n_last"]:
                        rec["changed"] = t
                        rec["sizes"].append(n)
                    rec["n_last"] = n
                    if pos:
                        rec["rmin"] = min(pos) if rec["rmin"] is None else min(rec["rmin"], min(pos))
                        rec["rmax"] = max(pos) if rec["rmax"] is None else max(rec["rmax"], max(pos))

    print("%d frames" % nframes)
    print("\nfields of parameterData (first value):")
    for key, val in param_fields.items():
        print("   %-28s %s" % (key, repr(val)[:70]))
    print("\nfields of a scan line (first value; arrays with their length):")
    for key, val in fields.items():
        print("   %-28s %s" % (key, val))

    print("\nscan lines in order of first appearance")
    print("   %-9s %-6s %-6s %-6s %-5s %-4s %9s %9s %7s %7s %8s %8s  %s" % (
        "first", "stage", "scan", "wavel.", "chan.", "rep.", "dt_prev", "growing", "n", "n_end", "r_min", "r_max", "other fields"))
    t_prev = None
    t0 = min((r["first"] for r in seen.values()), default=0.0)
    for key, rec in sorted(seen.items(), key=lambda kv: kv[1]["first"]):
        stage, scan, wl, chan, rep = key
        dt = "" if t_prev is None else "%.1f" % (rec["first"] - t_prev)
        t_prev = rec["first"]
        grow = "%.1f s" % (rec["changed"] - rec["first"]) if len(rec["sizes"]) > 1 else "no"
        other = {k2: v for k2, v in rec["extra"].items()
                 if k2 not in ("stageNum", "scanSeqNum", "wavelength", "radialPath", "replicate")}
        print("   %9.1f %-6s %-6s %-6s %-5s %-4s %9s %9s %7d %7d %8s %8s  %s" % (
            rec["first"] - t0, stage, scan, wl, chan, rep, dt, grow, rec["n_first"], rec["n_last"],
            "%.4f" % rec["rmin"] if rec["rmin"] is not None else "-",
            "%.4f" % rec["rmax"] if rec["rmax"] is not None else "-", json.dumps(other)[:80]))
    print("\nfirst: seconds after the first line; dt_prev: seconds after the previous line; growing: "
          "time over which the point count of the line still changed (no: it arrived complete).")
    print("Lines arrive when the server has them; the time resolution is the poll interval.")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("probe", help="poll the live-data WebSocket and log the frames")
    p.add_argument("--host", help="host (and port) of the Optima web interface")
    p.add_argument("--url", help="full WebSocket URL instead of --host")
    p.add_argument("--cell", type=int, help="cell number")
    p.add_argument("--wavelength", action="append", help="wavelength (nm); repeat for several")
    p.add_argument("--sub-type", default="Int", choices=["Int", "Abs"],
                   help="intensity (Int, both channels' raw signal) or absorbance (Abs)")
    p.add_argument("--scans", type=int, default=2, help="most recent scans per request")
    p.add_argument("--interval", type=float, default=2.0, help="seconds between polls (>= 1)")
    p.add_argument("--minutes", type=float, default=20.0, help="how long to log")
    p.add_argument("--timeout", type=float, default=20.0, help="seconds to wait for an answer")
    p.add_argument("--cookie", help="Cookie header of the browser session, if needed")
    p.add_argument("--template", help="JSON file with an outgoing request frame of the page")
    p.add_argument("--log", default="optima_live.jsonl", help="log file (JSON lines, appended)")
    s = sub.add_parser("summarize", help="evaluate a log")
    s.add_argument("log")
    a = ap.parse_args()
    if a.cmd == "probe":
        if not (a.host or a.url):
            ap.error("give --host or --url")
        probe(a)
    else:
        summarize(a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
