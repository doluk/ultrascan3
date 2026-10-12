#!/usr/bin/env python3
"""Runs clazy-standalone on the files changed since <base-ref> and reports only findings on ADDED lines.

usage: clazy-changed.py <base-ref> <compile-db-dir> [checks]
  checks   clazy check list, default $CLAZY_CHECKS or "level0,level1"

Prints GitHub workflow annotations (::warning file=..,line=..) and a markdown table to $GITHUB_STEP_SUMMARY.
Exit code 1 only if CLAZY_BLOCKING=true and there are findings.
"""
import json, os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

SCOPE = ("gui/", "utils/", "programs/", "test/")
SRC = (".cpp", ".cc", ".cxx")
HDR = (".h", ".hpp")
DIAG = re.compile(r"^(?P<file>[^\s:]+):(?P<line>\d+):(?P<col>\d+): warning: (?P<msg>.*) \[-Wclazy-(?P<check>[\w-]+)\]$")


def main():
    base, db = sys.argv[1], sys.argv[2]
    checks = sys.argv[3] if len(sys.argv) > 3 else os.environ.get("CLAZY_CHECKS", "level0,level1")
    root = os.getcwd()

    diff = subprocess.run(["git", "diff", "-U0", "--no-color", "--diff-filter=AM", base], check=True,
                          capture_output=True, text=True, errors="replace").stdout
    added, cur = {}, None
    for l in diff.splitlines():
        if l.startswith("+++ b/"):
            cur = l[6:]
        elif l.startswith("@@") and cur and cur.startswith(SCOPE) and cur.endswith(SRC + HDR):
            m = re.search(r"\+(\d+)(?:,(\d+))?", l)
            start, n = int(m.group(1)), int(1 if m.group(2) is None else m.group(2))
            added.setdefault(cur, set()).update(range(start, start + n))

    # translation units: changed sources, plus one source of the same directory per changed header
    units = {f for f in added if f.endswith(SRC)}
    in_db = {os.path.relpath(e["file"], root) if os.path.isabs(e["file"]) else e["file"]
             for e in json.load(open(os.path.join(db, "compile_commands.json")))}
    units &= in_db
    for h in (f for f in added if f.endswith(HDR)):
        tus = sorted(t for t in in_db if os.path.dirname(t) == os.path.dirname(h))
        if tus and not any(t in units for t in tus):
            units.add(tus[0])
    if not units:
        print("clazy: nothing to analyse")
        return 0

    def run(unit):
        r = subprocess.run(["clazy-standalone", "-p", db, "--checks=" + checks, unit], capture_output=True, text=True, errors="replace")
        return r.stdout + r.stderr

    findings, seen, failed = [], set(), []
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as ex:
        for unit, out in zip(sorted(units), ex.map(run, sorted(units))):
            errors = [l for l in out.splitlines() if re.search(r": (fatal )?error: ", l)]
            if errors:   # clang could not parse the file: clazy results are incomplete or missing
                failed.append((unit, errors[0]))
            for l in out.splitlines():
                m = DIAG.match(l)
                if not m:
                    continue
                f = os.path.relpath(m["file"], root) if os.path.isabs(m["file"]) else m["file"]
                key = (f, m["line"], m["check"])
                if key in seen or int(m["line"]) not in added.get(f, ()):
                    continue
                seen.add(key)
                findings.append((f, m["line"], m["col"], m["check"], m["msg"]))

    for unit, err in failed:
        print("::warning file=%s,title=clazy could not analyse the file::%s" % (unit, err.replace(root + "/", "")))
    for f, ln, col, chk, msg in sorted(findings):
        print("::warning file=%s,line=%s,col=%s,title=clazy %s::%s" % (f, ln, col, chk, msg))
    print("clazy (%s): %d finding(s) on changed lines, %d file(s) could not be analysed" % (checks, len(findings), len(failed)))
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a") as s:
            s.write("### clazy (%s)\n\n" % checks)
            for unit, err in failed:
                s.write("> :warning: `%s` could not be parsed, clazy results are incomplete: `%s`\n\n" % (unit, err.replace(root + "/", "")))
            if findings:
                s.write("| File | Line | Check | Message |\n|---|---|---|---|\n")
                for f, ln, col, chk, msg in sorted(findings):
                    s.write("| `%s` | %s | `%s` | %s |\n" % (f, ln, chk, msg.replace("|", "\\|")))
            else:
                s.write("No findings on changed lines.\n")
    return 1 if (findings or failed) and os.environ.get("CLAZY_BLOCKING") == "true" else 0


sys.exit(main())
