"""Read PE import names and stack/ASLR metadata without running the image."""
import struct
from pathlib import Path

OS_DLLS = set(("advapi32 bcrypt cabinet cfgmgr32 combase comctl32 comdlg32 crypt32 cryptbase "
              "cryptsp dbghelp dnsapi dwmapi dxgi gdi32 imm32 iphlpapi kernel32 kernelbase mpr "
              "msimg32 msvcrt ncrypt ntdll ole32 oleaut32 powrprof propsys psapi rpcrt4 secur32 "
              "setupapi shell32 shlwapi synchronization ucrtbase user32 userenv usp10 version "
              "winhttp wininet winmm winspool wintrust ws2_32 wsock32 wtsapi32 mfplat bcryptprimitives").split())
# BCryptPrimitives is Windows8+ ProcessPrng's OS DLL, not a VC redistributable:
# https://learn.microsoft.com/en-us/windows/win32/seccng/processprng


def closure(images, executable_directory):
    """Only app-local files or named Windows10+ OS imports; never host redists."""
    pending = list(map(Path, images))
    result={}
    while pending:
        path=pending.pop()
        key=str(path.resolve())
        if key in result:
            continue
        record=inspect(path)
        record["private_dependencies"]={}
        record["unresolved"]=[]
        for name in record["imports"]+record["delay_imports"]:
            if name.startswith(("api-ms-win-", "ext-ms-win-")) or name.removesuffix(".dll") in OS_DLLS:
                continue
            candidates=[path.parent/name,Path(executable_directory)/name]
            dependency=next((p for p in candidates if p.is_file()),None)
            if dependency:
                record["private_dependencies"][name]=str(dependency.resolve())
                pending.append(dependency)
            else:
                record["unresolved"].append(name)
        result[key]=record
    return result


def inspect(path):
    data = Path(path).read_bytes()
    if data[:2] != b"MZ" or len(data) < 64:
        raise ValueError("Not a PE image")
    pe, = struct.unpack_from("<I", data, 0x3C)
    if data[pe:pe+4] != b"PE\0\0":
        raise ValueError("Invalid PE signature")
    machine, sections, _, _, _, optional_size, _ = struct.unpack_from("<HHIIIHH", data, pe+4)
    optional = pe+24
    magic, = struct.unpack_from("<H", data, optional)
    if magic != 0x20B or machine != 0x8664:
        raise ValueError("Expected Windows x64 PE32+")
    table = optional + optional_size
    regions=[]
    for i in range(sections):
        base = table + i*40
        virtual_size, rva, raw_size, raw = struct.unpack_from("<IIII", data, base+8)
        regions.append((rva, max(virtual_size, raw_size), raw, raw_size))
    def offset(rva):
        for start,size,raw,raw_size in regions:
            if start <= rva < start+size and rva-start < raw_size:
                return raw+rva-start
        raise ValueError("PE import address outside file-backed sections")
    def cstring(rva):
        start=offset(rva)
        end=data.index(b"\0", start, min(start+1024,len(data)))
        return data[start:end].decode("ascii").lower()
    imports=[]
    import_rva, import_size=struct.unpack_from("<II", data, optional+112+8)
    if import_rva:
        at=offset(import_rva)
        for i in range(min(import_size//20+1,4096)):
            descriptor=struct.unpack_from("<IIIII", data, at+i*20)
            if not any(descriptor):
                break
            imports.append(cstring(descriptor[3]))
        else:
            raise ValueError("Unterminated PE import table")
    delay_rva, delay_size=struct.unpack_from("<II", data, optional+112+13*8)
    delay=[]
    if delay_rva:
        at=offset(delay_rva)
        for i in range(min(delay_size//32+1,4096)):
            descriptor=struct.unpack_from("<IIIIIIII", data, at+i*32)
            if not any(descriptor):
                break
            if descriptor[0] != 1:
                raise ValueError("Unsupported absolute delay-import descriptor")
            delay.append(cstring(descriptor[1]))
        else:
            raise ValueError("Unterminated PE delay import table")
    characteristics, = struct.unpack_from("<H", data, optional+70)
    stack, = struct.unpack_from("<Q", data, optional+72)
    resource_types=[]
    resource_rva, _=struct.unpack_from("<II", data, optional+112+2*8)
    if resource_rva:
        at=offset(resource_rva)
        named, ids=struct.unpack_from("<HH", data, at+12)
        for i in range(min(named+ids,64)):
            name, _=struct.unpack_from("<II", data, at+16+i*8)
            resource_types.append(name if not name & 0x80000000 else -1)
    symbols, = struct.unpack_from("<I", data, pe+12)
    section_names=[]
    for i in range(sections):
        raw_name=data[table+i*40:table+i*40+8].rstrip(b"\0").decode("ascii", "replace")
        section_names.append(raw_name)
    return {"imports": sorted(set(imports)), "delay_imports":sorted(set(delay)),
            "stack_reserve":stack, "dynamic_base":bool(characteristics & 0x40), "machine":"amd64",
            "resource_types":sorted(resource_types), "sections":section_names, "symbol_table":bool(symbols)}


VERSION_STRINGS = {"CompanyName": "HWDER project", "ProductName": "HWDER", "FileDescription": "HWDER Setup",
                   "OriginalFilename": "HWDER Setup.exe"}


def setup_identity_problems(path):
    """What HWDER Setup.exe lacks of its icon, manifest, version information and stripped symbols."""
    record = inspect(path)
    data = Path(path).read_bytes()
    problems = [f"no {kind} resource" for kind, rt in (("icon", 14), ("version", 16), ("manifest", 24))
                if rt not in record["resource_types"]]
    for key, value in VERSION_STRINGS.items():
        if (key + "\0").encode("utf-16-le") not in data or (value + "\0").encode("utf-16-le") not in data:
            problems.append(f"version string {key} is not {value!r}")
    if b"asInvoker" not in data:
        problems.append("manifest lacks requestedExecutionLevel asInvoker")
    if record["symbol_table"] or any(n.startswith((".debug", "/")) for n in record["sections"]):
        problems.append("symbols or debug sections not stripped")
    return problems
