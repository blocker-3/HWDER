"""Linux Add to Steam (tools/steam_shortcut.py): synthetic bytes and folders only, never a real Steam install."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import steam_shortcut as steam

TEST_ROOT = Path(__file__).resolve().parents[2] / "build" / "setup-tests"
TEST_ROOT.mkdir(parents=True, exist_ok=True)
GAME = "/home/deck/.cache/hwder/local-build-v1/play/hwder-steam.sh"
PLAY = "/home/deck/.cache/hwder/local-build-v1/play"


def entry(index, name, exe, extra=b""):
    """A shortcut entry as Steam writes it (different field order and an extra 64-bit field than ours)."""
    return (b"\x00" + index + b"\x00" + b"\x02appid\x00" + struct.pack("<I", 0x81234567) + b"\x01AppName\x00" + name
            + b"\x00\x01Exe\x00" + exe + b"\x00\x01StartDir\x00\"/opt\"\x00\x07LastPlayTime\x00" + b"\x11" * 8
            + extra + b"\x00tags\x00\x010\x00Favorites\x00\x08\x08")


# Two entries Steam wrote itself, one with a non-ASCII name.
FIXTURE = (steam.HEAD + entry(b"0", "Ünïcode Game".encode(), b"\"/opt/game/run.sh\"")
           + entry(b"1", b"Emulator", b"\"/usr/bin/emu\"", b"\x01LaunchOptions\x00-f %command%\x00") + b"\x08\x08")


def parse(content):
    root = steam.Node()
    at = steam._parse_map(content, len(steam.HEAD), root, 0)
    assert at == len(content) - 1
    return root


def fields(node):
    return {f.key: f.value for f in node.kids}


class Bytes(unittest.TestCase):
    def test_round_trip_keeps_every_byte(self):
        out = bytearray(steam.HEAD)
        for kid in parse(FIXTURE).kids:
            steam._write(out, kid)
        self.assertEqual(bytes(out) + b"\x08\x08", FIXTURE)

    def test_add_keeps_existing_entries_byte_for_byte(self):
        result, content, appid = steam.add(FIXTURE, steam.STEAM_NAME, GAME, PLAY)
        self.assertEqual(result, steam.YES)
        self.assertEqual(content[:len(FIXTURE) - 2], FIXTURE[:-2])
        self.assertTrue(content.endswith(b"\x00tags\x00\x08\x08\x08\x08"))
        new = parse(content).kids[-1]
        self.assertEqual(new.key, b"2")
        f = fields(new)
        self.assertEqual(f[b"Exe"], b'"' + GAME.encode() + b'"')
        self.assertEqual(f[b"StartDir"], b'"' + PLAY.encode() + b'"')
        self.assertEqual(f[b"AppName"], steam.STEAM_NAME.encode())
        self.assertEqual(struct.unpack("<I", f[b"appid"])[0], appid)

    def test_adding_twice_gives_one_entry(self):
        _, first, _ = steam.add(FIXTURE, steam.STEAM_NAME, GAME, PLAY)
        result, again, _ = steam.add(first, steam.STEAM_NAME, GAME, PLAY)
        self.assertEqual((result, again), (steam.ALREADY_THERE, None))
        self.assertEqual(sum(fields(k).get(b"Exe") == b'"' + GAME.encode() + b'"' for k in parse(first).kids), 1)

    def test_earlier_entry_is_updated_not_duplicated(self):
        _, first, _ = steam.add(b"", "HWDER", GAME, PLAY)
        _, second, _ = steam.add(first, "Other", "/opt/other", "/opt")
        icon = "/home/deck/.local/share/Steam/userdata/1/config/grid/1_icon.png"
        result, updated, appid = steam.add(second, steam.STEAM_NAME, GAME, PLAY, icon)
        self.assertEqual(result, steam.UPDATED)
        kids = parse(updated).kids
        self.assertEqual(len(kids), 2)
        f = fields(kids[0])
        self.assertEqual((f[b"AppName"], f[b"icon"]), (steam.STEAM_NAME.encode(), icon.encode()))
        self.assertEqual(appid, steam.shortcut_appid('"' + GAME + '"', steam.STEAM_NAME))
        self.assertEqual(struct.unpack("<I", f[b"appid"])[0], appid)
        self.assertEqual(fields(kids[1]), fields(parse(second).kids[1]))
        self.assertEqual(steam.add(updated, steam.STEAM_NAME, GAME, PLAY, icon)[0], steam.ALREADY_THERE)

    def test_new_file_and_foreign_bytes(self):
        result, content, _ = steam.add(b"", steam.STEAM_NAME, GAME, PLAY)
        self.assertEqual(result, steam.YES)
        self.assertTrue(content.startswith(steam.HEAD + b"\x000\x00"))
        self.assertEqual(steam.add(b"not a vdf", steam.STEAM_NAME, GAME, PLAY)[0], steam.UNREADABLE)
        self.assertEqual(steam.add(content[:-5], steam.STEAM_NAME, GAME, PLAY)[0], steam.UNREADABLE)

    def test_appid_matches_steam_shortcut_h(self):
        # Known value from tools/tests/steam_shortcut_test.cpp.
        self.assertEqual(steam.shortcut_appid('"C:\\a.exe"', "A"), 0xFE27FA28)
        for exe, name in (('"' + GAME + '"', steam.STEAM_NAME), ('"/x"', "Ü")):
            self.assertEqual(steam.shortcut_appid(exe, name),
                             zlib.crc32((exe + name).encode()) | 0x80000000)
        # Hard-coded from the C++ CRC loop for the Linux launcher path (also checked by the compiled cross-check).
        self.assertEqual(steam.shortcut_appid('"' + GAME + '"', steam.STEAM_NAME), 0xE1139D3C)


def compiler():
    """A C++17 compiler for the cross-check: c++/clang++/g++ on Linux, MSVC (tools/msvc.py) on Windows."""
    if os.name == "nt":
        try:
            import msvc
            return ("msvc", msvc) if msvc.vs_install() else None
        except Exception:
            return None
    for name in ("c++", "clang++", "g++"):
        if shutil.which(name):
            return ("posix", shutil.which(name))
    return None


class CrossCheck(unittest.TestCase):
    """Compiles tools/tests/steam_shortcut_driver.cpp (steam_shortcut.h) and compares its output bytes."""

    @classmethod
    def setUpClass(cls):
        found = compiler()
        if not found:
            raise unittest.SkipTest("no C++ compiler for the steam_shortcut.h cross-check")
        cls.temp = tempfile.TemporaryDirectory(dir=TEST_ROOT)
        cls.dir = Path(cls.temp.name)
        source = Path(__file__).resolve().parent / "steam_shortcut_driver.cpp"
        kind, tool = found
        if kind == "msvc":
            cls.driver = cls.dir / "driver.exe"
            r = tool.run(["cl", "/nologo", "/std:c++17", "/EHsc", "/utf-8", "/Fe:" + str(cls.driver),
                          "/Fo:" + str(cls.dir) + "\\", str(source)], cwd=str(cls.dir))
        else:
            cls.driver = cls.dir / "driver"
            r = subprocess.run([tool, "-std=c++17", "-O1", "-o", str(cls.driver), str(source)],
                               capture_output=True, text=True)
        if r.returncode:
            cls.temp.cleanup()
            raise unittest.SkipTest("driver did not compile: " + (r.stdout + r.stderr)[-400:])

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def cpp(self, existing, name, exe, start_dir, icon=""):
        source = self.dir / "in.vdf"
        source.write_bytes(existing)
        target = self.dir / "out.vdf"
        target.unlink(missing_ok=True)
        r = subprocess.run([str(self.driver), str(source) if existing else "-", str(target), name, exe, start_dir, icon],
                           capture_output=True, check=True)
        result, appid = r.stdout.decode().split()
        return result, target.read_bytes() or None, int(appid) or None

    def test_same_bytes_and_appid(self):
        icon = "/home/deck/.local/share/Steam/userdata/1/config/grid/2_icon.png"
        cases = [(b"", steam.STEAM_NAME, GAME, PLAY, ""), (FIXTURE, steam.STEAM_NAME, GAME, PLAY, ""),
                 (b"not a vdf", steam.STEAM_NAME, GAME, PLAY, "")]
        _, first, _ = steam.add(FIXTURE, "HWDER", GAME, PLAY)
        cases += [(first, steam.STEAM_NAME, GAME, PLAY, icon), (first, "HWDER", GAME, PLAY, ""),
                  (first, "HWDER", GAME.upper(), PLAY, "")]
        for existing, name, exe, start_dir, icon_path in cases:
            # MSVC's argv is the ANSI code page: keep names passed to the driver ASCII there.
            with self.subTest(name=name, exe=exe, size=len(existing)):
                self.assertEqual(self.cpp(existing, name, exe, start_dir, icon_path),
                                 steam.add(existing, name, exe, start_dir, icon_path))


class Folders(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=TEST_ROOT)
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.steam = self.home / ".local/share/Steam"

    def account(self, number):
        (self.steam / "userdata" / str(number) / "config").mkdir(parents=True)

    def test_most_recent_login_wins(self):
        self.account(11)
        self.account(22)
        self.assertEqual(steam.steam_account(self.steam), "")  # two accounts, no login record
        (self.steam / "config").mkdir()
        (self.steam / "config/loginusers.vdf").write_text(
            '"users"\n{\n\t"76561197960265739"\n\t{\n\t\t"MostRecent"\t\t"0"\n\t}\n'
            '\t"76561197960265750"\n\t{\n\t\t"AccountName"\t\t"x"\n\t\t"MostRecent"\t\t"1"\n\t}\n}\n')
        self.assertEqual(steam.steam_account(self.steam), "22")

    def test_single_account_without_login_record(self):
        self.account(0)
        self.account(33)
        self.assertEqual(steam.steam_account(self.steam), "33")

    def test_roots_native_then_flatpak(self):
        flatpak = self.home / ".var/app/com.valvesoftware.Steam/.local/share/Steam"
        (flatpak / "userdata").mkdir(parents=True)
        self.account(1)
        roots = steam.steam_roots(self.home)
        self.assertEqual(roots, [(self.steam, False), (flatpak, True)])

    def test_game_mode(self):
        self.assertTrue(steam.game_mode({5: [b"steam", b"-gamepadui"]}, {}))
        self.assertTrue(steam.game_mode({}, {"XDG_CURRENT_DESKTOP": "gamescope"}))
        self.assertFalse(steam.game_mode({5: [b"steam", b"-silent"]}, {"XDG_CURRENT_DESKTOP": "KDE"}))

    def test_proc_scan(self):
        proc = self.home / "proc"
        for pid, comm in ((10, "steam"), (11, "steamwebhelper"), (12, "bash")):
            (proc / str(pid)).mkdir(parents=True)
            (proc / str(pid) / "comm").write_text(comm + "\n")
            (proc / str(pid) / "cmdline").write_bytes(comm.encode() + b"\x00-silent\x00")
        (proc / "self").mkdir()
        self.assertEqual(list(steam.steam_processes(str(proc))), [10])

    def test_write_with_backup(self):
        self.account(7)
        file = self.steam / "userdata/7/config/shortcuts.vdf"
        file.write_bytes(FIXTURE)
        target = self.home / "play" / steam.LAUNCHER
        ok, message = steam.add_to_root(self.steam, target, artwork=False)
        self.assertTrue(ok, message)
        backups = list(file.parent.glob("shortcuts.vdf.bak-*"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), FIXTURE)
        self.assertEqual(file.read_bytes()[:len(FIXTURE) - 2], FIXTURE[:-2])
        ok, message = steam.add_to_root(self.steam, target, artwork=False)
        self.assertEqual((ok, message), (True, "HWDER is already in your Steam library."))
        self.assertEqual(len(list(file.parent.glob("shortcuts.vdf.bak-*"))), 1)
        self.assertTrue(steam.entry_present(self.home / "play" / "hwder", self.home))

    def test_flow_with_running_steam(self):
        self.account(7)
        play = self.home / "play"
        play.mkdir()
        (play / "hwder").write_bytes(b"\x7fELF")
        state = {"running": {9: [b"steam", b"-silent"]}, "closed": 0, "started": 0}

        def close(flatpak, container):
            state["closed"] += 1
            state["running"] = {}
            return True

        def start(flatpak, container):
            state["started"] += 1
            return True

        with patch.object(steam, "WINDOWS", False):
            refused = steam.add_to_steam(play / "hwder", assume_yes=False, artwork=False,
                                         processes=lambda: state["running"], close=close, start=start,
                                         environ={"HOME": str(self.home)})
            self.assertFalse(refused)
            self.assertFalse((self.steam / "userdata/7/config/shortcuts.vdf").exists())
            ok = steam.add_to_steam(play / "hwder", assume_yes=True, artwork=False,
                                    processes=lambda: state["running"], close=close, start=start,
                                    environ={"HOME": str(self.home)})
        self.assertTrue(ok)
        self.assertEqual((state["closed"], state["started"]), (1, 1))
        script = (play / steam.LAUNCHER).read_text()
        self.assertIn("exec ./hwder \"$@\"", script)
        content = (self.steam / "userdata/7/config/shortcuts.vdf").read_bytes()
        self.assertEqual(steam.add(content, steam.STEAM_NAME, str((play / steam.LAUNCHER).resolve()),
                                   str(play.resolve()))[0], steam.ALREADY_THERE)

    def test_first_run_asks_once(self):
        import first_run
        self.account(7)
        cache = self.home / "cache"
        (cache / "play").mkdir(parents=True)
        asked = []
        with patch.object(steam.Path, "home", return_value=self.home):
            first_run.offer_steam(cache, cache / "play" / "hwder", "n", "k", ask=lambda q: asked.append(q) or False)
            first_run.offer_steam(cache, cache / "play" / "hwder", "n", "k", ask=lambda q: asked.append(q) or False)
        self.assertEqual(len(asked), 1)
        self.assertTrue(first_run.read_json(cache / "steam-offer.json")["declined"])
        self.assertFalse((self.steam / "userdata/7/config/shortcuts.vdf").exists())

    def test_game_paths_keep_other_settings(self):
        cache = self.home / "cache"
        (cache / "play").mkdir(parents=True)
        settings = cache / "play" / "hwder_settings.ini"
        settings.write_bytes(b"version=2\r\nnsp_path=/old.nsp\r\nupdate_path=/u\r\nsave_dir=/s\r\nfps120=1")
        steam.store_game_paths(cache, cache / "play" / "hwder", self.home / "g.nsp", self.home / "k.keys")
        self.assertEqual(settings.read_bytes(),
                         b"version=2\r\nnsp_path=" + str((self.home / "g.nsp").resolve()).encode()
                         + b"\r\nupdate_path=/u\r\nsave_dir=/s\r\nfps120=1\nkeys_path="
                         + str((self.home / "k.keys").resolve()).encode() + b"\n")

    def test_container_launcher(self):
        play = self.home / "play"
        play.mkdir()
        script = steam.write_launcher(play / "hwder", "hwder-dev").read_text()
        self.assertIn("exec distrobox-enter -n hwder-dev -- ", script)


if __name__ == "__main__":
    unittest.main(verbosity=2)
