"""Validate and extract only known ExeFS modules from a user's NSP. Never writes keys."""
import hashlib
import json
import struct
from pathlib import Path

MODULES = ("rtld", "main", "subsdk0", "subsdk1", "sdk")
TITLE_ID = 0x0100AE00096EA000
# The Japanese release has its own title. It is read like the international one; it plays only if its modules
# match a profile in supported_games.json, which nobody has checked yet (GitHub issue #3).
TITLE_ID_JP = 0x01002AB007FD2000
TITLE_IDS = (TITLE_ID, TITLE_ID_JP)
MANIFEST = Path(__file__).resolve().parent / "supported_games.json"


def bounded_pfs(read, size):
    """PFS parser with bounded tables, files and leaf-only names."""
    hdr = read(0, 16)
    if len(hdr) != 16:
        raise ValueError("truncated PFS0 header")
    magic, count, strings, _ = struct.unpack("<4sIII", hdr)
    base = 16 + count * 24 + strings
    if magic != b"PFS0" or count > 10000 or base > size or strings > 16 * 1024 * 1024:
        raise ValueError("invalid PFS0 table")
    entries, names = read(16, count * 24), read(16 + count * 24, strings)
    if len(entries) != count * 24 or len(names) != strings:
        raise ValueError("truncated PFS0 table")
    files = {}
    for i in range(count):
        off, length, name_off, _ = struct.unpack_from("<QQII", entries, 24 * i)
        end = names.find(b"\0", name_off)
        if name_off >= strings or end < 0 or base + off + length > size:
            raise ValueError("PFS0 entry outside container")
        name = names[name_off:end].decode("utf-8")
        if not name or name in (".", "..") or any(c in name for c in "/\\:") or name in files:
            raise ValueError("unsafe or duplicate PFS0 filename")
        files[name] = (base + off, length)
    return files


def program_exefs(program):
    # A program NCA may also contain a separate logo PFS0 section.
    candidates = []
    for sec in program.sections:
        if sec.fs_type != 1:
            continue
        doff, size = sec.data_region()
        if doff + size > sec.end - sec.start:
            raise ValueError("ExeFS outside NCA section")
        entries = bounded_pfs(lambda off, n: sec.read(doff + off, n), size)
        found = {name for name in entries if name in MODULES or name.startswith("subsdk")}
        if not found:
            continue
        if found != set(MODULES):
            raise ValueError("Unsupported ExeFS module set")
        candidates.append((sec, doff, entries))
    if len(candidates) != 1:
        raise ValueError("Expected exactly one program ExeFS containing the supported modules")
    return candidates[0]


def read_program(nsp, keys_path):
    from nx_extract import Nca, load_keys
    try:
        keys = load_keys(keys_path)
        if len(keys.get("header_key", b"")) != 32:
            raise ValueError()
    except (ValueError, UnicodeError):
        raise ValueError("Keys file is malformed or missing a 32-byte header_key") from None
    with Path(nsp).open("rb") as f:
        def raw(off, size):
            f.seek(off)
            return f.read(size)
        files = bounded_pfs(raw, Path(nsp).stat().st_size)
        title_keys = {}
        for name, (off, size) in files.items():
            if not name.endswith(".tik"):
                continue
            if size > 1024 * 1024:
                raise ValueError("oversized ticket")
            tik = raw(off, size)
            sig = struct.unpack_from("<I", tik)[0]
            body = {0x10000: 0x240, 0x10001: 0x140, 0x10002: 0x80,
                    0x10003: 0x240, 0x10004: 0x140, 0x10005: 0x80}.get(sig)
            if body is None or len(tik) < body + 0x170:
                raise ValueError("invalid ticket")
            title_keys[tik[body + 0x160:body + 0x170]] = tik[body + 0x40:body + 0x50]
        programs = []
        for name, (off, size) in files.items():
            if not name.endswith(".nca") or name.endswith(".cnmt.nca"):
                continue
            if size < 0xC00:
                raise ValueError("Truncated NCA header")
            try:
                nca = Nca(f, off, size, keys, title_keys)
            except KeyError:
                # Key names are public identifiers; never interpolate input key values.
                raise ValueError("Required decryption key is missing; supply a complete own prod.keys and ticket") from None
            except ValueError:
                raise ValueError("Cannot decrypt NCA header; check your own dump and keys") from None
            except (IndexError, struct.error):
                raise ValueError("Malformed or unsupported NCA metadata") from None
            if nca.content_type == 0:
                if any(s.start < 0xC00 or s.end > size or s.end <= s.start for s in nca.sections):
                    raise ValueError("NCA section outside container")
                programs.append(nca)
        if len(programs) != 1 or programs[0].title_id not in TITLE_IDS:
            raise ValueError("Expected exactly one base-game program for title 0100AE00096EA000 or 01002AB007FD2000; "
                             "updates/bundles unsupported")
        program = programs[0]
        sec, doff, entries = program_exefs(program)
        modules = {}
        for name in MODULES:
            off, size = entries[name]
            if size < 0x100 or size > 128 * 1024 * 1024:
                raise ValueError("Invalid NSO size")
            data = sec.read(doff + off, size)
            if len(data) != size or data[:4] != b"NSO0":
                raise ValueError("Invalid NSO module; check dump and keys")
            modules[name] = data
        if program.title_id == TITLE_ID_JP:
            try:
                supported_profile(modules, MANIFEST)
            except ValueError:
                raise ValueError("Japanese version (title 01002AB007FD2000) is not supported yet: "
                                 + main_identity(modules) + " does not match a tested version. No code generated") from None
        return modules


def main_identity(modules):
    """Build ID and SHA-256 of main: enough to tell game builds apart in a report, and safe to share."""
    main = modules["main"]
    return f"main build ID {main[0x40:0x50].hex()}, SHA-256 {hashlib.sha256(main).hexdigest()}"


def supported_profile(modules, manifest):
    hashes = {name: hashlib.sha256(modules[name]).hexdigest() for name in MODULES}
    titles = {f"{t:016x}" for t in TITLE_IDS}
    for profile in json.loads(Path(manifest).read_text(encoding="utf-8"))["profiles"]:
        if profile["title_id"] in titles and profile["modules_sha256"] == hashes:
            return profile["id"]
    raise ValueError("Unsupported game version: ExeFS SHA-256 does not match the tested base game ("
                     + main_identity(modules) + "). No code generated")


def profile(profile_id, manifest):
    """The profile entry: its optional "recomp" names the symbol and extra-entry files of that version."""
    for entry in json.loads(Path(manifest).read_text(encoding="utf-8"))["profiles"]:
        if entry["id"] == profile_id:
            return entry
    raise ValueError("Unknown game profile " + profile_id)
