"""Compare two autoplay runs made by run-baseline.ps1 (runs/<name>/filmstrip).

Each run folder holds filmstrip/index.txt ("<capture> <seconds> <tag> present=<n>" per captured
frame), filmstrip/f_<capture>.raw (96x54 RGBA8 thumbnails) and filmstrip/drawlog.txt (one line per
draw, starting "<present> <draw-counter> ..."). The report shows how far the two runs agree:
frames in each, the first position in the per-frame draw-count series where the counts differ,
how many of the first 300 thumbnails hash identically (by position), and min/max/mean draws per frame.

Usage: python tools/compare_runs.py runs/baseline-7a4fee3 runs/after-split
"""
import argparse
import hashlib
import os
import re
import statistics
import sys


def filmstrip_dir(run):
    cand = os.path.join(run, "filmstrip")
    return cand if os.path.isdir(cand) else run


def read_index(strip):
    """[(capture, seconds, tag, present)] in capture order."""
    rows = []
    path = os.path.join(strip, "index.txt")
    if not os.path.isfile(path):
        return rows
    pat = re.compile(r"^(\d+)\s+([\d.]+)\s+(\S+)\s+present=(\d+)")
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = pat.match(line)
            if m:
                rows.append((int(m.group(1)), float(m.group(2)), m.group(3), int(m.group(4))))
    return rows


def frame_hashes(strip):
    """{capture: sha256 hex} for every f_<n>.raw."""
    out = {}
    pat = re.compile(r"^f_(\d+)\.raw$")
    for name in os.listdir(strip):
        m = pat.match(name)
        if not m:
            continue
        h = hashlib.sha256()
        with open(os.path.join(strip, name), "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        out[int(m.group(1))] = h.hexdigest()
    return out


def draw_counts(strip):
    """[(present, draws)] in present order, from drawlog.txt."""
    path = os.path.join(strip, "drawlog.txt")
    counts = {}
    size = 0
    if os.path.isfile(path):
        size = os.path.getsize(path)
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                parts = line.split(None, 2)
                if len(parts) >= 2 and parts[0].isdigit():
                    counts[int(parts[0])] = counts.get(int(parts[0]), 0) + 1
    return sorted(counts.items()), size


def summary(series):
    vals = [d for _, d in series]
    if not vals:
        return "no draws logged"
    return "min %d max %d mean %.1f over %d frames" % (min(vals), max(vals), statistics.fmean(vals), len(vals))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_a")
    ap.add_argument("run_b")
    ap.add_argument("--frames", type=int, default=300, help="thumbnails to compare by position (default 300)")
    a = ap.parse_args()
    runs = []
    for run in (a.run_a, a.run_b):
        strip = filmstrip_dir(run)
        if not os.path.isdir(strip):
            sys.exit(f"not a run folder: {run}")
        idx = read_index(strip)
        hashes = frame_hashes(strip)
        series, log_size = draw_counts(strip)
        runs.append((run, idx, hashes, series, log_size))
        print(f"{run}:")
        print(f"  filmstrip frames: {len(hashes)} thumbnails, {len(idx)} index rows"
              + (f", t={idx[0][1]:.1f}..{idx[-1][1]:.1f} s, presents {idx[0][3]}..{idx[-1][3]}" if idx else ""))
        print(f"  drawlog: {log_size} bytes, {sum(d for _, d in series)} draws, {summary(series)}")

    (_, idx_a, h_a, s_a, _), (_, idx_b, h_b, s_b, _) = runs
    first_diff = None
    for i, ((pa, da), (pb, db)) in enumerate(zip(s_a, s_b)):
        if da != db:
            first_diff = (i, pa, da, pb, db)
            break
    print("draw-count series:")
    if first_diff:
        i, pa, da, pb, db = first_diff
        print(f"  first differing frame: position {i} (A present {pa}: {da} draws, B present {pb}: {db} draws)")
    elif len(s_a) != len(s_b):
        print(f"  identical over the common {min(len(s_a), len(s_b))} frames; lengths differ ({len(s_a)} vs {len(s_b)})")
    else:
        print(f"  identical over all {len(s_a)} frames")

    # thumbnails by position in capture order (capture numbers restart at 0 in every run)
    ca = [c for c, *_ in idx_a] or sorted(h_a)
    cb = [c for c, *_ in idx_b] or sorted(h_b)
    n = min(a.frames, len(ca), len(cb))
    same = sum(1 for i in range(n) if h_a.get(ca[i]) and h_a.get(ca[i]) == h_b.get(cb[i]))
    print(f"thumbnails: {same} of the first {n} identical by SHA-256 (position-matched)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
