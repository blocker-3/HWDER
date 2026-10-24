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
import time
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


class FileProgress:
    """Actual completed work only; at most two intermediate updates per second."""
    def __init__(self, callback=None, clock=None):
        self.callback=callback
        self.clock=clock or time.monotonic
        self.last=None

    def update(self, count, finished=False):
        if self.callback is None:
            return
        now=self.clock()
        if finished or self.last is None or now-self.last>=0.5:
            self.callback(count,finished)
            self.last=now

def filesystem_path(path):
    """Use Win32 extended paths locally; never change the host long-path policy."""
    path = Path(path).absolute()
    if os.name == "nt" and not str(path).startswith("\\\\?\\"):
        return Path("\\\\?\\" + str(path))
    return path


def plain_path(path):
    text = str(path)
    if os.name == "nt" and text.startswith("\\\\?\\"):
        text = text[4:]
    return Path(os.path.abspath(text))


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
    root, path = plain_path(root), plain_path(path)
    if path != root and root not in path.parents:
        raise ValueError("Tool output escaped its private root")
    # Include ancestors of root too: a user-selected redirected data root must
    # be rejected rather than silently installing somewhere else.
    for part in [path, *path.parents]:
        native = filesystem_path(part)
        if native.exists():
            info = native.lstat()
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


def unpack(archive, destination, item, cancelled=lambda: False, progress=None):
    """Preflight all names/budgets before creating any extracted member."""
    entries = inspect_zip(archive, item)
    counter=FileProgress(progress)
    destination = Path(destination)
    filesystem_path(destination).mkdir(parents=True, exist_ok=False)
    with zipfile.ZipFile(archive) as z:
        for completed,(info, relative) in enumerate(entries,1):
            if cancelled():
                raise InterruptedError("Private tool extraction cancelled")
            target = owned_path(destination.joinpath(*relative.parts), destination)
            target = filesystem_path(target)
            if info.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                counter.update(completed)
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
            counter.update(completed)
    base = destination / item["archive_root"]
    for relative in item["required"]:
        if not filesystem_path(base / relative).is_file():
            raise ValueError("Tool archive omitted " + relative)
    counter.update(len(entries),True)
    return base


def identity(value, source):
    # The embedded _pth binds generator children to this exact source/tools path.
    return first_run.digest({"manifest": value, "source": str(Path(source).resolve())})


def complete(directory, key, progress=None):
    directory = Path(directory)
    certificate = first_run.read_json(directory / "complete.json")
    output = directory / "output"
    return (certificate.get("owner") == OWNER and certificate.get("key") == key
            and bool(certificate.get("files")) and output.is_dir()
            and certificate["files"] == tool_hashes(output,progress))


def tool_hashes(root, progress=None):
    # Check ancestors once, then each entry before traversal; do not follow
    # redirects. The old per-header ancestor scan took minutes for 17k files.
    root = filesystem_path(owned_path(root, root))
    pending = [root]
    result = {}
    counter=FileProgress(progress)
    while pending:
        directory = pending.pop()
        with os.scandir(directory) as entries:
            for entry in entries:
                info = entry.stat(follow_symlinks=False)
                if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
                    raise ValueError("Redirected tool output is unsupported")
                path = Path(entry.path)
                if stat.S_ISDIR(info.st_mode):
                    pending.append(path)
                elif stat.S_ISREG(info.st_mode):
                    result[path.relative_to(root).as_posix()] = first_run.file_hash(path)
                    counter.update(len(result))
                else:
                    raise ValueError("Nonregular private tool output")
    counter.update(len(result),True)
    return result


def probe_imports(output):
    script = (
        "import sys,struct;assert sys.version_info[:3]==(3,13,16);assert struct.calcsize('P')==8;"
        "from cryptography.hazmat.primitives.ciphers import Cipher,algorithms,modes;"
        "e=Cipher(algorithms.AES(bytes(16)),modes.ECB()).encryptor();"
        "assert (e.update(bytes(16))+e.finalize()).hex()=='66e94bd4ef8a2c3b884cfa59ca342b2e';"
        "import lz4.block;v=b'private synthetic setup'*31;"
        "assert lz4.block.decompress(lz4.block.compress(v))==v;"
        "import cffi,pycparser;assert cffi.__version__=='2.0.0';"
        "import zstandard;assert zstandard.ZstdDecompressor().decompress(zstandard.ZstdCompressor().compress(v))==v;"
        "print('managed-python: AES/LZ4/zstd/import contracts PASS')"
    )
    subprocess.run([str(Path(output) / "python/python.exe"), "-I", "-B", "-c", script],
                   env=environment(Path(output).parent), check=True)


def install(data, source, value=None, fetch=download, progress=lambda *a: None,
            cancelled=lambda: False, validate=probe_imports, phase=lambda *a,**k:None):
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
        phase("checking_installation")
        if complete(final, key,lambda count,done:phase("verify_existing",files=count,finished=done)):
            phase("installed_ready")
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
            phase("archive_check",artifact=item["id"])
            if not archive.is_file() or first_run.file_hash(archive) != item["sha256"]:
                temporary = downloads / (item["sha256"] + ".partial-" + uuid.uuid4().hex[:8])
                fetch(item, temporary, progress, cancelled)
                phase("download_checksum",artifact=item["id"])
                if temporary.stat().st_size > item["max_download_bytes"] or first_run.file_hash(temporary) != item["sha256"]:
                    raise ValueError("Tool checksum mismatch: " + item["id"])
                os.replace(temporary, archive)
            phase("extract",artifact=item["id"],entries=0,finished=False)
            unpack(archive, output / item["id"], item, cancelled,
                   lambda count,done:phase("extract",artifact=item["id"],entries=count,finished=done))
        # Preserve every wheel file, including dist-info and licenses. No pip,
        # executable wheel hooks, .pth execution, or arbitrary wheel relocation.
        phase("packages")
        python = output / "python"
        packages = python / "Lib/site-packages"
        packages.mkdir(parents=True)
        for item in value["artifacts"]:
            if item["id"] in ("cryptography", "lz4", "cffi", "pycparser", "zstandard"):
                base = output / item["id"]
                for entry in base.iterdir():
                    if entry.name.endswith(".pth") or (packages / entry.name).exists():
                        raise ValueError("Unsupported/colliding wheel layout")
                    entry.rename(packages / entry.name)
        (python / "python313._pth").write_text(
            "python313.zip\n.\nLib/site-packages\n" + str(source / "tools") + "\nimport site\n", encoding="utf-8")
        phase("imports")
        validate(output)
        phase("imports_done")
        phase("verify_new",files=0,finished=False)
        files = tool_hashes(output,lambda count,done:phase("verify_new",files=count,finished=done))
        if cancelled():
            raise InterruptedError("Private tool setup cancelled before publication")
        phase("finalize")
        first_run.atomic_json(attempt / "complete.json",
                             {"owner": OWNER, "key": key, "files": files, "abi": value["abi"]})
        if final.exists():
            final.rename(root / (final.name + ".invalid-" + uuid.uuid4().hex[:8]))
        attempt.rename(final)
        phase("installed_ready")
        return final


def paths(directory, value=None):
    value = value or manifest()
    # CMake compiler paths and child PATH entries must remain absolute when
    # callers supply a relative --managed-toolchain from the checkout.
    root = plain_path(directory) / "output"
    by_id = {item["id"]: root / item["id"] / item["archive_root"] for item in value["artifacts"]}
    return {"python": by_id["python"] / "python.exe",
            "cc": by_id["llvm"] / "bin/x86_64-w64-mingw32-clang.exe",
            "cxx": by_id["llvm"] / "bin/x86_64-w64-mingw32-clang++.exe",
            "cmake": by_id["cmake"] / "bin/cmake.exe", "ninja": by_id["ninja"] / "ninja.exe"}

def installed(data, source, value=None, progress=None):
    value = value or manifest()
    data = plain_path(data)
    key = identity(value, source)
    directory = owned_path(data / "managed-tools" / key[:24], data)
    if not complete(directory, key,progress):
        raise ValueError("Private tools are missing or changed. Choose Install private tools, then recheck.")
    return directory


def prerequisites(source, directory):
    """Exact tool/package identity for the existing first_run build cache key."""
    source, directory = Path(source).resolve(), Path(directory).resolve()
    value = manifest()
    key = identity(value, source)
    def checked(count,done):
        first_run.log(f"private-tools: Checking private tool files: {count} files hashed" + (" (scan complete)" if done else ""))
    if not complete(directory, key,checked):
        raise ValueError("Private tool certificate changed. Choose Install private tools.")
    import importlib.metadata
    versions = {name:importlib.metadata.version(name) for name in ("cryptography","lz4","cffi","pycparser","zstandard")}
    for item in value["artifacts"]:
        if item["id"] in versions and versions[item["id"]] != item["version"]:
            raise ValueError("Private Python package identity differs from the pinned wheels")
    for relative in ("SDL/CMakeLists.txt","imgui/imgui.cpp","vulkan-headers/include/vulkan/vulkan.h",
                     "spirv-headers/include/spirv/unified1/spirv.hpp"):
        if not (source/"third_party"/relative).is_file():
            raise ValueError("Missing pinned third-party sources; extract the complete release kit")
    selected = paths(directory, value)
    env = environment(directory, value)
    for name in ("cc","cmake","ninja"):
        result = subprocess.run([str(selected[name]),"--version"],env=env,check=True,capture_output=True,text=True)
        versions[name] = {"path":str(selected[name]),"version":result.stdout.strip(),
                          "sha256":first_run.file_hash(selected[name])}
    versions.update(kind=value["abi"],managed_directory=str(directory),installation_key=key,
                    tool_certificate_sha256=first_run.file_hash(directory/"complete.json"),python=sys.version)
    return versions


def game_image(path):
    """Static private ABI requires OS-only imports; never depend on tool PATH."""
    import pe_dependencies
    record = pe_dependencies.inspect(path)
    unknown = [name for name in record["imports"] + record["delay_imports"]
               if not name.startswith(("api-ms-win-", "ext-ms-win-"))
               and name.removesuffix(".dll") not in pe_dependencies.OS_DLLS]
    if unknown or record["stack_reserve"] != 16777216 or record["dynamic_base"]:
        raise ValueError("Private game image failed dependency/stack/ASLR certification: " + ", ".join(unknown))
    return record


def environment(directory, value=None, temporary=None):
    selected = paths(directory, value)
    directory = Path(directory).absolute()
    temporary_root = directory.parent.parent if temporary is not None else directory
    temporary = Path(temporary).absolute() if temporary else directory / "tmp"
    # A completed toolset and its build outputs share the chosen private Data
    # root. No compiler or test temporary files may escape that ownership.
    owned_path(temporary, temporary_root)
    filesystem_path(temporary).mkdir(parents=True, exist_ok=True)
    env = {key: val for key, val in os.environ.items()
           if key.upper() not in {"INCLUDE", "LIB", "LIBPATH", "CL", "_CL_", "LINK", "_LINK_",
                                  "CC", "CXX", "CFLAGS", "CXXFLAGS", "LDFLAGS", "CMAKE_PREFIX_PATH",
                                  "RC", "AR", "RANLIB", "LD", "CMAKE_TOOLCHAIN_FILE",
                                  "CMAKE_GENERATOR", "CMAKE_MAKE_PROGRAM",
                                  "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH",
                                  "OBJCPLUS_INCLUDE_PATH", "CCC_OVERRIDE_OPTIONS", "LIBRARY_PATH",
                                  "COMPILER_PATH", "GCC_EXEC_PREFIX",
                                  "PYTHONPATH", "PYTHONHOME", "HW_VCVARS", "VCTOOLSINSTALLDIR",
                                  "WINDOWSSDKDIR", "VSINSTALLDIR", "VCINSTALLDIR"}}
    windows = Path(os.environ.get("SystemRoot", r"C:\Windows"))
    env["PATH"] = os.pathsep.join(map(str, [selected["cc"].parent, selected["cmake"].parent,
                                           selected["ninja"].parent, windows / "System32", windows]))
    env.update({name:str(temporary) for name in ("TEMP","TMP","TMPDIR")})
    return env


def phase_message(kind, **fields):
    if kind in ("verify_existing","verify_new"):
        return f"Checking private tool files: {fields['files']} files hashed" + (" (scan complete)" if fields['finished'] else "")
    if kind=="extract":
        return f"Extracting {fields['artifact']}: {fields['entries']} entries processed" + (" (complete)" if fields['finished'] else "")
    if kind in ("archive_check","download_checksum"):
        return "Checking archive checksum: " + fields["artifact"]
    return {"checking_installation":"Checking for an existing private tool installation",
            "packages":"Preparing private Python packages and preserving license notices",
            "imports":"Checking private Python, AES and LZ4 imports",
            "imports_done":"Private Python import checks passed",
            "finalize":"Finalizing the verified private tool installation",
            "installed_ready":"Private tools verified and ready"}[kind]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", required=True, type=Path)
    parser.add_argument("--source", default=MANIFEST.parent.parent, type=Path)
    parser.add_argument("--install", action="store_true", help="explicit consent to pinned private downloads")
    parser.add_argument("--bridge-action", choices=("check","prepare","launch"))
    parser.add_argument("--nonce",default="")
    parser.add_argument("--nsp")
    parser.add_argument("--keys")
    parser.add_argument("--update")
    args = parser.parse_args()
    value = manifest()
    if args.nonce and not re.fullmatch("[0-9a-f]{32}",args.nonce):
        parser.error("Invalid setup session")
    def event(kind,message,**fields):
        prefix="HWDER_UI:"+args.nonce+":" if args.nonce else ""
        print(prefix+json.dumps({"type":kind,"message":message,**fields},ensure_ascii=False),flush=True)
    def phase(kind,**fields):
        event("stage",phase_message(kind,**fields),stage="tools",phase=kind,**fields)
    if args.bridge_action:
        try:
            event("stage","Verifying private tool files (every file is checked)",stage="tools")
            directory=installed(args.data,args.source,value,
                                progress=lambda count,done:phase("verify_existing",files=count,finished=done))
            command=[str(paths(directory,value)["python"]),"-B","-X","utf8","-u",
                     str(args.source/"tools/setup_bridge.py"),"--action",args.bridge_action,
                     "--data",str(args.data),"--managed-toolchain",str(directory),"--ui-protocol",
                     "--nonce",args.nonce]
            if args.nsp:
                command+=["--nsp",args.nsp]
            if args.keys:
                command+=["--keys",args.keys]
            if args.update:
                command+=["--update",args.update]
            return subprocess.run(command,env=environment(directory,value)).returncode
        except (ValueError,OSError,subprocess.SubprocessError) as error:
            event("log",str(error))
            event("failed","Private tools are not installed for this kit yet. Choose Install (about 260 MB, no administrator access).",next="install")
            return 1
    if not args.install:
        print(json.dumps({"manifest_sha256": first_run.file_hash(MANIFEST), "downloads": value["artifacts"]}, indent=2))
        return
    result = install(args.data, args.source, value,
                     progress=lambda name, got, total:event("stage",f"Downloading {name}: {got//1048576} MiB received",stage="tools"),
                     phase=phase)
    print("tools: verified " + str(result), flush=True)
    event("components_ready","Private tools installed and verified. Choose Prepare next.")


if __name__ == "__main__":
    sys.exit(main() or 0)
