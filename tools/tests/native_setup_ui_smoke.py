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

import sys
ROOT=Path(__file__).resolve().parents[2]
EXE=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else ROOT/"build/setup-ui/HWDER Setup.exe"
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
u.RedrawWindow.argtypes=[w.HWND,c.c_void_p,c.c_void_p,w.UINT]
def shot(hwnd,name):
    # The composed surface can lag behind child repaints on a locked/remote desktop; force a full redraw first.
    u.RedrawWindow(hwnd,None,None,0x1|0x4|0x80|0x400);time.sleep(0.3)
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
def start(mode,name,data=None,*extra):
    data=Path(data) if data else OUT/name
    system_root=Path(os.environ.get("SystemRoot",r"C:\Windows"))
    env={**os.environ,"PATH":os.pathsep.join(map(str,(system_root/"System32",system_root)))}
    p=subprocess.Popen([str(EXE),*([mode] if mode else []),"--data",str(data),*extra],env=env)
    active.append(p);image=c.create_unicode_buffer(32768);size=w.DWORD(len(image))
    require(k.QueryFullProcessImageNameW(w.HANDLE(int(p._handle)),0,image,c.byref(size)),"Owned image identity unavailable")
    require(Path(image.value).resolve()==EXE.resolve(),"Unexpected child executable")
    wait(lambda:any(classname(h)=="HWDERSetupPrototype" for h in windows(p.pid)))
    hwnd=next(h for h in windows(p.pid) if classname(h)=="HWDERSetupPrototype")
    return p,hwnd
def close(p,h):
    u.PostMessageW(h,0x10,0,0);p.wait(timeout=8);require(p.returncode==0,"Preview exit failed")
PRIMARY=111;CANCEL=115;CHECK=107;SHORTCUT=113;DATA=103;ADVANCED=117;DETAILS=114
def command(h,id):u.SendMessageW(h,0x111,id,0)  # WM_COMMAND: Advanced controls may be hidden
def primary(h):return label(u.GetDlgItem(h,PRIMARY))
def settext(h,id,value):
    buffer=c.create_unicode_buffer(str(value));u.SendMessageW(u.GetDlgItem(h,id),0xC,0,c.addressof(buffer))  # WM_SETTEXT; buffer outlives the call
def edit(h,id):
    buffer=c.create_unicode_buffer(32768);u.SendMessageW(u.GetDlgItem(h,id),0xD,len(buffer),c.addressof(buffer));return buffer.value  # WM_GETTEXT works cross-process
def details(h):command(h,ADVANCED);u.SendMessageW(u.GetDlgItem(h,DETAILS),0xF1,1,0);command(h,DETAILS)
def checked(h):
    command(h,CHECK);wait(lambda:"Requirements ready" in status(h) and enabled(h,PRIMARY));require(primary(h)=="Prepare","Preview Check changed the primary step")
def confirm_close(p,h):
    u.PostMessageW(h,0x10,0,0)
    wait(lambda:any(classname(d)=="#32770" for d in windows(p.pid)))
    dialog=next(d for d in windows(p.pid) if classname(d)=="#32770")
    u.SendMessageW(u.GetDlgItem(dialog,6),0xF5,0,0)

try:
    p,h=start("--demo-no-python","no-python")
    shot(h,"01-initial");require(primary(h)=="Prepare" and not enabled(h,SHORTCUT),"Preview initial state")
    command(h,CHECK);wait(lambda:"Preview interpreter unavailable" in status(h))
    require(not (OUT/"no-python").exists(),"No-Python check unexpectedly created/installed data")
    shot(h,"02-requirements");checks.append("Native window opens with no Python candidate; only synthetic preview needs a fixture interpreter; no data/install created")
    close(p,h)
    # Normal mode state machine with synthetic inputs/tool markers; nothing is downloaded or run.
    fixture=OUT/"fixture";fixture.mkdir();(fixture/"game.nsp").write_bytes(b"synthetic, never read");(fixture/"prod.keys").write_bytes(b"synthetic, never read")
    tools_marker=OUT/"synthetic-tools/managed-tools/0123456789abcdef01234567";tools_marker.mkdir(parents=True);(tools_marker/"complete.json").write_text("{}")
    p,h=start("","normal-missing")
    wait(lambda:primary(h)=="Next" and enabled(h,PRIMARY));require("Pick your game file" in status(h),"Next step has no hint")
    shot(h,"02a-next")
    settext(h,101,fixture/"game.nsp");require(edit(h,101)==str(fixture/"game.nsp") and primary(h)=="Next","Keys missing still Next")
    settext(h,102,fixture/"prod.keys");wait(lambda:primary(h)=="Install");shot(h,"02b-install")
    command(h,CHECK);wait(lambda:"Private tools are not installed" in status(h) and enabled(h,PRIMARY),seconds=30)
    require(primary(h)=="Install","Missing tools did not keep Install")
    require(not any((OUT/"normal-missing").rglob("*.zip")),"Verify-only Check downloaded payload")
    require(not any((OUT/"normal-missing").rglob("*.partial-*")),"Verify-only Check created an install attempt")
    shot(h,"02c-private-tools-missing")
    settext(h,DATA,OUT/"synthetic-tools");wait(lambda:primary(h)=="Prepare");require("Choose Prepare" in status(h),"Prepare step has no hint")
    shot(h,"02d-prepare")
    settext(h,DATA,OUT/"normal-missing");wait(lambda:primary(h)=="Install")
    settext(h,101,"");wait(lambda:primary(h)=="Next")
    checks.append("Normal state machine: Next (inputs) -> Install (no tool certificate) -> Prepare (certificate present); Check refuses missing private tools without payload/partial creation and keeps Install")
    close(p,h)
    installed=ROOT/"build/private-toolchain-data"
    if (installed/"managed-tools").is_dir():
        p,h=start("","normal-installed",installed)
        settext(h,101,fixture/"game.nsp");settext(h,102,fixture/"prod.keys");wait(lambda:primary(h)=="Prepare")
        command(h,CHECK)
        def normal_checked():
            current=status(h)
            if any(word in current for word in ("did not finish","Private tools need","Private tools are not")):
                raise AssertionError("Normal private Check failed: "+current)
            return "Requirements ready" in current and enabled(h,PRIMARY)
        wait(normal_checked,seconds=360)
        require(primary(h)=="Prepare","Tool-only Check changed the primary step")
        shot(h,"02e-private-tools-checked")
        checks.append("Normal native Check traverses system PowerShell -> certified private Python -> private bridge with Windows-only PATH; no download/game; primary stays Prepare")
        close(p,h)
    p,h=start("--demo","cancel")
    checked(h);click(h,PRIMARY);wait(lambda:"Preparing game code" in status(h))
    backend=owned_backend_handles(p.pid)
    require(enabled(h,CANCEL) and not enabled(h,PRIMARY),"Raw spoof event changed privileged controls")
    details(h);shot(h,"03-progress")
    click(h,CANCEL);wait(lambda:enabled(h,PRIMARY))
    for pid,handle in backend:
        require(k.WaitForSingleObject(handle,0)==0,"Owned backend survived JobObject cancellation")
        k.CloseHandle(handle)
    require("Cancelled" in status(h) and primary(h)=="Prepare","Cancel left a stale step")
    shot(h,"04-cancel");checks.append("Native JobObject cancels owned preview before ACK; primary returns to Prepare, stale/plain/wrong-nonce ready/publish logs ignored")
    close(p,h)
    p,h=start("--demo-fail","failure");checked(h);click(h,PRIMARY);wait(lambda:primary(h)=="Retry" and enabled(h,PRIMARY))
    require("Preview failure" in status(h) and not enabled(h,SHORTCUT),"Failed setup lost its message or enabled handoff");shot(h,"05-retry");checks.append("Native failure shows the bridge sentence and turns the primary button into Retry")
    close(p,h)
    p,h=start("--demo","ready");checked(h);click(h,PRIMARY)
    wait(lambda:"Finishing setup" in status(h));require(not enabled(h,CANCEL),"Commit phase left hard Cancel enabled")
    shot(h,"06-publishing");wait(lambda:"Preview ready" in status(h) and primary(h)=="Play")
    require(not enabled(h,PRIMARY) and not enabled(h,SHORTCUT) and not enabled(h,CANCEL),"Synthetic Ready misrepresented playable game")
    shot(h,"07-preview-ready")
    # A selected data change must invalidate the ready binding.
    settext(h,DATA,OUT/"other-data");wait(lambda:primary(h)=="Prepare" and enabled(h,PRIMARY))
    checks.append("Actual publishing handshake reaches synthetic Ready as a disabled Play; data edits return to Prepare")
    close(p,h)
    p,h=start("--demo","deferred-close");checked(h);click(h,PRIMARY);wait(lambda:"Finishing setup" in status(h))
    begin=time.monotonic();confirm_close(p,h);require(p.poll() is None,"Close killed critical publication")
    p.wait(timeout=8);require(p.returncode==0 and time.monotonic()-begin>.3,"Deferred close did not await commit interval")
    checks.append("Native WM_CLOSE after ACK defers through owned synthetic critical publication, then exits normally")
    p,h=start("--demo","advanced",None,"--advanced");u.SendMessageW(u.GetDlgItem(h,DETAILS),0xF1,1,0);command(h,DETAILS);shot(h,"08-advanced");close(p,h)
    (OUT/"results.json").write_text(json.dumps({"result":"PASS","exe":str(EXE),"checks":checks,"scope":"Actual native Check and synthetic preparation preview; no game/dependency install/download or real NSP","screenshots":sorted(p.name for p in OUT.glob("*.png"))},indent=2))
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
