"""Controlled Windows native preview only. Never installs, builds or launches a game."""
import ctypes as c
from ctypes import wintypes as w
import itertools
import json
import os
from pathlib import Path
import struct
import subprocess
import time
import uuid
import zlib

ROOT=Path(__file__).resolve().parents[2]
EXE=ROOT/"build/setup-ui/HWDER Setup.exe"
OUT=ROOT/"runs"/("setup-preview-"+uuid.uuid4().hex[:8])
OUT.mkdir(parents=True)
assert EXE.is_file() and OUT.resolve().is_relative_to(ROOT.resolve())
u=c.WinDLL("user32",use_last_error=True);g=c.WinDLL("gdi32",use_last_error=True);k=c.WinDLL("kernel32",use_last_error=True)
u.SetProcessDpiAwarenessContext.argtypes=[c.c_void_p]
u.SetProcessDpiAwarenessContext(c.c_void_p(-4))  # physical bounds, not virtualized half-size screenshots
u.GetWindowDpiAwarenessContext.argtypes=[w.HWND];u.GetWindowDpiAwarenessContext.restype=c.c_void_p
u.SetThreadDpiAwarenessContext.argtypes=[c.c_void_p];u.SetThreadDpiAwarenessContext.restype=c.c_void_p
u.GetDlgItem.argtypes=[w.HWND,c.c_int];u.GetDlgItem.restype=w.HWND
u.SendMessageW.argtypes=[w.HWND,w.UINT,c.c_size_t,c.c_ssize_t];u.SendMessageW.restype=c.c_ssize_t
u.GetWindowTextW.argtypes=[w.HWND,w.LPWSTR,c.c_int]
u.GetClassNameW.argtypes=[w.HWND,w.LPWSTR,c.c_int]
u.GetWindowThreadProcessId.argtypes=[w.HWND,c.POINTER(w.DWORD)]
u.GetWindowRect.argtypes=[w.HWND,c.POINTER(w.RECT)]
u.IsWindowEnabled.argtypes=[w.HWND];u.IsWindowEnabled.restype=w.BOOL
u.PostMessageW.argtypes=[w.HWND,w.UINT,c.c_size_t,c.c_ssize_t]
u.GetDC.argtypes=[w.HWND];u.GetDC.restype=w.HDC
u.ReleaseDC.argtypes=[w.HWND,w.HDC]
u.PrintWindow.argtypes=[w.HWND,w.HDC,w.UINT];u.PrintWindow.restype=w.BOOL
g.CreateCompatibleDC.argtypes=[w.HDC];g.CreateCompatibleDC.restype=w.HDC
g.CreateCompatibleBitmap.argtypes=[w.HDC,c.c_int,c.c_int];g.CreateCompatibleBitmap.restype=w.HBITMAP
g.SelectObject.argtypes=[w.HDC,w.HANDLE];g.SelectObject.restype=w.HANDLE
g.DeleteObject.argtypes=[w.HANDLE];g.DeleteDC.argtypes=[w.HDC]
g.GetDIBits.argtypes=[w.HDC,w.HBITMAP,w.UINT,w.UINT,w.LPVOID,w.LPVOID,w.UINT]
callback=c.WINFUNCTYPE(w.BOOL,w.HWND,c.c_ssize_t)
u.EnumWindows.argtypes=[callback,c.c_ssize_t]
u.EnumChildWindows.argtypes=[w.HWND,callback,c.c_ssize_t]
k.QueryFullProcessImageNameW.argtypes=[w.HANDLE,w.DWORD,w.LPWSTR,c.POINTER(w.DWORD)]
k.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD];k.OpenProcess.restype=w.HANDLE
k.WaitForSingleObject.argtypes=[w.HANDLE,w.DWORD];k.WaitForSingleObject.restype=w.DWORD
k.CloseHandle.argtypes=[w.HANDLE]
class ProcessEntry(c.Structure):
    _fields_=[("dwSize",w.DWORD),("cntUsage",w.DWORD),("pid",w.DWORD),("heap",c.c_size_t),
              ("module",w.DWORD),("threads",w.DWORD),("parent",w.DWORD),("priority",w.LONG),
              ("flags",w.DWORD),("exe",w.WCHAR*260)]
k.CreateToolhelp32Snapshot.argtypes=[w.DWORD,w.DWORD];k.CreateToolhelp32Snapshot.restype=w.HANDLE
k.Process32FirstW.argtypes=[w.HANDLE,c.POINTER(ProcessEntry)];k.Process32NextW.argtypes=[w.HANDLE,c.POINTER(ProcessEntry)]
checks=[];active=[]
def owned_backend_handles(parent):
    snapshot=k.CreateToolhelp32Snapshot(2,0);entry=ProcessEntry();entry.dwSize=c.sizeof(entry);handles=[];entries=[]
    try:
        ok=k.Process32FirstW(snapshot,c.byref(entry))
        while ok:
            entries.append((entry.pid,entry.parent,entry.exe.lower()))
            ok=k.Process32NextW(snapshot,c.byref(entry))
    finally:k.CloseHandle(snapshot)
    owned={parent}
    for _ in range(8):
        found={pid for pid,ppid,name in entries if ppid in owned}
        if found.issubset(owned):break
        owned.update(found)
    names=[]
    for pid,ppid,name in entries:
        if pid in owned and pid!=parent and name in ("python.exe","powershell.exe"):
            handle=k.OpenProcess(0x100000|0x1000,False,pid)
            if handle:handles.append((pid,handle));names.append(name)
    require("python.exe" in names and "powershell.exe" in names,"No exact owned PowerShell/private bridge chain found")
    return handles
def require(value,message):
    if not value:raise AssertionError(message)
def label(hwnd):
    b=c.create_unicode_buffer(32768);u.GetWindowTextW(hwnd,b,len(b));return b.value
def classname(hwnd):
    b=c.create_unicode_buffer(128);u.GetClassNameW(hwnd,b,len(b));return b.value
def windows(pid):
    found=[]
    @callback
    def visit(h,_):
        p=w.DWORD();u.GetWindowThreadProcessId(h,c.byref(p))
        if p.value==pid:found.append(h)
        return True
    u.EnumWindows(visit,0);return found
def children(hwnd):
    found=[]
    @callback
    def visit(h,_):found.append(h);return True
    u.EnumChildWindows(hwnd,visit,0);return found
def status(hwnd):
    return [label(h) for h in children(hwnd) if classname(h)=="Static"][-1]
def enabled(hwnd,id):return bool(u.IsWindowEnabled(u.GetDlgItem(hwnd,id)))
def click(hwnd,id):u.SendMessageW(u.GetDlgItem(hwnd,id),0xF5,0,0)
def wait(test,seconds=10):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        if test():return
        time.sleep(.03)
    raise AssertionError("Timed out waiting for native UI state")
def shot(hwnd,name):
    previous=u.SetThreadDpiAwarenessContext(u.GetWindowDpiAwarenessContext(hwnd))
    r=w.RECT();u.GetWindowRect(hwnd,c.byref(r));width,height=r.right-r.left,r.bottom-r.top
    dc=u.GetDC(hwnd);mem=g.CreateCompatibleDC(dc);bitmap=g.CreateCompatibleBitmap(dc,width,height);old=g.SelectObject(mem,bitmap)
    try:
        require(u.PrintWindow(hwnd,mem,2),"PrintWindow failed")
        g.SelectObject(mem,old)
        header=c.create_string_buffer(struct.pack("<IiiHHIIiiII",40,width,-height,1,32,0,width*height*4,0,0,0,0))
        raw=c.create_string_buffer(width*height*4);require(g.GetDIBits(dc,bitmap,0,height,raw,header,0)==height,"GetDIBits failed")
        data=raw.raw;rows=[]
        for y in range(height):
            row=data[y*width*4:(y+1)*width*4]
            rows.append(b"\0"+bytes(itertools.chain.from_iterable(zip(row[2::4],row[1::4],row[0::4]))))
        def chunk(kind,payload):return struct.pack(">I",len(payload))+kind+payload+struct.pack(">I",zlib.crc32(kind+payload)&0xffffffff)
        png=b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR",struct.pack(">IIBBBBB",width,height,8,2,0,0,0))+chunk(b"IDAT",zlib.compress(b"".join(rows)))+chunk(b"IEND",b"")
        (OUT/(name+".png")).write_bytes(png)
    finally:
        g.SelectObject(mem,old);g.DeleteObject(bitmap);g.DeleteDC(mem);u.ReleaseDC(hwnd,dc)
        u.SetThreadDpiAwarenessContext(previous)
def start(mode,name,data=None):
    data=Path(data) if data else OUT/name
    system_root=Path(os.environ.get("SystemRoot",r"C:\Windows"))
    env={**os.environ,"PATH":os.pathsep.join(map(str,(system_root/"System32",system_root)))}
    p=subprocess.Popen([str(EXE),*([mode] if mode else []),"--data",str(data)],env=env)
    active.append(p);image=c.create_unicode_buffer(32768);size=w.DWORD(len(image))
    require(k.QueryFullProcessImageNameW(w.HANDLE(int(p._handle)),0,image,c.byref(size)),"Owned image identity unavailable")
    require(Path(image.value).resolve()==EXE.resolve(),"Unexpected child executable")
    wait(lambda:any(classname(h)=="HWDERSetupPrototype" for h in windows(p.pid)))
    hwnd=next(h for h in windows(p.pid) if classname(h)=="HWDERSetupPrototype")
    return p,hwnd
def close(p,h):
    u.PostMessageW(h,0x10,0,0);p.wait(timeout=8);require(p.returncode==0,"Preview exit failed")
def checked(h):
    click(h,107);wait(lambda:enabled(h,111));require(not enabled(h,112),"Preview enabled Play")
def confirm_close(p,h):
    u.PostMessageW(h,0x10,0,0)
    wait(lambda:any(classname(d)=="#32770" for d in windows(p.pid)))
    dialog=next(d for d in windows(p.pid) if classname(d)=="#32770")
    u.SendMessageW(u.GetDlgItem(dialog,6),0xF5,0,0)

try:
    p,h=start("--demo-no-python","no-python")
    shot(h,"01-initial");require(not enabled(h,111) and not enabled(h,112),"Initial unsafe controls")
    click(h,107);wait(lambda:"Preview interpreter unavailable" in status(h))
    require(not (OUT/"no-python").exists(),"No-Python check unexpectedly created/installed data")
    shot(h,"02-requirements");checks.append("Native window opens with no Python candidate; only synthetic preview needs a fixture interpreter; no data/install created")
    close(p,h)
    p,h=start("","normal-missing")
    click(h,107);wait(lambda:"Private tools are not ready" in status(h),seconds=30)
    require(not enabled(h,111) and not enabled(h,112),"Missing tools enabled preparation/Play")
    require(not any((OUT/"normal-missing").rglob("*.zip")),"Verify-only Check downloaded payload")
    require(not any((OUT/"normal-missing").rglob("*.partial-*")),"Verify-only Check created an install attempt")
    shot(h,"02b-private-tools-missing")
    checks.append("Normal Check with Windows-only PATH refuses missing private tools without payload/partial creation")
    close(p,h)
    installed=ROOT/"build/private-toolchain-data"
    if (installed/"managed-tools").is_dir():
        p,h=start("","normal-installed",installed)
        click(h,107)
        def normal_checked():
            current=status(h)
            if any(word in current for word in ("did not finish","Private tools need","Private tools are not ready")):
                raise AssertionError("Normal private Check failed: "+current)
            return "Requirements ready" in current and enabled(h,111)
        wait(normal_checked,seconds=360)
        require(not enabled(h,112),"Tool-only Check enabled game Play")
        shot(h,"02c-private-tools-checked")
        checks.append("Normal native Check traverses system PowerShell → certified private Python → private bridge with Windows-only PATH; no download/game")
        close(p,h)
    p,h=start("--demo","cancel")
    checked(h);click(h,111);wait(lambda:"Preparing game code" in status(h))
    backend=owned_backend_handles(p.pid)
    require(enabled(h,115) and not enabled(h,112),"Raw spoof event changed privileged controls")
    click(h,114);shot(h,"03-progress")
    click(h,115);wait(lambda:enabled(h,111))
    for pid,handle in backend:
        require(k.WaitForSingleObject(handle,0)==0,"Owned backend survived JobObject cancellation")
        k.CloseHandle(handle)
    require("Cancelled before" in status(h) and not enabled(h,112),"Cancel enabled stale Play")
    shot(h,"04-cancel");checks.append("Native JobObject cancels owned preview before ACK; retry enabled, stale/plain/wrong-nonce ready/publish logs ignored")
    close(p,h)
    p,h=start("--demo-fail","failure");checked(h);click(h,111);wait(lambda:label(u.GetDlgItem(h,111))=="Retry" and enabled(h,111))
    require(not enabled(h,112),"Failed setup enabled Play");shot(h,"05-retry");checks.append("Native failure leaves Retry enabled and Play disabled")
    close(p,h)
    p,h=start("--demo","ready");checked(h);click(h,111)
    wait(lambda:"Finishing setup" in status(h));require(not enabled(h,115),"Commit phase left hard Cancel enabled")
    shot(h,"06-publishing");wait(lambda:enabled(h,111))
    require("Preview ready" in status(h) and not enabled(h,112),"Synthetic Ready misrepresented playable game")
    shot(h,"07-preview-ready")
    # A selected data change must invalidate the requirements/ready binding.
    new_data=str(OUT/"other-data");u.SendMessageW(u.GetDlgItem(h,103),0xC,0,c.cast(c.c_wchar_p(new_data),c.c_void_p).value)
    require(not enabled(h,111) and not enabled(h,112),"Changing data left stale ready/requirements")
    checks.append("Actual publishing handshake reaches synthetic Ready; Play/shortcut remain disabled for preview; data edits invalidate requirements")
    close(p,h)
    p,h=start("--demo","deferred-close");checked(h);click(h,111);wait(lambda:"Finishing setup" in status(h))
    begin=time.monotonic();confirm_close(p,h);require(p.poll() is None,"Close killed critical publication")
    p.wait(timeout=8);require(p.returncode==0 and time.monotonic()-begin>.3,"Deferred close did not await commit interval")
    checks.append("Native WM_CLOSE after ACK defers through owned synthetic critical publication, then exits normally")
    (OUT/"results.json").write_text(json.dumps({"result":"PASS","exe":str(EXE),"checks":checks,"scope":"Native preview only; no game/dependency install/download or real NSP","screenshots":sorted(p.name for p in OUT.glob("*.png"))},indent=2))
    print("PASS native UI/JobObject/handshake/state tests:",OUT)
finally:
    for p in active:
        if p.poll() is None:
            for h in windows(p.pid):
                if classname(h)=="HWDERSetupPrototype":u.PostMessageW(h,0x10,0,0)
            try:p.wait(timeout=4)
            except subprocess.TimeoutExpired:
                # Only this exact Popen child. Kill-on-close job owns its synthetic descendants.
                p.terminate();p.wait(timeout=5)
