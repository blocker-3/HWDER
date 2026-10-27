#!/usr/bin/env python3
"""Add HWDER to Steam as a non-Steam game on Linux (userdata/<account>/config/shortcuts.vdf).

The byte functions are a port of tools/setup_ui/steam_shortcut.h (the Windows Setup's Add to Steam): same entry
name, same appid, an earlier entry for the same exe is updated instead of duplicated, and every other entry is
written back byte for byte. Steam keeps the list in memory and writes it back when it closes, so a running Steam
is closed first (`steam -shutdown`) and started again afterwards. The previous file is kept as
shortcuts.vdf.bak-<timestamp>.

    python3 tools/steam_shortcut.py --add           # after a successful tools/first_run.py build

shortcuts.vdf is Valve's binary KeyValues: 0x00 name\\0 {children} 0x08 is a map, 0x01 name\\0 value\\0 a string,
0x02 name\\0 <int32 LE> an integer (0x07: a 64-bit one). The file is one map "shortcuts" whose children are maps
named "0", "1", ..., then a final 0x08.
"""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request

STEAM_NAME = "Hyrule Warriors: Definitive Edition"
LAUNCHER = "hwder-steam.sh"
WINDOWS = os.name == "nt"  # Setup's Add to Steam button covers Windows
HEAD = b"\x00shortcuts\x00"

# Added results, as steam::Added.
YES, UPDATED, ALREADY_THERE, UNREADABLE = "yes", "updated", "already-there", "unreadable"


def shortcut_appid(quoted_exe, name):
    """Steam's id for a non-Steam shortcut: CRC-32 of the quoted exe plus the name, top bit set."""
    data = (quoted_exe + name).encode("utf-8") if isinstance(quoted_exe, str) else quoted_exe + name
    crc = 0xFFFFFFFF
    for c in data:
        crc ^= c
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1))
    return (~crc & 0xFFFFFFFF) | 0x80000000


class Node:
    __slots__ = ("type", "key", "value", "kids")

    def __init__(self, type_=0, key=b"", value=b""):
        self.type, self.key, self.value, self.kids = type_, key, value, []


def _parse_map(b, at, node, depth):
    """Reads the children of a map up to and including its closing 0x08; returns the new offset or None."""
    if depth > 16:
        return None
    while at < len(b):
        n = Node(b[at])
        at += 1
        if n.type == 0x08:
            return at
        end = b.find(b"\x00", at)
        if end < 0:
            return None
        n.key, at = b[at:end], end + 1
        if n.type == 0x00:
            at = _parse_map(b, at, n, depth + 1)
            if at is None:
                return None
        elif n.type == 0x01:
            end = b.find(b"\x00", at)
            if end < 0:
                return None
            n.value, at = b[at:end], end + 1
        elif n.type in (0x02, 0x07):
            size = 4 if n.type == 0x02 else 8
            if at + size > len(b):
                return None
            n.value, at = b[at:at + size], at + size
        else:
            return None
        node.kids.append(n)
    return None


def _write(out, n):
    out += bytes((n.type,)) + n.key + b"\x00"
    if n.type == 0x00:
        for kid in n.kids:
            _write(out, kid)
        out += b"\x08"
    elif n.type == 0x01:
        out += n.value + b"\x00"
    else:
        out += n.value


def _unquoted(s):
    return s[1:-1] if len(s) >= 2 and s[:1] == b'"' and s[-1:] == b'"' else s


def _str(key, value):
    return b"\x01" + key + b"\x00" + value + b"\x00"


def _num(key, value):
    return b"\x02" + key + b"\x00" + struct.pack("<I", value)


def add(existing, name, exe, start_dir, icon=""):
    """`existing` is the current file (b"" when there is none). Returns (result, new file or None, appid or None).
    exe and start_dir are unquoted; the appid names the entry's artwork in config/grid."""
    name, exe, start_dir, icon = (v.encode("utf-8") for v in (name, exe, start_dir, icon))
    base = existing or HEAD + b"\x08\x08"
    if not base.startswith(HEAD):
        return UNREADABLE, None, None
    shortcuts = Node()
    at = _parse_map(base, len(HEAD), shortcuts, 0)
    if at is None or at + 1 != len(base) or base[at] != 0x08:
        return UNREADABLE, None, None
    quoted = b'"' + exe + b'"'
    following = 0
    for kid in shortcuts.kids:
        if kid.type != 0x00:
            continue
        if kid.key.isdigit():
            following = max(following, int(kid.key) + 1)
        field = None
        for f in kid.kids:
            if f.type == 0x01 and f.key.lower() == b"exe" and _unquoted(f.value).lower() == exe.lower():
                field = f
        if field is None:
            continue
        # Steam derives the id from the Exe value exactly as stored.
        appid = shortcut_appid(field.value, name)
        # Bring an earlier HWDER entry up to date (name, the id its artwork files are named by, icon);
        # every other field and every other entry is written back unchanged.
        changed = False
        for key, type_, value in ((b"AppName", 0x01, name), (b"appid", 0x02, struct.pack("<I", appid)),
                                  (b"icon", 0x01, icon)):
            if key == b"icon" and not icon:
                continue
            for f in kid.kids:
                if f.type == type_ and f.key.lower() == key.lower():
                    changed = changed or f.value != value
                    f.value = value
                    break
        if not changed:
            return ALREADY_THERE, None, appid
        out = bytearray(HEAD)
        for entry in shortcuts.kids:
            _write(out, entry)
        out += b"\x08\x08"
        return UPDATED, bytes(out), appid
    appid = shortcut_appid(quoted, name)
    e = (b"\x00" + str(following).encode() + b"\x00" + _num(b"appid", appid) + _str(b"AppName", name)
         + _str(b"Exe", quoted) + _str(b"StartDir", b'"' + start_dir + b'"') + _str(b"icon", icon)
         + _str(b"ShortcutPath", b"") + _str(b"LaunchOptions", b"") + _num(b"IsHidden", 0)
         + _num(b"AllowDesktopConfig", 1) + _num(b"AllowOverlay", 1) + _num(b"OpenVR", 0) + _num(b"Devkit", 0)
         + _str(b"DevkitGameID", b"") + _num(b"DevkitOverrideAppID", 0) + _num(b"LastPlayTime", 0)
         + _str(b"FlatpakAppID", b"") + b"\x00tags\x00\x08" + b"\x08")
    # base ends with the list's closing 0x08 and the file's final 0x08: insert before both.
    return YES, base[:-2] + e + b"\x08\x08", appid


def find_entry(existing, exe):
    """True when the file already has an entry for this exe (unquoted, compared as Steam setup does)."""
    result, _, _ = add(existing, STEAM_NAME, exe, str(Path(exe).parent))
    return result in (ALREADY_THERE, UPDATED)


def entry_present(exe, home=None):
    """True when Steam is not installed or already has the HWDER entry for this game's launcher; first_run then
    does not ask again."""
    roots = steam_roots(home)
    if not roots:
        return True
    account = steam_account(roots[0][0])
    try:
        existing = (roots[0][0] / "userdata" / account / "config" / "shortcuts.vdf").read_bytes() if account else b""
    except OSError:
        existing = b""
    return find_entry(existing, str(Path(exe).resolve().parent / LAUNCHER))


# ---- Steam folder, account and process --------------------------------------------------------------------------

def steam_roots(home=None):
    """Existing Steam folders with userdata, native first, then Flatpak; ~/.steam/steam is usually a link to
    ~/.local/share/Steam and is listed once."""
    home = Path(home or Path.home())
    found = []
    for path, flatpak in ((home / ".local/share/Steam", False), (home / ".steam/steam", False),
                          (home / ".steam/root", False),
                          (home / ".var/app/com.valvesoftware.Steam/.local/share/Steam", True),
                          (home / ".var/app/com.valvesoftware.Steam/data/Steam", True)):
        try:
            real = path.resolve()
            if (path / "userdata").is_dir() and all(real != r.resolve() for r, _ in found):
                found.append((path, flatpak))
        except OSError:
            continue
    return found


def steam_account(root):
    """The Steam account to add to: the most recent login in config/loginusers.vdf, else the only userdata
    account (as Setup's steamAccount on Windows)."""
    root = Path(root)
    try:
        text = (root / "config" / "loginusers.vdf").read_text(encoding="utf-8", errors="replace")
    except OSError:
        text = ""
    tokens = text.split('"')[1::2]
    steam_id = ""
    for i in range(len(tokens) - 1):
        if len(tokens[i]) == 17 and tokens[i].isdigit():
            steam_id = tokens[i]
        elif steam_id and tokens[i].lower() == "mostrecent" and tokens[i + 1] == "1":
            account = str(int(steam_id) - 76561197960265728)
            if (root / "userdata" / account).is_dir():
                return account
    try:
        accounts = [p.name for p in (root / "userdata").iterdir()
                    if p.is_dir() and p.name != "0" and p.name.isdigit()]
    except OSError:
        accounts = []
    return accounts[0] if len(accounts) == 1 else ""


def steam_processes(proc="/proc"):
    """PIDs of running Steam client processes (comm "steam"), with their command lines."""
    found = {}
    try:
        entries = os.listdir(proc)
    except OSError:
        return found
    for pid in entries:
        if not pid.isdigit():
            continue
        try:
            if Path(proc, pid, "comm").read_text().strip() != "steam":
                continue
            found[int(pid)] = Path(proc, pid, "cmdline").read_bytes().split(b"\x00")
        except OSError:
            continue
    return found


def game_mode(processes, environ=None):
    """Bazzite / SteamOS Game Mode: Steam runs fullscreen in gamescope (-gamepadui) and its session restarts it
    when it exits."""
    environ = os.environ if environ is None else environ
    if environ.get("XDG_CURRENT_DESKTOP", "").lower() == "gamescope" or environ.get("GAMESCOPE_WAYLAND_DISPLAY"):
        return True
    return any(b"-gamepadui" in args or b"-steamos3" in args for args in processes.values())


def container_name(environ=None):
    """The distrobox this runs in (its build only runs inside the same container), or ""."""
    environ = os.environ if environ is None else environ
    if environ.get("CONTAINER_ID") and (Path("/run/.containerenv").exists() or Path("/.dockerenv").exists()):
        return environ["CONTAINER_ID"]
    return ""


def steam_command(flatpak, container):
    command = ["flatpak", "run", "com.valvesoftware.Steam"] if flatpak else ["steam"]
    return (["distrobox-host-exec"] + command) if container else command


def close_steam(flatpak, container, timeout=30.0, run=subprocess.run, processes=steam_processes, sleep=time.sleep):
    """Asks Steam to exit (steam -shutdown) and waits up to `timeout` seconds."""
    try:
        run(steam_command(flatpak, container) + ["-shutdown"], stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=timeout)
    except (OSError, subprocess.SubprocessError):
        return False
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not processes():
            return True
        sleep(0.25)
    return not processes()


def start_steam(flatpak, container):
    try:
        subprocess.Popen(steam_command(flatpak, container), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, start_new_session=True)
        return True
    except OSError:
        return False


# ---- What the shortcut starts -----------------------------------------------------------------------------------

def write_launcher(exe, container=""):
    """A small script next to the game that Steam starts: it runs the game from its own folder (settings, saves and
    caches are found relative to it) and, for a build made inside a distrobox, inside that same container so the
    shared libraries it was built against are there. Returns its path."""
    exe = Path(exe).resolve()
    script = exe.parent / LAUNCHER
    run = "./" + shlex.quote(exe.name)
    if container:
        run = "distrobox-enter -n " + shlex.quote(container) + " -- " + shlex.quote(str(exe))
    text = ("#!/bin/sh\n"
            "# Written by tools/steam_shortcut.py; Steam's \"" + STEAM_NAME + "\" entry starts this.\n"
            "cd \"$(dirname -- \"$0\")\" || exit 1\n"
            "exec " + run + " \"$@\"\n")
    if not script.is_file() or script.read_text(encoding="utf-8", errors="replace") != text:
        temporary = script.with_name("." + LAUNCHER + ".tmp")
        temporary.write_text(text, encoding="utf-8", newline="\n")
        os.chmod(temporary, 0o755)
        os.replace(temporary, script)
    return script


def installed_game(cache):
    """The published play executable of a successful first-run build in `cache`, and its recorded inputs."""
    import first_run
    cache = Path(cache)
    exe = cache / "play" / "hwder"
    marker = first_run.read_json(cache / "last-success.json")
    if not exe.is_file() or not marker.get("exe_sha256") or first_run.file_hash(exe) != marker["exe_sha256"]:
        return None
    return exe


def store_game_paths(cache, exe, nsp, keys):
    """Steam starts the game without HWDER_NSP/HWDER_KEYS, so its settings file has to name them (as the Windows
    Setup does in setup_bridge.installed_paths). Only nsp_path and keys_path change; every other line, including a
    hand-set update_path or save_dir, is kept byte for byte."""
    import first_run
    values = {"nsp_path": str(Path(nsp).resolve()), "keys_path": str(Path(keys).resolve())}
    for key, value in values.items():
        if any(c in value for c in "\r\n\0") or len((key + "=" + value).encode("utf-8")) >= 2046:
            raise ValueError("The game or keys path cannot be stored in the game's settings. Use a shorter path.")
    first_run.atomic_json(Path(cache) / "setup-inputs.json", {"nsp": values["nsp_path"], "keys": values["keys_path"]})
    settings = Path(exe).parent / "hwder_settings.ini"
    # A new file carries the game's first-run defaults (see setup_bridge.installed_paths).
    original = settings.read_bytes() if settings.exists() else b"version=2\nface_labels=1\n"
    result, seen = [], set()
    for line in original.splitlines(keepends=True):
        key = line.split(b"=", 1)[0].decode("utf-8", errors="replace")
        if key in values:
            if key not in seen:
                result.append((key + "=" + values[key]).encode("utf-8") + (b"\r\n" if line.endswith(b"\r\n") else b"\n"))
                seen.add(key)
        else:
            result.append(line)
    if result and not result[-1].endswith((b"\r", b"\n")):
        result.append(b"\n")
    result.extend((key + "=" + value + "\n").encode("utf-8") for key, value in values.items() if key not in seen)
    content = b"".join(result)
    if content != original or not settings.exists():
        temporary = settings.with_name(".steam-settings.tmp")
        try:
            temporary.write_bytes(content)
            os.replace(temporary, settings)
        finally:
            temporary.unlink(missing_ok=True)


# ---- Artwork (as Setup's nintendoArtwork / steamGridArtwork) ----------------------------------------------------

NINTENDO_ART = ("/store/software/switch/70010000006023/"
                "4b035b357b10e59dce73609dd071e61d131d90b2e477c29088f26fbe50b44d1b")
NINTENDO_CDN = "https://assets.nintendo.com/image/upload/"


def _get(url, key=""):
    request = urllib.request.Request(url, headers={"User-Agent": "HWDER-Setup",
                                                   **({"Authorization": "Bearer " + key} if key else {})})
    try:
        with urllib.request.urlopen(request, timeout=20) as reply:
            return reply.status, reply.read(32 << 20)
    except urllib.error.HTTPError as e:
        return e.code, b""
    except (OSError, ValueError):
        return 0, b""


def _art_present(grid, base, suffix):
    for ext in (".png", ".jpg"):
        if (grid / (base + suffix + ext)).is_file():
            return grid / (base + suffix + ext)
    return None


def _art_save(grid, base, suffix, url, counts, key=""):
    if _art_present(grid, base, suffix):
        counts["kept"] += 1
        return
    status, picture = _get(url, key)
    png, jpg = picture.startswith(b"\x89PNG"), picture.startswith(b"\xff\xd8\xff")
    if status != 200 or len(picture) < 64 or not (png or jpg):
        return
    grid.mkdir(parents=True, exist_ok=True)
    (grid / (base + suffix + (".png" if png else ".jpg"))).write_bytes(picture)
    counts["saved"] += 1


def download_artwork(grid, appid, steamgriddb_key=""):
    """Steam's names for a shortcut's pictures in config/grid: <id>p (portrait), <id> (wide), <id>_hero, <id>_logo,
    <id>_icon. Pictures already there are kept. Returns (note, icon path or None)."""
    import json
    grid, base, counts, problem = Path(grid), str(appid), {"saved": 0, "kept": 0}, ""
    if steamgriddb_key:
        api = "https://www.steamgriddb.com/api/v2/"
        status, found = _get(api + "search/autocomplete/Hyrule%20Warriors%3A%20Definitive%20Edition", steamgriddb_key)
        try:
            game = json.loads(found)["data"][0]["id"] if status == 200 else None
        except (ValueError, KeyError, IndexError, TypeError):
            game = None
        if status in (401, 403):
            problem = " SteamGridDB did not accept the API key."
        elif game is None:
            problem = " SteamGridDB could not be reached."
        else:
            for suffix, query in (("p", "grids/game/%s?dimensions=600x900"),
                                  ("", "grids/game/%s?dimensions=920x430,460x215"), ("_hero", "heroes/game/%s?"),
                                  ("_logo", "logos/game/%s?"), ("_icon", "icons/game/%s?")):
                if _art_present(grid, base, suffix):
                    continue
                path = query % game
                path += ("" if path.endswith("?") else "&") + "types=static&mimes=image/png,image/jpeg"
                status, reply = _get(api + path, steamgriddb_key)
                try:
                    url = json.loads(reply)["data"][0]["url"] if status == 200 else ""
                except (ValueError, KeyError, IndexError, TypeError):
                    url = ""
                if url:
                    _art_save(grid, base, suffix, url, counts)
    for suffix, crop in (("p", "c_fill,g_auto,w_600,h_900/f_jpg/q_auto"), ("", "c_fill,w_920,h_430/f_jpg/q_auto"),
                         ("_hero", "c_fill,g_north,w_1920,h_620/f_jpg/q_auto"), ("_icon", "c_fill,g_auto,w_256,h_256/f_png")):
        _art_save(grid, base, suffix, NINTENDO_CDN + crop + NINTENDO_ART, counts)
    if _art_present(grid, base, "_logo"):
        counts["kept"] += 1
    note = (" Box art added." if counts["saved"] else " Your existing artwork was kept." if counts["kept"]
            else " Box art could not be downloaded; check the internet connection.")
    icon = _art_present(grid, base, "_icon")
    return note + problem, icon


# ---- Adding -----------------------------------------------------------------------------------------------------

def write_shortcuts(file, existing, content, stamp=None):
    """Backs up the current file to shortcuts.vdf.bak-<timestamp>, then replaces it atomically."""
    file = Path(file)
    file.parent.mkdir(parents=True, exist_ok=True)
    backup = None
    if existing:
        backup = file.with_name(file.name + ".bak-" + (stamp or time.strftime("%Y%m%d-%H%M%S")))
        shutil.copy2(file, backup)
        if backup.read_bytes() != existing:
            raise OSError("Could not back up Steam's game list")
    temporary = file.with_name(file.name + ".hwder-new")
    try:
        temporary.write_bytes(content)
        os.replace(temporary, file)
    finally:
        temporary.unlink(missing_ok=True)
    return backup


def add_to_root(root, target, artwork=True, steamgriddb_key="", account=""):
    """Adds `target` (the launcher script) to the Steam folder `root`, which Steam must not be running on.
    Returns (ok, message)."""
    root = Path(root)
    account = account or steam_account(root)
    if not account:
        return False, ("Could not tell which Steam account to use. Sign in to Steam once, close it, then try again "
                       "(or name the account with --account <userdata folder number>).")
    file = root / "userdata" / account / "config" / "shortcuts.vdf"
    try:
        existing = file.read_bytes() if file.exists() else b""
    except OSError:
        return False, "Steam's game list could not be read, so it was left unchanged."
    target = Path(target)
    result, content, appid = add(existing, STEAM_NAME, str(target), str(target.parent))
    if result == UNREADABLE:
        return False, "Steam's game list is in a format HWDER does not know, so it was left unchanged."
    note = ""
    if artwork:
        note, icon = download_artwork(file.parent / "grid", appid, steamgriddb_key)
        if icon:
            result, content, appid = add(existing, STEAM_NAME, str(target), str(target.parent), str(icon))
    if result == ALREADY_THERE:
        return True, "HWDER is already in your Steam library." + note
    try:
        backup = write_shortcuts(file, existing, content)
    except OSError:
        return False, "Could not write Steam's game list; it was left unchanged."
    kept = f" The previous list is kept as {backup.name}." if backup else ""
    return True, ("Your Steam entry is up to date." if result == UPDATED else "Added to Steam as a non-Steam game.") \
        + note + kept


def ask(question, default=False, answer=None):
    if answer is not None:
        return answer
    if not sys.stdin.isatty():
        return default
    try:
        reply = input(question + (" [Y/n] " if default else " [y/N] ")).strip().lower()
    except EOFError:
        return default
    return reply in ("y", "yes") if reply else default


def say(message):
    print(f"steam: {message}", flush=True)


def add_to_steam(exe, steam_dir=None, account="", assume_yes=None, artwork=True, steamgriddb_key="",
                 processes=steam_processes, close=close_steam, start=start_steam, environ=None):
    """Interactive Add to Steam for the play executable `exe`. assume_yes=True closes and restarts a running
    Steam without asking, False never closes it, None asks. Returns True when the entry is in place."""
    if WINDOWS:
        say("on Windows use Setup's Add to Steam button.")
        return False
    environ = os.environ if environ is None else environ
    if steam_dir:
        roots = [(Path(steam_dir), "com.valvesoftware.Steam" in str(steam_dir))]
    else:
        roots = steam_roots(environ.get("HOME"))
    if not roots:
        say("Steam was not found (~/.local/share/Steam, ~/.steam/steam or the Flatpak). "
            "Install Steam and sign in once, then run: python3 tools/steam_shortcut.py --add")
        return False
    root, flatpak = roots[0]
    if len(roots) > 1:
        say(f"several Steam folders found; using {root} (choose another with --steam-dir).")
    container = container_name(environ)
    running = processes()
    in_game_mode = bool(running) and game_mode(running, environ)
    if running:
        if in_game_mode:
            say("Steam is running in Game Mode. It keeps its game list in memory and overwrites shortcuts.vdf when "
                "it exits, so it has to close first. Steam will then restart on its own and HWDER appears in your "
                "library after that.")
        else:
            say("Steam is running. It keeps its game list in memory and overwrites shortcuts.vdf when it exits, "
                "so it has to close before HWDER can be added; it is started again afterwards.")
        if not ask("Close Steam now?", answer=assume_yes):
            say("left unchanged. Exit Steam, then run: python3 tools/steam_shortcut.py --add")
            return False
        say("closing Steam...")
        if not close(flatpak, container):
            say("Steam did not close. Exit it (Steam menu, Exit), then run: python3 tools/steam_shortcut.py --add")
            return False
    target = write_launcher(exe, container)
    ok, message = add_to_root(root, target, artwork, steamgriddb_key, account)
    say(message)
    if running and not in_game_mode:
        if start(flatpak, container):
            say("Steam is starting again.")
        else:
            say("start Steam again yourself.")
    elif in_game_mode:
        say("Steam restarts by itself in Game Mode; HWDER shows up in your library once it is back.")
    return ok


def main(argv=None):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import first_run
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--add", action="store_true", help="add (or update) the HWDER entry in Steam")
    ap.add_argument("--cache", type=Path, default=first_run.default_cache(),
                    help="first-run cache with the built game (default: %(default)s)")
    ap.add_argument("--nsp", default=os.environ.get("HWDER_NSP"), help="your game .nsp (stored in the game's settings)")
    ap.add_argument("--keys", default=os.environ.get("HWDER_KEYS"), help="your prod.keys (stored in the game's settings)")
    ap.add_argument("--steam-dir", type=Path, help="Steam folder (default: found automatically)")
    ap.add_argument("--account", default="", help="userdata folder number (default: Steam's most recent login)")
    ap.add_argument("--yes", action="store_true", help="close and restart a running Steam without asking")
    ap.add_argument("--no-artwork", action="store_true", help="do not download box art")
    ap.add_argument("--steamgriddb-key", default="", help="your own SteamGridDB API key for community art")
    a = ap.parse_args(argv)
    if not a.add:
        ap.error("nothing to do; use --add")
    cache = a.cache.resolve()
    exe = installed_game(cache)
    if exe is None:
        say(f"no finished build in {cache}. Run play-local.sh (tools/first_run.py) first.")
        return 1
    inputs = first_run.read_json(cache / "setup-inputs.json")
    nsp, keys = a.nsp or inputs.get("nsp"), a.keys or inputs.get("keys")
    if not nsp or not keys or not Path(nsp).is_file() or not Path(keys).is_file():
        say("Steam starts the game without --nsp/--keys, so name them once: --nsp <game.nsp> --keys <prod.keys> "
            "(or set HWDER_NSP and HWDER_KEYS).")
        return 1
    try:
        store_game_paths(cache, exe, nsp, keys)
    except (OSError, ValueError) as e:
        say(f"could not store the game paths in its settings: {e}")
        return 1
    key = a.steamgriddb_key.strip()
    key_file = cache / "steamgriddb-key.txt"
    if key:
        key_file.write_text(key, encoding="utf-8")
    elif key_file.is_file():
        key = key_file.read_text(encoding="utf-8").strip()
    ok = add_to_steam(exe, a.steam_dir, a.account, True if a.yes else None, not a.no_artwork, key)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
