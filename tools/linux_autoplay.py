"""Unattended autoplay run of the Linux build on the Ally (Bazzite), run on the host, outside the container.

Each run gets its own folder under runs/autoplay/: a hard link of the play build's hwder, copies of its
settings and pipeline caches, and a copy of a fixed save. The play folder, its save and the fixtures are never
written. The game runs in the hwder-dev container, by default inside a headless gamescope (an invisible
virtual display), and is stopped after --seconds. A summary of the frame-time, 120 fps and thread lines of
hwder.log goes to summary.txt, and --filmstrip writes a contact sheet of battle thumbnails (filmstrip.png).

    python3 hwder/tools/linux_autoplay.py --fixture autoplay --mode 120 --seconds 90 --name base120
"""
import argparse
import datetime
import os
from pathlib import Path
import re
import shutil
import signal
import struct
import subprocess
import sys
import time
import zlib

ROOT = Path("/var/home/bazzite/Documents/HWDER-linux-native")
PLAY = ROOT / "cache/local-build/play"
RUNS = ROOT / "runs/autoplay"
SAVE_LEAF = Path("0100AE00096EA000/48574445525F55530000000000000001")
FIXTURES = {  # fixed saves copied from hywar/test-saves (sha256 of zmha.bin)
    "autoplay": "9038321106d28444dc26864749274c8403ef3222b741008f60349a21dc07bfbe",
    "busy-spot": "38e73f2680b232572ba4a2548645ab8ca8f52825512133f080dedf6d6a33467a",
}


def owned(path):
    path = Path(path)
    if not path.resolve().is_relative_to(ROOT):
        raise SystemExit(f"refusing a path outside {ROOT}: {path}")
    return path


def sha256(path):
    import hashlib
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def running_games():
    out = subprocess.run(["pgrep", "-x", "hwder"], capture_output=True, text=True).stdout.split()
    return [int(p) for p in out]


def prepare(args, run):
    fixture = owned(RUNS / "fixtures" / args.fixture / SAVE_LEAF)
    if sha256(fixture / "zmha.bin") != FIXTURES[args.fixture]:
        raise SystemExit(f"fixture {args.fixture} changed; refusing")
    run.mkdir(parents=True)
    os.link(PLAY / "hwder", run / "hwder")  # exe_dir() is the run folder: settings, saves and caches stay here
    for name in ("pipeline_cache.bin", "pipeline_warmup.bin"):
        if (PLAY / name).exists():
            shutil.copy2(PLAY / name, run / name)
    settings = (PLAY / "hwder_settings.ini").read_text().splitlines()
    forced = {"save_dir": "", "save_copy_from": "", "debug_env": "", "skip_prelaunch": "1", "show_fps": "0"}
    lines = [l for l in settings if l.partition("=")[0] not in forced] + [f"{k}={v}" for k, v in forced.items()]
    (run / "hwder_settings.ini").write_text("\n".join(lines) + "\n")
    shutil.copytree(fixture, run / "save" / SAVE_LEAF)
    for d in ("home", "tmp"):
        (run / d).mkdir()
    (RUNS / "mesa-cache").mkdir(exist_ok=True)  # shared, so later runs start with compiled shaders


def environment(args, run):
    env = {k: v for k, v in os.environ.items() if not k.startswith("HWDER_") and k != "LD_PRELOAD"}
    env.update(HOME=str(run / "home"), TMPDIR=str(run / "tmp"), XDG_CACHE_HOME=str(run / "home/.cache"),
               XDG_CONFIG_HOME=str(run / "home/.config"), XDG_DATA_HOME=str(run / "home/.local/share"),
               MESA_SHADER_CACHE_DIR=str(RUNS / "mesa-cache"))
    flags = {"HWDER_NSP": str(ROOT / "Hyrule Warriors Definitive Edition.nsp"), "HWDER_KEYS": str(ROOT / "prod.keys"),
             "HWDER_LOG": str(run / "hwder.log"), "HWDER_MUTE": "1", "HWDER_NO_PRELAUNCH": "1", "HWDER_NO_DUMP": "1",
             "HWDER_AUTOPLAY": "2", "HWDER_GAME_INTERP": "1" if args.mode == 120 else "0", "HWDER_GI_LOG": "1"}
    if args.fixture == "busy-spot":
        flags["HWDER_AUTOPLAY_IDLE"] = "1"  # mid-battle save: stand still so every run sees the same scene
    if args.stats:
        flags["HWDER_THREAD_STATS"] = str(args.stats)
    if args.filmstrip:
        flags.update(HWDER_FILMSTRIP=str(run / "filmstrip"), HWDER_FILMSTRIP_BATTLE="10", HWDER_FILMSTRIP_FOR="3")
        (run / "filmstrip").mkdir()
    for kv in args.env:
        k, _, v = kv.partition("=")
        flags[k] = v
    env.update(flags)
    return env, flags


def stop(run, deadline):
    exe = str(run / "hwder")
    pid = None
    while time.time() < deadline:
        for p in running_games():
            try:
                if os.readlink(f"/proc/{p}/exe") == exe:
                    pid = p
            except OSError:
                pass
        time.sleep(1)
    if pid is None:
        return "game was not running at the deadline"
    os.kill(pid, signal.SIGTERM)
    for _ in range(10):
        time.sleep(0.5)
        if not os.path.exists(f"/proc/{pid}"):
            return "stopped with SIGTERM"
    os.kill(pid, signal.SIGKILL)
    return "stopped with SIGKILL"


def summarize(run, args, flags, outcome):
    log = (run / "hwder.log").read_text(errors="replace") if (run / "hwder.log").exists() else ""
    frames = [l for l in log.splitlines() if "display: 300 frames" in l]
    gi = [l for l in log.splitlines() if l.split("] ", 1)[-1].startswith("gi: ")]
    bad = [l for l in log.splitlines() if re.search(r"fatal|TERMINATE|ABORT|signal|device lost|VK_ERROR", l)]
    perf = [l for l in log.splitlines() if "perf:" in l]
    avgs = [float(m.group(1)) for l in frames if (m := re.search(r"avg ([\d.]+) ms", l))]
    worst = [float(m.group(1)) for l in frames if (m := re.search(r"worst ([\d.]+) ms", l))]
    lines = [f"run {run.name}: fixture {args.fixture}, mode {args.mode}, {args.seconds} s, {outcome}",
             f"flags: {' '.join(f'{k}={v}' for k, v in flags.items() if k not in ('HWDER_NSP', 'HWDER_KEYS'))}"]
    if avgs:
        steady = avgs[2:] or avgs  # the first windows include loading
        lines.append(f"300-frame windows: {len(avgs)}; steady avg {min(steady):.2f}-{max(steady):.2f} ms "
                     f"({1000 / max(steady):.0f}-{1000 / min(steady):.0f} fps); worst frame {max(worst):.1f} ms")
    lines += ["", "frame windows:"] + frames + ["", "120 fps mode:"] + gi[-8:] + ["", "errors:"] + bad[:20]
    if perf:
        lines += ["", "thread stats (last report):"]
        last = max(i for i, l in enumerate(perf) if "ms window" in l)
        lines += perf[last:last + 17]
    (run / "summary.txt").write_text("\n".join(lines) + "\n")
    print("\n".join(lines[:3]))


def png(path, width, height, rgb):
    raw = b"".join(b"\x00" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def contact_sheet(run, columns=8, rows=6):
    strip = run / "filmstrip"
    shots = sorted(strip.glob("f_*.raw"), key=lambda p: int(re.sub(r"\D", "", p.stem) or 0))
    if not shots:
        return
    pick = [shots[i * len(shots) // (columns * rows)] for i in range(min(columns * rows, len(shots)))]
    w, h = 96, 54
    sheet = bytearray(columns * w * rows * h * 3)
    for n, shot in enumerate(pick):
        data = shot.read_bytes()
        if len(data) < w * h * 4:
            continue
        ox, oy = (n % columns) * w, (n // columns) * h
        for y in range(h):
            for x in range(w):
                s = (y * w + x) * 4
                d = ((oy + y) * columns * w + ox + x) * 3
                sheet[d:d + 3] = data[s:s + 3]
    png(run / "filmstrip.png", columns * w, rows * h, bytes(sheet))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fixture", choices=sorted(FIXTURES), default="autoplay")
    ap.add_argument("--mode", type=int, choices=(60, 120), default=120)
    ap.add_argument("--seconds", type=int, default=90)
    ap.add_argument("--name", required=True)
    ap.add_argument("--stats", type=int, default=0, help="HWDER_THREAD_STATS period in ms (costs ~10%% of the GPU thread)")
    ap.add_argument("--filmstrip", action="store_true")
    ap.add_argument("--visible", action="store_true", help="use the current display instead of a headless gamescope")
    ap.add_argument("--size", default="1920x1080")
    ap.add_argument("--env", action="append", default=[], help="extra NAME=VALUE for the game")
    args = ap.parse_args()
    if running_games():
        raise SystemExit("hwder is already running; not starting a test")
    run = owned(RUNS / f"{datetime.datetime.now():%Y%m%d-%H%M%S}-{args.name}")
    prepare(args, run)
    env, flags = environment(args, run)
    game = ["/usr/bin/distrobox", "enter", "-n", "hwder-dev", "--", "env", "-u", "LD_PRELOAD", str(run / "hwder")]
    if not args.visible:
        w, h = args.size.split("x")
        game = ["/usr/bin/gamescope", "--backend", "headless", "-W", w, "-H", h, "-w", w, "-h", h, "-r", "120", "--"] + game
    with open(run / "console.log", "wb") as console:
        proc = subprocess.Popen(game, cwd=run, env=env, stdout=console, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                start_new_session=True)
        outcome = stop(run, time.time() + args.seconds)
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGTERM)
            proc.wait(timeout=10)
    if sha256(run / "save" / SAVE_LEAF / "zmha.bin") != FIXTURES[args.fixture]:
        outcome += "; the run wrote its save copy"
    summarize(run, args, flags, outcome)
    if args.filmstrip:
        contact_sheet(run)
    os.remove(run / "hwder")  # the hard link only; the play build is unchanged
    return 0


if __name__ == "__main__":
    sys.exit(main())
