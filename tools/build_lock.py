"""One build lock per user across worktrees, using the actual Windows profile path."""
import contextlib
import errno
import os
from pathlib import Path
import time


def lock_path():
    if os.name == "nt":
        # Both LOCALAPPDATA and legacy SHGetFolderPathW redirect inside AppContainers.
        # Explicitly disable package redirection to share ordinary host builds' lock.
        import ctypes
        from ctypes import wintypes
        import uuid
        class GUID(ctypes.Structure):
            _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD),
                        ("Data3", wintypes.WORD), ("Data4", ctypes.c_ubyte * 8)]
        folder = GUID.from_buffer_copy(uuid.UUID("F1B32785-6FBA-4FCF-9D55-7B8E7F157091").bytes_le)
        shell = ctypes.WinDLL("shell32")
        ole = ctypes.WinDLL("ole32")
        get_path = shell.SHGetKnownFolderPath
        get_path.argtypes = [ctypes.POINTER(GUID), wintypes.DWORD, wintypes.HANDLE,
                            ctypes.POINTER(ctypes.c_void_p)]
        get_path.restype = ctypes.c_long
        ole.CoInitializeEx.argtypes = [ctypes.c_void_p, wintypes.DWORD]
        ole.CoInitializeEx.restype = ctypes.c_long
        ole.CoTaskMemFree.argtypes = [ctypes.c_void_p]
        ole.CoTaskMemFree.restype = None
        initialized = ole.CoInitializeEx(None, 2)
        # RPC_E_CHANGED_MODE means the calling thread already has another COM mode.
        if initialized not in (0, 1, -2147417850):
            raise OSError("Cannot initialize the host known-folder lookup")
        raw_path = ctypes.c_void_p()
        try:
            result = get_path(ctypes.byref(folder), 0x10000, None, ctypes.byref(raw_path))
            if result != 0 or not raw_path.value:
                raise OSError("Cannot resolve the host Local AppData build lock")
            base = Path(ctypes.wstring_at(raw_path))
        finally:
            if raw_path.value:
                ole.CoTaskMemFree(raw_path)
            if initialized in (0, 1):
                ole.CoUninitialize()
    else:
        base = Path(os.environ.get("XDG_STATE_HOME", str(Path.home() / ".local/state")))
    return base / "hwder" / "build.lock"


@contextlib.contextmanager
def build_lock():
    path = lock_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+b") as stream:
        # msvcrt can lock past EOF. Do not seed a byte: an older build.py may
        # already own byte zero of an empty file, making that write fail.
        waited = False
        while True:
            try:
                if os.name == "nt":
                    import msvcrt
                    stream.seek(0)
                    msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except OSError as error:
                if error.errno not in (errno.EACCES, errno.EAGAIN, errno.EDEADLK):
                    raise
                if not waited:
                    print(f"build.py: waiting for another build to finish ({path})", flush=True)
                    waited = True
                time.sleep(1)
        try:
            yield
        finally:
            if os.name == "nt":
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(stream, fcntl.LOCK_UN)
