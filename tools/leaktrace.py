#!/usr/bin/env python3
"""Diff two tools/leaktrace.c dumps from the same process and show the stacks
whose live bytes grew, symbolized with addr2line where debug info exists.

    python3 tools/leaktrace.py /tmp/lt.000060 /tmp/lt.000600 [--top 10]
"""
import argparse
import re
import subprocess


def parse(path):
    stacks, cur = {}, None
    with open(path) as f:
        for line in f:
            m = re.match(r"stack (\w+) count=(\d+) bytes=(\d+)", line)
            if m:
                cur = {"count": int(m[2]), "bytes": int(m[3]), "frames": []}
                stacks[m[1]] = cur
            elif line.startswith("  ") and cur is not None:
                cur["frames"].append(line.strip())
    return stacks


def symbolize(frame, cache={}):
    if frame in cache:
        return cache[frame]
    out = frame
    m = re.match(r"(\S+)\+(0x[0-9a-f]+) (\S+)", frame)
    if m and not m[1].endswith(".so") and ".so." not in m[1]:
        # executables: dladdr gives the link-time offset of a non-PIE binary
        try:
            r = subprocess.run(["addr2line", "-f", "-C", "-e", m[1], hex(int(m[2], 16) - 1)],
                               capture_output=True, text=True, timeout=10)
            fn, loc = (r.stdout.splitlines() + ["?", "?"])[:2]
            out = f"{fn} {loc}"
        except (OSError, subprocess.SubprocessError):
            pass
    elif m:
        out = f"{m[1].rsplit('/', 1)[-1]} {m[3]}+{m[2]}"
    cache[frame] = out
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("before")
    ap.add_argument("after")
    ap.add_argument("--top", type=int, default=10)
    args = ap.parse_args()
    a, b = parse(args.before), parse(args.after)
    rows = []
    for key, s in b.items():
        old = a.get(key, {"count": 0, "bytes": 0})
        rows.append((s["bytes"] - old["bytes"], s["count"] - old["count"], s, key))
    rows.sort(key=lambda r: r[0], reverse=True)
    for grow, count, s, key in rows[:args.top]:
        if grow <= 0:
            break
        print(f"+{grow} bytes  +{count} allocs  (live {s['bytes']} in {s['count']})  stack {key}")
        for fr in s["frames"]:
            print("    " + symbolize(fr))
    print("note: dumps list only each run's top stacks; a stack missing from the first dump counts from zero")


if __name__ == "__main__":
    main()
