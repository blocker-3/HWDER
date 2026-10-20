"""Private, pinned Windows tools. No global installation, pip, or registry writes.

The native bootstrap installs only the pinned interpreter first. This module
runs with that interpreter and installs a separately certified complete toolset.
All network and process boundaries are injectable for no-install contract tests.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys
import urllib.parse
import urllib.request
import uuid
import zipfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import first_run

MANIFEST = Path(__file__).with_name("managed_toolchain.json")
OWNER = "hwder-private-tools-v1"
NETWORK_HOSTS = {"github.com", "release-assets.githubusercontent.com",
                 "objects.githubusercontent.com", "www.python.org", "files.pythonhosted.org"}
RESERVED = re.compile(r"^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)", re.I)


def manifest(path=MANIFEST):
    value = json.loads(Path(path).read_text(encoding="utf-8"))
    if value.get("schema") != 1 or value.get("owner") != OWNER:
        raise ValueError("Unknown private tool manifest")
    seen = set()
    for item in value["artifacts"]:
        name = item["id"]
        if not re.fullmatch(r"[a-z][a-z0-9-]*", name) or name in seen:
            raise ValueError("Invalid or duplicate tool ID")
        seen.add(name)
        check_url(item["url"])
        if not re.fullmatch(r"[0-9a-f]{64}", item["sha256"]):
            raise ValueError("Missing pinned tool checksum")
        for key, limit in (("max_download_bytes", 256_000_000),
                           ("max_unpacked_bytes", 2_000_000_000), ("max_entries", 50_000)):
            if type(item[key]) is not int or not 0 < item[key] <= limit:
                raise ValueError("Invalid tool budget")
        for relative in [item["archive_root"], *item["required"]]:
            if relative:
                safe_name(relative)
        if not item.get("license") or not item.get("license_source") or not item.get("metadata_source"):
            raise ValueError("Missing upstream notice/source record")
    return value


def check_url(url):
    parsed = urllib.parse.urlsplit(url)
    if (parsed.scheme != "https" or parsed.hostname not in NETWORK_HOSTS
            or parsed.username or parsed.password or parsed.port not in (None, 443)):
        raise ValueError("Tool download outside the pinned HTTPS host allowlist")


def safe_name(name):
    if "\\" in name or "\x00" in name or name.startswith("/") or any(c in name for c in ':<>"|?*'):
        raise ValueError("Unsafe ZIP member")
    parts = name.rstrip("/").split("/")
    if not parts or any(not p or p in (".", "..") or p[-1:] in (" ", ".")
                        or RESERVED.match(p) or any(ord(c) < 32 for c in p) for p in parts):
        raise ValueError("Unsafe Windows ZIP member")
    return PurePosixPath(*parts)


def owned_path(path, root):
    """Reject redirects, including existing junctions anywhere below the owned root."""
    root, path = Path(root).absolute(), Path(path).absolute()
    if path != root and root not in path.parents:
        raise ValueError("Tool output escaped its private root")
    # Include ancestors of root too: a user-selected redirected data root must
    # be rejected rather than silently installing somewhere else.
    for part in [path, *path.parents]:
        if part.exists():
            info = part.lstat()
            if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
                raise ValueError("Redirected tool output is unsupported")
    return path


class HTTPSOnly(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        check_url(newurl)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def download(item, target, progress=lambda *a: None, cancelled=lambda: False):
    """Create a unique partial only. Callers verify its bytes before promotion."""
    check_url(item["url"])
    opener = urllib.request.build_opener(HTTPSOnly())
    request = urllib.request.Request(item["url"], headers={"User-Agent": "HWDER-private-setup/1"})
    with opener.open(request, timeout=30) as response, Path(target).open("xb") as out:
        total = 0
        while True:
            if cancelled():
                raise InterruptedError("Private tool download cancelled")
            chunk = response.read(1024 * 1024)
            if not chunk:
                break
            total += len(chunk)
            if total > item["max_download_bytes"]:
                raise ValueError("Tool download exceeded its declared budget")
            out.write(chunk)
            progress(item["id"], total, item["max_download_bytes"])
    return total


def inspect_zip(archive, item):
    entries = []
    kinds = {}
    spelling = {}
    total = 0
    with zipfile.ZipFile(archive) as z:
        if len(z.infolist()) > item["max_entries"]:
            raise ValueError("Tool ZIP has too many entries")
        for info in z.infolist():
            # ZipInfo normalizes Windows backslashes on construction; validate
            # its original central-directory name before using the normalized one.
            relative = safe_name(info.orig_filename)
            key = relative.as_posix().casefold()
            mode = info.external_attr >> 16
            if (info.flag_bits & 1 or info.external_attr & 0x400 or stat.S_ISLNK(mode)
                    or stat.S_IFMT(mode) not in (0, stat.S_IFREG, stat.S_IFDIR)):
                raise ValueError("Encrypted/link/special tool ZIP entry")
            if key in kinds:
                raise ValueError("Case-colliding or duplicate tool ZIP entry")
            kinds[key] = info.is_dir()
            for length in range(1, len(relative.parts) + 1):
                prefix = "/".join(relative.parts[:length])
                folded = prefix.casefold()
                if folded in spelling and spelling[folded] != prefix:
                    raise ValueError("Case-colliding tool ZIP directory")
                spelling[folded] = prefix
            if info.file_size < 0 or info.file_size > item["max_unpacked_bytes"]:
                raise ValueError("Tool ZIP entry exceeded its budget")
            total += info.file_size
            if total > item["max_unpacked_bytes"]:
                raise ValueError("Tool ZIP exceeded its unpacked budget")
            entries.append((info, relative))
        for _, relative in entries:
            for parent in relative.parents:
                if parent.as_posix() != "." and kinds.get(parent.as_posix().casefold()) is False:
                    raise ValueError("Tool ZIP file/directory collision")
    return entries


def unpack(archive, destination, item, cancelled=lambda: False):
    """Preflight all names/budgets before creating any extracted member."""
    entries = inspect_zip(archive, item)
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=False)
    with zipfile.ZipFile(archive) as z:
        for info, relative in entries:
            if cancelled():
                raise InterruptedError("Private tool extraction cancelled")
            target = owned_path(destination.joinpath(*relative.parts), destination)
            if info.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with z.open(info) as src, target.open("xb") as out:
                count = 0
                while chunk := src.read(1024 * 1024):
                    if cancelled():
                        raise InterruptedError("Private tool extraction cancelled")
                    count += len(chunk)
                    if count > info.file_size:
                        raise ValueError("Tool ZIP entry size changed")
                    out.write(chunk)
                if count != info.file_size:
                    raise ValueError("Tool ZIP entry was truncated")
    base = destination / item["archive_root"]
    for relative in item["required"]:
        if not (base / relative).is_file():
            raise ValueError("Tool archive omitted " + relative)
    return base


def identity(value, source):
    # The embedded _pth binds generator children to this exact source/tools path.
    return first_run.digest({"manifest": value, "source": str(Path(source).resolve())})


def complete(directory, key):
    directory = Path(directory)
    certificate = first_run.read_json(directory / "complete.json")
    output = directory / "output"
    return (certificate.get("owner") == OWNER and certificate.get("key") == key
            and bool(certificate.get("files")) and output.is_dir()
            and certificate["files"] == tool_hashes(output))


def tool_hashes(root):
    for entry in Path(root).rglob("*"):
        owned_path(entry, root)
    return first_run.tree_hashes(root)


def probe_imports(output):
    script = (
        "import sys,struct;assert sys.version_info[:3]==(3,13,16);assert struct.calcsize('P')==8;"
        "from cryptography.hazmat.primitives.ciphers import Cipher,algorithms,modes;"
        "e=Cipher(algorithms.AES(bytes(16)),modes.ECB()).encryptor();"
        "assert (e.update(bytes(16))+e.finalize()).hex()=='66e94bd4ef8a2c3b884cfa59ca342b2e';"
        "import lz4.block;v=b'private synthetic setup'*31;"
        "assert lz4.block.decompress(lz4.block.compress(v))==v;"
        "import cffi,pycparser;assert cffi.__version__=='2.0.0';"
        "print('managed-python: AES/LZ4/import contracts PASS')"
    )
    subprocess.run([str(Path(output) / "python/python.exe"), "-I", "-B", "-c", script],
                   env=environment(Path(output).parent), check=True)


def install(data, source, value=None, fetch=download, progress=lambda *a: None,
            cancelled=lambda: False, validate=probe_imports):
    value = value or manifest()
    data, source = Path(data).absolute(), Path(source).resolve()
    owned_path(data, data)
    owned_path(data / "setup.lock", data)
    with first_run.setup_lock(data):
        root = owned_path(data / "managed-tools", data)
        root.mkdir(exist_ok=True)
        key = identity(value, source)
        final = root / key[:24]
        owned_path(final, data)
        if complete(final, key):
            return final
        downloads = owned_path(root / "downloads", data)
        downloads.mkdir(exist_ok=True)
        attempt = root / (key[:24] + ".partial-" + uuid.uuid4().hex[:8])
        output = attempt / "output"
        output.mkdir(parents=True)
        for item in value["artifacts"]:
            if cancelled():
                raise InterruptedError("Private tool setup cancelled")
            archive = downloads / (item["sha256"] + ".zip")
            owned_path(archive, data)
            if not archive.is_file() or first_run.file_hash(archive) != item["sha256"]:
                temporary = downloads / (item["sha256"] + ".partial-" + uuid.uuid4().hex[:8])
                fetch(item, temporary, progress, cancelled)
                if temporary.stat().st_size > item["max_download_bytes"] or first_run.file_hash(temporary) != item["sha256"]:
                    raise ValueError("Tool checksum mismatch: " + item["id"])
                os.replace(temporary, archive)
            unpack(archive, output / item["id"], item, cancelled)
        # Preserve every wheel file, including dist-info and licenses. No pip,
        # executable wheel hooks, .pth execution, or arbitrary wheel relocation.
        python = output / "python"
        packages = python / "Lib/site-packages"
        packages.mkdir(parents=True)
        for item in value["artifacts"]:
            if item["id"] in ("cryptography", "lz4", "cffi", "pycparser"):
                base = output / item["id"]
                for entry in base.iterdir():
                    if entry.name.endswith(".pth") or (packages / entry.name).exists():
                        raise ValueError("Unsupported/colliding wheel layout")
                    entry.rename(packages / entry.name)
        (python / "python313._pth").write_text(
            "python313.zip\n.\nLib/site-packages\n" + str(source / "tools") + "\nimport site\n", encoding="utf-8")
        validate(output)
        files = tool_hashes(output)
        if cancelled():
            raise InterruptedError("Private tool setup cancelled before publication")
        first_run.atomic_json(attempt / "complete.json",
                             {"owner": OWNER, "key": key, "files": files, "abi": value["abi"]})
        if final.exists():
            final.rename(root / (final.name + ".invalid-" + uuid.uuid4().hex[:8]))
        attempt.rename(final)
        return final


def paths(directory, value=None):
    value = value or manifest()
    root = Path(directory) / "output"
    by_id = {item["id"]: root / item["id"] / item["archive_root"] for item in value["artifacts"]}
    return {"python": by_id["python"] / "python.exe",
            "cc": by_id["llvm"] / "bin/x86_64-w64-mingw32-clang.exe",
            "cxx": by_id["llvm"] / "bin/x86_64-w64-mingw32-clang++.exe",
            "cmake": by_id["cmake"] / "bin/cmake.exe", "ninja": by_id["ninja"] / "ninja.exe"}


def environment(directory, value=None):
    selected = paths(directory, value)
    env = {key: val for key, val in os.environ.items()
           if key.upper() not in {"INCLUDE", "LIB", "LIBPATH", "CL", "_CL_", "LINK", "_LINK_",
                                  "CC", "CXX", "CFLAGS", "CXXFLAGS", "LDFLAGS", "CMAKE_PREFIX_PATH",
                                  "RC", "AR", "RANLIB", "LD", "CMAKE_TOOLCHAIN_FILE",
                                  "CMAKE_GENERATOR", "CMAKE_MAKE_PROGRAM",
                                  "PYTHONPATH", "PYTHONHOME", "HW_VCVARS", "VCTOOLSINSTALLDIR",
                                  "WINDOWSSDKDIR", "VSINSTALLDIR", "VCINSTALLDIR"}}
    windows = Path(os.environ.get("SystemRoot", r"C:\Windows"))
    env["PATH"] = os.pathsep.join(map(str, [selected["cc"].parent, selected["cmake"].parent,
                                           selected["ninja"].parent, windows / "System32", windows]))
    return env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", required=True, type=Path)
    parser.add_argument("--source", default=MANIFEST.parent.parent, type=Path)
    parser.add_argument("--install", action="store_true", help="explicit consent to pinned private downloads")
    args = parser.parse_args()
    value = manifest()
    if not args.install:
        print(json.dumps({"manifest_sha256": first_run.file_hash(MANIFEST), "downloads": value["artifacts"]}, indent=2))
        return
    result = install(args.data, args.source, value,
                     progress=lambda name, got, total: print(f"tools: {name} {got}/{total}", flush=True))
    print("tools: verified " + str(result), flush=True)


if __name__ == "__main__":
    main()
