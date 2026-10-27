"""Unattended autoplay run of the Linux build on the Ally (Bazzite), run on the host, outside the container.

Each run gets its own folder under <root>/runs/autoplay/: a hard link of the play build's hwder, copies of its
settings and pipeline caches, and a copy of a fixed save. The play folder, its save and the fixtures are never
written. The game runs natively (the release build needs no container; --container NAME runs an old dev build in
its distrobox), by default inside a headless gamescope (an invisible virtual display), and is stopped after
--seconds. Root: --root or $HWDER_TEST_ROOT (fixtures in <root>/runs/autoplay/fixtures); play build: --play, by
default the installed release (~/.local/share/HWDER/build/play); game and keys: --nsp/--keys, else the play
settings' nsp_path/keys_path. A summary of the frame-time, 120 fps and thread lines of
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

ROOT = Path(os.environ.get("HWDER_TEST_ROOT", "/var/home/bazzite/Documents/HWDER-test"))
PLAY = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share") / "HWDER/build/play"
RUNS = ROOT / "runs/autoplay"
GAME_FILES = {}  # HWDER_NSP / HWDER_KEYS for the run
SAVE_LEAF = Path("0100AE00096EA000/48574445525F55530000000000000001")
FIXTURES = {  # fixed saves copied from hywar/test-saves (sha256 of zmha.bin)
    "autoplay": "9038321106d28444dc26864749274c8403ef3222b741008f60349a21dc07bfbe",
    "busy-spot": "38e73f2680b232572ba4a2548645ab8ca8f52825512133f080dedf6d6a33467a",
    "heavy-fight": "8eec90e953a32ecc9ce08ae1afd8f654ae9bff94877f3797599b8a76f464cba5",
}


def owned(path):
    path = Path(path)
    if not path.resolve().is_relative_to(ROOT) and not path.resolve().is_relative_to(PLAY):
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
    exe = owned(Path(args.exe)) if args.exe else PLAY / "hwder"
    os.link(exe, run / "hwder")  # exe_dir() is the run folder: settings, saves and caches stay here
    caches = owned(Path(args.caches_from)) if args.caches_from else PLAY
    for name in ("pipeline_cache.bin", "pipeline_warmup.bin"):
        if (caches / name).exists():
            shutil.copy2(caches / name, run / name)
    settings = (PLAY / "hwder_settings.ini").read_text().splitlines()
    # save_dir: the run's own copy of the fixture, so the default (~/.local/share/HWDER/save) is never used
    forced = {"save_dir": str(run / "save"), "save_copy_from": "", "debug_env": "", "skip_prelaunch": "1", "show_fps": "0"}
    lines = [l for l in settings if l.partition("=")[0] not in forced] + [f"{k}={v}" for k, v in forced.items()]
    (run / "hwder_settings.ini").write_text("\n".join(lines) + "\n")
    shutil.copytree(fixture, run / "save" / SAVE_LEAF)
    for d in ("home", "tmp"):
        (run / d).mkdir()
    (RUNS / "mesa-cache").mkdir(exist_ok=True)  # shared, so later runs start with compiled shaders


def environment(args, run):
    # The launcher (gamescope, distrobox/podman) keeps the real HOME; only the game's own files are redirected.
    env = {k: v for k, v in os.environ.items() if not k.startswith("HWDER_") and k != "LD_PRELOAD"}
    env.setdefault("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}")  # unset in SSH sessions; gamescope needs it
    game = {"HOME": str(run / "home"), "TMPDIR": str(run / "tmp"), "XDG_CACHE_HOME": str(run / "home/.cache"),
            "XDG_CONFIG_HOME": str(run / "home/.config"), "XDG_DATA_HOME": str(run / "home/.local/share"),
            "MESA_SHADER_CACHE_DIR": str(RUNS / "mesa-cache")}
    flags = {**GAME_FILES,
             "HWDER_LOG": str(run / "hwder.log"), "HWDER_MUTE": "1", "HWDER_NO_PRELAUNCH": "1", "HWDER_NO_DUMP": "1",
             "HWDER_AUTOPLAY": "2", "HWDER_GAME_INTERP": "1" if args.mode == 120 else "0", "HWDER_GI_LOG": "1"}
    if args.fixture == "busy-spot":
        flags["HWDER_AUTOPLAY_IDLE"] = "1"  # mid-battle save: stand still so every run sees the same scene
    if args.fixture == "heavy-fight":  # mid-battle save; X once starts the special attack with the heavy frames
        flags["HWDER_AUTOPLAY_X"] = "1"
        if not args.fight:  # standing still: the attack mostly keeps its camera; fighting first brings its zoom
            flags["HWDER_AUTOPLAY_IDLE"] = "1"
    if args.stats:
        flags["HWDER_THREAD_STATS"] = str(args.stats)
    if args.filmstrip:
        flags.update(HWDER_FILMSTRIP=str(run / "filmstrip"), HWDER_FILMSTRIP_BATTLE="10", HWDER_FILMSTRIP_FOR="3")
        (run / "filmstrip").mkdir()
    for kv in args.env:
        k, _, v = kv.partition("=")
        flags[k] = v
    game.update(flags)
    return env, game, flags


def stop(proc, run, deadline):
    exe = str(run / "hwder")
    pid = None
    while time.time() < deadline and proc.poll() is None:
        for p in running_games():
            try:
                if os.readlink(f"/proc/{p}/exe") == exe:
                    pid = p
            except OSError:
                pass
        time.sleep(1)
    if proc.poll() is not None and not (pid and os.path.exists(f"/proc/{pid}")):
        return f"exited on its own (launcher exit {proc.returncode}) before the deadline"
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
    lines = [f"run {run.name}: fixture {args.fixture}, mode {args.mode}, {args.seconds} s, {outcome}, exe {args.exe or 'play build'}",
             f"flags: {' '.join(f'{k}={v}' for k, v in flags.items() if k not in ('HWDER_NSP', 'HWDER_KEYS'))}"]
    if avgs:
        # Battle windows: in 120 mode from the first 120 Hz window on (menus run at 60); the first two include loading.
        first = next((i for i, a in enumerate(avgs) if a < 12.0), len(avgs)) if args.mode == 120 else 2
        start = min(first + 1, len(avgs) - 1)  # the first battle window still contains the load
        steady, worst_battle = avgs[start:], worst[start:]
        lines.append(f"300-frame windows: {len(avgs)}; battle avg {min(steady):.2f}-{max(steady):.2f} ms "
                     f"({1000 / max(steady):.0f}-{1000 / min(steady):.0f} fps); worst battle frame {max(worst_battle):.1f} ms; "
                     f"worst frame overall {max(worst):.1f} ms")
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
    global ROOT, PLAY, RUNS
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
    ap.add_argument("--exe", help="another build of hwder inside the folder (default: the play build)")
    ap.add_argument("--fight", action="store_true", help="heavy-fight: run and attack instead of standing still")
    ap.add_argument("--caches-from", help="folder inside the project to copy the pipeline caches from (default: the play build's)")
    ap.add_argument("--root", type=Path, help="test folder with runs/autoplay/fixtures (default: $HWDER_TEST_ROOT or %s)" % ROOT)
    ap.add_argument("--play", type=Path, help="play build folder (default: %s)" % PLAY)
    ap.add_argument("--nsp", help="game .nsp (default: the play settings' nsp_path)")
    ap.add_argument("--keys", help="prod.keys (default: the play settings' keys_path)")
    ap.add_argument("--container", help="run inside this distrobox (old dev builds); default: natively")
    args = ap.parse_args()
    ROOT = (args.root or ROOT).resolve()
    PLAY = (args.play or PLAY).resolve()
    RUNS = ROOT / "runs/autoplay"
    settings = dict(l.partition("=")[::2] for l in (PLAY / "hwder_settings.ini").read_text().splitlines() if "=" in l)
    GAME_FILES.update(HWDER_NSP=args.nsp or settings.get("nsp_path", ""), HWDER_KEYS=args.keys or settings.get("keys_path", ""))
    if not all(Path(v).is_file() for v in GAME_FILES.values()):
        raise SystemExit("name the game and keys with --nsp/--keys (the play settings have none)")
    if running_games():
        raise SystemExit("hwder is already running; not starting a test")
    run = owned(RUNS / f"{datetime.datetime.now():%Y%m%d-%H%M%S}-{args.name}")
    prepare(args, run)
    env, game_env, flags = environment(args, run)
    if args.container:
        listed = subprocess.run(["/usr/bin/distrobox", "list", "--no-color"], capture_output=True, text=True, env=env).stdout
        if not re.search(r"\|\s*%s\s*\|" % re.escape(args.container), listed):
            raise SystemExit(f"the {args.container} container is missing; not creating one")
        game = (["/usr/bin/distrobox", "enter", "-n", args.container, "--", "env", "-u", "LD_PRELOAD"]
                + [f"{k}={v}" for k, v in game_env.items()] + [str(run / "hwder")])
    else:
        game = ["/usr/bin/env", "-u", "LD_PRELOAD"] + [f"{k}={v}" for k, v in game_env.items()] + [str(run / "hwder")]
    if not args.visible:
        w, h = args.size.split("x")
        game = ["/usr/bin/gamescope", "--backend", "headless", "-W", w, "-H", h, "-w", w, "-h", h, "-r", "120", "--"] + game
    with open(run / "console.log", "wb") as console:
        proc = subprocess.Popen(game, cwd=run, env=env, stdout=console, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                start_new_session=True)
        outcome = stop(proc, run, time.time() + args.seconds)
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
