"""Compare autoplay runs by GPU command thread cost: busy ms per frame and per 1000 draws in battle windows.

    python3 hwder/tools/linux_autoplay_compare.py runs/autoplay/<run> [...]

Frame rate at the busy spot varies by ~1 ms between identical runs (the enemies differ); the GPU thread's busy
time per draw is the steadier measure of translator cost.
"""
from pathlib import Path
import re
import statistics
import sys


def stats(run):
    log = (Path(run) / "hwder.log").read_text(errors="replace").splitlines()
    busy, draws, frames = [], [], []
    last_draws = None
    for line in log:
        if m := re.search(r"vk: frame \d+: draws (\d+)", line):
            last_draws = int(m.group(1))
        elif (m := re.search(r"gpu thread busy ([\d.]+) ms per frame", line)) and last_draws and last_draws > 900:
            busy.append(float(m.group(1)))
            draws.append(last_draws)
        elif m := re.search(r"display: 300 frames: avg ([\d.]+) ms", line):
            frames.append(float(m.group(1)))
    battle = [f for f in frames if f < 12.5][1:] or frames
    per_k = [b / d * 1000 for b, d in zip(busy, draws)]
    return (len(busy), statistics.mean(busy) if busy else 0, statistics.mean(draws) if draws else 0,
            statistics.median(per_k) if per_k else 0, statistics.mean(battle) if battle else 0)


for run in sys.argv[1:]:
    n, busy, draws, per_k, frame = stats(run)
    print(f"{Path(run).name}: {n} samples, GPU thread {busy:.2f} ms/frame at {draws:.0f} draws "
          f"= {per_k:.2f} ms per 1000 draws; frame {frame:.2f} ms ({1000 / frame if frame else 0:.0f} fps)")
