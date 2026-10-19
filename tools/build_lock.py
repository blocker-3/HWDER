"""One build lock per user across worktrees, using the actual Windows profile path."""
import contextlib
import errno
import os
from pathlib import Path
import time


def lock_path():
    if os.name == "nt":
        # LOCALAPPDATA can be redirected by an agent sandbox. Shell32 reads the host
        # profile's known folder, matching ordinary native build.py invocations.
        import ctypes
        buffer = ctypes.create_unicode_buffer(32768)
        result = ctypes.windll.shell32.SHGetFolderPathW(None, 0x1C, None, 0, buffer)
        if result != 0:
            raise OSError("Cannot resolve the host Local AppData build lock")
        base = Path(buffer.value)
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
