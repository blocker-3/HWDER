"""Actual HWND/handle/shortcut lifecycle fixture. Only a fixed Python sleep child, never game."""
from pathlib import Path
import sys

# Reuse the existing Win32 screenshot/window helpers without running its broader test sequence.
helpers = Path(__file__).with_name("native_setup_ui_smoke.py")
exec(compile(helpers.read_text(encoding="utf-8").split('\ntry:\n    p,h=start(')[0], str(helpers), "exec"))
EXE = Path(sys.argv[1]).resolve()
assert EXE.is_relative_to(ROOT.resolve()) and EXE.is_file()
shortcut_dir = OUT / "desktop-fixture"
shortcut_dir.mkdir()
observed = []


def lifecycle(name, mode="--demo-lifecycle"):
    data = OUT / name
    system = Path(os.environ.get("SystemRoot", r"C:\Windows"))
    env = {**os.environ, "PATH": os.pathsep.join(map(str, (system / "System32", system)))}
    child = subprocess.Popen([str(EXE), mode, "--data", str(data),
                              "--shortcut-directory", str(shortcut_dir)], env=env)
    active.append(child)
    wait(lambda: any(classname(h) == "HWDERSetupPrototype" for h in windows(child.pid)))
    hwnd = next(h for h in windows(child.pid) if classname(h) == "HWDERSetupPrototype")
    wait(lambda: enabled(hwnd, 112))
    return child, hwnd, data


try:
    p, h, data = lifecycle("lifecycle")
    require("Ready to play" in status(h), "Startup did not discover Ready")
    require(enabled(h, 113) and enabled(h, 116), "Ready has no shortcut/folder handoff")
    shot(h, "01-ready-handoff")
    click(h, 112)
    wait(lambda: (data / "preview-child.json").exists())
    record = json.loads((data / "preview-child.json").read_text())
    handle = k.OpenProcess(0x100000 | 0x1000, False, int(record["pid"]))
    require(handle, "Cannot open exact synthetic child handle")
    wait(lambda: "Preview child running" in status(h))
    require(not enabled(h, 112) and not enabled(h, 115), "Running game allowed duplicate Play/Cancel")
    # Direct command delivery must also refuse duplicate launch, not only a disabled button.
    before = (data / "preview-child.json").read_bytes()
    u.SendMessageW(h, 0x111, 112, 0)
    time.sleep(0.7)  # Permit a wrongly started asynchronous helper to reach its fixed child spawn.
    require((data / "preview-child.json").read_bytes() == before, "Duplicate command replaced running child")
    shot(h, "02-running")
    wait(lambda: k.WaitForSingleObject(handle, 0) == 0)
    k.CloseHandle(handle)
    wait(lambda: enabled(h, 112) and enabled(h, 113), seconds=15)
    require("Ready to play" in status(h), "Game exit lost Ready")
    shot(h, "03-play-again")
    click(h, 113)
    require((shortcut_dir / "HWDER.lnk").is_file(), "Explicit shortcut was not created")
    script = "$s=(New-Object -ComObject WScript.Shell).CreateShortcut($args[0]);@{target=$s.TargetPath;cwd=$s.WorkingDirectory;arguments=$s.Arguments}|ConvertTo-Json -Compress"
    system = Path(os.environ.get("SystemRoot", r"C:\Windows"))
    # Inspect only; never invoke the shortcut or its synthetic executable.
    command = "$s=(New-Object -ComObject WScript.Shell).CreateShortcut('" + str(shortcut_dir / "HWDER.lnk").replace("'", "''") + "');@{target=$s.TargetPath;cwd=$s.WorkingDirectory;arguments=$s.Arguments}|ConvertTo-Json -Compress"
    result = subprocess.run([str(system / "System32/WindowsPowerShell/v1.0/powershell.exe"), "-NoProfile", "-NonInteractive", "-Command", command], capture_output=True, text=True, check=True)
    shortcut = json.loads(result.stdout)
    require(Path(shortcut["target"]).resolve() == Path(record["exe"]).resolve(), "Shortcut targets source kit rather than installed entry")
    require(Path(shortcut["cwd"]).resolve() == Path(record["exe"]).parent.resolve() and shortcut["arguments"] == "", "Shortcut cwd/arguments depend on kit")
    observed.append("Quick Ready enables explicit installed path/folder/shortcut; launch disables duplicates; owned child exit restores Play")
    click(h, 112)
    wait(lambda: json.loads((data / "preview-child.json").read_text())["pid"] != record["pid"])
    second = json.loads((data / "preview-child.json").read_text())
    handle = k.OpenProcess(0x100000 | 0x1000, False, int(second["pid"]))
    wait(lambda: "Preview child running" in status(h))
    close(p, h)
    require(k.WaitForSingleObject(handle, 0) == 258, "Closing Setup killed the unjobbed child")
    require(k.WaitForSingleObject(handle, 8000) == 0, "Synthetic child did not exit naturally")
    k.CloseHandle(handle)
    observed.append("Repeat Play works; closing Setup leaves exact unjobbed sleep child alive until natural exit; shortcut uses installed target/cwd")
    p, h, data = lifecycle("invalidation")
    u.SendMessageW(u.GetDlgItem(h, 103), 0xC, 0, c.cast(c.c_wchar_p(str(OUT / 'other-data')), c.c_void_p).value)
    require(not enabled(h, 112) and not enabled(h, 113), "Changed Data retained stale Ready/shortcut")
    close(p, h)
    observed.append("Data edit invalidates prepared binding and shortcut")
    p, h, data = lifecycle("exit-during-helper", "--demo-lifecycle-race")
    require((data / "preview-race-once").exists(), "Near-exit fixture was not exercised")
    wait(lambda: enabled(h, 113), seconds=15)
    require("Ready to play" in status(h), "Near-exit helper race lost Ready/installed-path refresh")
    shot(h, "04-immediate-exit-ready")
    close(p, h)
    observed.append("Child exits before startup worker completion; deferred quick status restores ready and standalone shortcut")
    (OUT / "play-results.json").write_text(json.dumps({"result": "PASS", "exe": str(EXE), "checks": observed,
         "shortcut": shortcut, "scope": "Actual native HWND/process handle/COM shortcut; fixed Python sleep fixture only; no real game, inputs, tools installation or user Data", "screenshots": sorted(x.name for x in OUT.glob('*.png'))}, indent=2))
    print("PASS native Play lifecycle:", OUT)
finally:
    for p in active:
        if p.poll() is None:
            for h in windows(p.pid):
                if classname(h) == "HWDERSetupPrototype": u.PostMessageW(h, 0x10, 0, 0)
            p.wait(timeout=8)
