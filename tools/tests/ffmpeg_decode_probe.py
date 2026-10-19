"""Public native-FFmpeg decoder integration probe; no game inputs or encoder required.

Pass matching installed FFmpeg development headers and shared libraries, for example:
  python tools/tests/ffmpeg_decode_probe.py --avcodec /usr/lib/libavcodec.so \
      --avutil /usr/lib/libavutil.so
Windows also accepts --include-dir <FFmpeg include root>. Nothing is downloaded.
--generate-only writes the synthetic fixture/probe without compiling or loading libraries.

The exact AvDecoder class is extracted from production source so this exercises its
buffer ownership, tag mapping, NV12 conversion, reset and destruction rather than a
second implementation. Windows results validate the codec path, not Linux integration.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


class Bits:
    def __init__(self):
        self.bits = []

    def bit(self, value):
        self.bits.append(value & 1)

    def n(self, value, count):
        self.bits.extend((value >> i) & 1 for i in range(count - 1, -1, -1))

    def ue(self, value):
        value += 1
        count = value.bit_length()
        self.n(0, count - 1)
        self.n(value, count)

    def se(self, value):
        self.ue(-2 * value if value <= 0 else 2 * value - 1)

    def align(self):
        while len(self.bits) % 8:
            self.bit(0)

    def finish(self):
        self.bit(1)
        self.align()
        return bytes(sum(self.bits[j + i] << (7 - i) for i in range(8))
                     for j in range(0, len(self.bits), 8))


def nal(code, payload):
    data = bytearray(b"\0\0\0\1" + bytes([code]))
    zeros = 0
    for value in payload:
        if zeros >= 2 and value <= 3:
            data.append(3)
            zeros = 0
        data.append(value)
        zeros = zeros + 1 if value == 0 else 0
    return data


def headers(width=16, height=16):
    s = Bits()
    for value in (100, 0, 51):
        s.n(value, 8)
    for value in (0, 1, 0, 0):
        s.ue(value)
    s.bit(0)
    s.bit(0)
    for value in (0, 0, 0, 16):
        s.ue(value)
    s.bit(0)
    s.ue(width // 16 - 1)
    s.ue(height // 16 - 1)
    for value in (1, 1, 0, 1):
        s.bit(value)
    for _ in range(8):
        s.bit(0)
    s.bit(1)
    s.bit(1)
    # Match production SPS: max_num_reorder_frames=0, max_dec_frame_buffering=16.
    for value in (0, 0, 16, 16, 0, 16):
        s.ue(value)
    p = Bits()
    p.ue(0)
    p.ue(0)
    p.bit(0)
    p.bit(0)
    for _ in range(3):
        p.ue(0)
    p.bit(0)
    p.n(0, 2)
    for _ in range(3):
        p.se(0)
    p.bit(1)
    p.bit(0)
    p.bit(0)
    return nal(0x67, s.finish()) + nal(0x68, p.finish())


def picture(kind, frame_num, poc, value, skip=False, width=16, height=16):
    """I_PCM or skipped macroblocks with exact expected constant Y/U/V planes."""
    b = Bits()
    b.ue(0)
    b.ue({"I": 2, "P": 0, "B": 1}[kind])
    b.ue(0)
    b.n(frame_num, 4)
    if kind == "I":
        b.ue(0)
    b.n(poc, 4)
    if kind == "B":
        b.bit(1)
    if kind != "I":
        b.bit(0)
        b.bit(0)
        if kind == "B":
            b.bit(0)
    if kind == "I":
        b.bit(0)
        b.bit(0)
    elif kind == "P":
        b.bit(0)
    b.se(0)
    b.ue(1)
    macroblocks = width // 16 * (height // 16)
    if skip:
        assert kind != "I"
        b.ue(macroblocks)
    else:
        for _ in range(macroblocks):
            if kind != "I":
                b.ue(0)
            b.ue({"I": 25, "P": 30, "B": 48}[kind])
            b.align()
            for sample in [value] * 256 + [96] * 64 + [160] * 64:
                b.n(sample, 8)
    return headers(width, height) + nal({"I": 0x65, "P": 0x41, "B": 0x01}[kind], b.finish())


FUNCTIONS = ("avcodec_find_decoder_by_name", "avcodec_alloc_context3", "av_frame_alloc", "av_packet_alloc",
             "avcodec_open2", "avcodec_flush_buffers", "av_frame_unref", "av_packet_unref", "av_new_packet",
             "avcodec_send_packet", "avcodec_default_get_buffer2", "av_frame_ref", "av_packet_free",
             "av_frame_free", "avcodec_free_context", "av_strerror", "avcodec_receive_frame", "avcodec_version")


def generate(directory):
    source = (ROOT / "runtime/src/video/decoder.cpp").read_text(encoding="utf-8")
    source = source[source.index("class AvDecoder {"):source.index("using HostDecoder = AvDecoder;")]
    code = r'''
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <vector>
#undef assert
#define assert(value) do {if(!(value)){fprintf(stderr,"check failed at line %d: %s\n",__LINE__,#value);std::exit(1);}} while(0)
using u8=uint8_t;using u32=uint32_t;
struct Frame {u32 width=0,height=0;std::vector<u8> y,uv;};
static std::map<u32,std::shared_ptr<Frame>> frames;
void hw_log(const char*,...) {}
void put_frame(u32 tag,std::shared_ptr<Frame> frame) {frames[tag]=std::move(frame);}
'''
    for fn in FUNCTIONS:
        code += f"static decltype(&{fn}) p_{fn};\n#define {fn} p_{fn}\n"
    code += source + r'''
int main(int argc,char** argv) {
assert(argc==3);
#ifdef _WIN32
auto load=[](const char* path) {return LoadLibraryExW(std::filesystem::u8path(path).c_str(),nullptr,
    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);};
auto av=load(argv[1]),util=load(argv[2]);assert(av&&util);
auto symbol=[](HMODULE lib,const char* name) {return GetProcAddress(lib,name);};
#else
auto av=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL),util=dlopen(argv[2],RTLD_NOW|RTLD_LOCAL);assert(av&&util);
auto symbol=[](void* lib,const char* name) {return dlsym(lib,name);};
#endif
'''
    for fn in FUNCTIONS:
        lib = "util" if fn.startswith(("av_frame_", "av_strerror")) else "av"
        code += f'p_{fn}=reinterpret_cast<decltype(p_{fn})>(symbol({lib},"{fn}"));assert(p_{fn});\n'
    code += r'''
assert((avcodec_version()>>16)==LIBAVCODEC_VERSION_MAJOR);
unsigned checks=0,pictures=0;
for(unsigned instance=0;instance<3;++instance) {
AvDecoder decoder;
for(unsigned session=0;session<2;++session) {
decoder.restart();frames.clear();
'''
    cases = [("I", 0, 0, 32, False), ("P", 1, 4, 160, False), ("B", 2, 2, 80, False),
             ("P", 2, 8, 200, False), ("B", 3, 6, 120, False), ("P", 3, 12, 200, True),
             ("B", 4, 10, 200, True), ("P", 6, 14, 192, False)]
    for i, case in enumerate(cases):
        data = picture(*case)
        (directory / f"{i}-{case[0]}.h264").write_bytes(data)
        code += "{const std::vector<u8> packet={" + ",".join(map(str, data)) + "};\n"
        code += f"const u32 tag=0x123400+session*0x1000+{i}*0x100;decoder.decode(packet,tag);\n"
        code += "assert(frames.count(tag));auto f=frames.at(tag);assert(f->width==16&&f->height==16);\n"
        code += "assert(f->y.size()==256&&f->uv.size()==128);\n"
        code += f"for(u8 value:f->y){{assert(value=={case[3]});++checks;}}\n"
        code += "for(size_t x=0;x<f->uv.size();++x){assert(f->uv[x]==(x%2?160:96));++checks;}\n"
        code += f"assert(frames.size()=={i + 1});++pictures;}}\n"
    code += r'''
}
const auto prior=frames.size();
decoder.decode(std::vector<u8>{0,0,0,1,0x61},0x777);
assert(frames.size()==prior&&!frames.count(0x777));
decoder.restart();decoder.decode({},0x999);assert(!frames.count(0x999));
'''
    data = picture("I", 0, 0, 64, width=32, height=32)
    code += "{const std::vector<u8> packet={" + ",".join(map(str, data)) + "};\n"
    code += r'''
decoder.decode(packet,0x123400);auto f=frames.at(0x123400);
assert(f->width==32&&f->height==32&&f->y.size()==1024&&f->uv.size()==512);
for(u8 value:f->y){assert(value==64);++checks;}
for(size_t x=0;x<f->uv.size();++x){assert(f->uv[x]==(x%2?160:96));++checks;}++pictures;
}}
printf("PASS exact production AvDecoder: %u I/P/B pictures, %u pixel checks; reference gaps, inter predictions, resets, resolution changes, tag reuse, malformed/empty inputs and destruction\n",pictures,checks);
}
'''
    target = directory / "decoder_probe.cpp"
    target.write_text(code, encoding="utf-8")
    return target


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--avcodec", type=Path)
    ap.add_argument("--avutil", type=Path)
    ap.add_argument("--include-dir", type=Path)
    ap.add_argument("--build-dir", type=Path, default=ROOT / "build/media-probe/public")
    ap.add_argument("--generate-only", action="store_true")
    args = ap.parse_args()
    directory = args.build_dir.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    source = generate(directory)
    if args.generate_only:
        print(f"Public synthetic fixtures and production-class probe generated: {directory}")
        return
    if not args.avcodec or not args.avutil:
        ap.error("--avcodec and --avutil matching installed shared libraries are required")
    for path in (args.avcodec, args.avutil):
        if not path.is_file():
            ap.error(f"Shared library does not exist: {path}")
    env = dict(os.environ)
    if args.include_dir:
        if not (args.include_dir / "libavcodec/avcodec.h").is_file():
            ap.error("--include-dir must contain matching libavcodec/avcodec.h")
        if os.name == "nt":
            env["CL"] = f'/I"{args.include_dir.resolve()}" ' + env.get("CL", "")
        else:
            env["CPLUS_INCLUDE_PATH"] = str(args.include_dir.resolve()) + os.pathsep + env.get("CPLUS_INCLUDE_PATH", "")
    subprocess.run([sys.executable, str(ROOT / "tools/build.py"), "--only-file", str(source),
                    "--build-dir", str(directory)], env=env, check=True)
    sys.path.insert(0, str(ROOT / "tools"))
    import build
    from build_lock import build_lock
    executable = directory / ("decoder_probe.exe" if os.name == "nt" else "decoder_probe")
    with build_lock():
        tool_env, _, _, _, cxx = build.toolchain()
        if os.name == "nt":
            command = [cxx, "/nologo", str(directory / "probe.obj"), "/Fe" + str(executable)]
        else:
            command = [cxx, str(directory / "probe.o"), "-ldl", "-o", str(executable)]
        subprocess.run(command, env=tool_env, check=True)
    subprocess.run([str(executable), str(args.avcodec.resolve()), str(args.avutil.resolve())], check=True)


if __name__ == "__main__":
    main()
