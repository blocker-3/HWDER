#!/bin/sh
# Builds the prebuilt, game-free parts of the Linux release on Ubuntu 22.04 (glibc 2.35):
#   static SDL3 (from third_party/SDL), FFmpeg's libavcodec/libavutil with only the H.264 decoder (LGPL, no GPL
#   parts), OpenSSL's libcrypto (AES and SHA-256 for the game's RomFS and pipeline catalog).
# The player's machine links these into hwder with the pinned Zig toolchain, so the game needs only glibc and the
# Vulkan loader from the system (SDL loads X11/Wayland/audio libraries at run time when they are there).
#
#   sh tools/linux/build_deps.sh <out-dir> [<download-dir>]
#
# Needs the packages in tools/linux/build-packages.txt. Sources are downloaded once and checked by SHA-256.
set -eu

OUT=$(realpath -m "${1:?usage: build_deps.sh <out-dir> [<download-dir>]}")
DL=$(realpath -m "${2:-$OUT.downloads}")
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
JOBS=${JOBS:-$(nproc)}

FFMPEG_VERSION=8.0.3
FFMPEG_SHA256=6136812ea6d4e68bdba27e33c2a94382711cdf4f8602ffef056ff792bd6f9818
FFMPEG_URL=https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz
OPENSSL_VERSION=3.5.9
OPENSSL_SHA256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
OPENSSL_URL=https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz

fetch() {  # url sha256 -> path
    file="$DL/$(basename "$1")"
    if [ ! -f "$file" ] || ! echo "$2  $file" | sha256sum -c --status; then
        mkdir -p "$DL"
        curl -sSfL --retry 5 --retry-all-errors --retry-delay 10 -o "$file.part" "$1"
        echo "$2  $file.part" | sha256sum -c --status || { echo "SHA-256 mismatch: $1" >&2; exit 1; }
        mv "$file.part" "$file"
    fi
    echo "$file"
}

[ -f "$ROOT/third_party/SDL/CMakeLists.txt" ] || { echo "third_party/SDL is missing (git submodule update --init)" >&2; exit 1; }
WORK="$OUT.work"
rm -rf "$OUT" "$WORK"
mkdir -p "$OUT/lib" "$OUT/include" "$OUT/licenses" "$OUT/sources" "$WORK"
# Hidden visibility: hwder is linked with -rdynamic (named crash backtraces), and these libraries' symbols must not
# be exported from it, or Vulkan layers loaded into the game with their own copies would bind to ours.
export CFLAGS="-O2 -fPIC -fvisibility=hidden" CXXFLAGS="-O2 -fPIC -fvisibility=hidden"

echo "== SDL3 (static; X11, Wayland, PipeWire/PulseAudio/ALSA and udev are loaded at run time)"
CFLAGS="$CFLAGS -DSDL_DECLSPEC=" cmake -S "$ROOT/third_party/SDL" -B "$WORK/sdl" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$OUT" \
    -DCMAKE_INSTALL_LIBDIR=lib -DSDL_STATIC=ON -DSDL_SHARED=OFF -DSDL_STATIC_PIC=ON -DSDL_TEST_LIBRARY=OFF \
    -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_DEPS_SHARED=ON -DSDL_GPU=OFF -DSDL_CAMERA=OFF -DSDL_KMSDRM=OFF \
    -DSDL_VULKAN=ON -DSDL_RENDER=ON -DSDL_OPENGL=ON -DSDL_OPENGLES=ON -DSDL_X11=ON -DSDL_WAYLAND=ON \
    -DSDL_WAYLAND_LIBDECOR=ON -DSDL_PIPEWIRE=ON -DSDL_PULSEAUDIO=ON -DSDL_ALSA=ON -DSDL_JACK=OFF -DSDL_SNDIO=OFF \
    -DSDL_HIDAPI=ON -DSDL_HIDAPI_LIBUSB=OFF -DSDL_LIBUDEV=ON -DSDL_DBUS=ON -DSDL_IBUS=OFF -DSDL_LIBURING=OFF >"$WORK/sdl-configure.log"
cmake --build "$WORK/sdl" -j "$JOBS" >"$WORK/sdl-build.log"
cmake --install "$WORK/sdl" >/dev/null
cp "$ROOT/third_party/SDL/LICENSE.txt" "$OUT/licenses/SDL-LICENSE.txt"

echo "== FFmpeg $FFMPEG_VERSION (libavcodec + libavutil, H.264 decoder only, LGPL)"
src=$(fetch "$FFMPEG_URL" "$FFMPEG_SHA256")  # a failed fetch stops here (set -e), not in tar
tar -xJf "$src" -C "$WORK"
FFMPEG_CONFIGURE="--prefix=$OUT --libdir=$OUT/lib --enable-static --disable-shared --enable-pic --disable-programs \
--disable-doc --disable-everything --enable-decoder=h264 --disable-avformat --disable-avdevice --disable-avfilter \
--disable-swscale --disable-swresample --disable-network --disable-autodetect --disable-iconv --disable-debug \
--extra-cflags=-fvisibility=hidden"
(cd "$WORK/ffmpeg-$FFMPEG_VERSION" && ./configure $FFMPEG_CONFIGURE >"$WORK/ffmpeg-configure.log" \
    && make -j "$JOBS" >"$WORK/ffmpeg-build.log" && make install >/dev/null)
grep -q "^#define CONFIG_GPL 0" "$WORK/ffmpeg-$FFMPEG_VERSION/config.h" || { echo "FFmpeg build is not LGPL-only" >&2; exit 1; }
cp "$WORK/ffmpeg-$FFMPEG_VERSION/COPYING.LGPLv2.1" "$OUT/licenses/FFmpeg-COPYING.LGPLv2.1"
cp "$WORK/ffmpeg-$FFMPEG_VERSION/LICENSE.md" "$OUT/licenses/FFmpeg-LICENSE.md"
# LGPL: the exact source of the linked FFmpeg ships with the release.
cp "$DL/ffmpeg-$FFMPEG_VERSION.tar.xz" "$OUT/sources/"

echo "== OpenSSL $OPENSSL_VERSION (libcrypto only, static)"
src=$(fetch "$OPENSSL_URL" "$OPENSSL_SHA256")
tar -xzf "$src" -C "$WORK"
OPENSSL_CONFIGURE="linux-x86_64 --prefix=$OUT --libdir=lib --openssldir=/etc/ssl no-shared no-dso no-engine \
no-module no-tests no-apps no-docs no-legacy no-ui-console -fPIC -fvisibility=hidden"
(cd "$WORK/openssl-$OPENSSL_VERSION" && ./Configure $OPENSSL_CONFIGURE >"$WORK/openssl-configure.log" \
    && make -j "$JOBS" build_libs >"$WORK/openssl-build.log" && make install_dev >/dev/null)
rm -f "$OUT/lib/libssl.a"
rm -rf "$OUT/lib/pkgconfig" "$OUT/lib/cmake/OpenSSL"
cp "$WORK/openssl-$OPENSSL_VERSION/LICENSE.txt" "$OUT/licenses/OpenSSL-LICENSE.txt"

for lib in libSDL3.a libavcodec.a libavutil.a libcrypto.a; do
    [ -f "$OUT/lib/$lib" ] || { echo "missing $lib" >&2; exit 1; }
done
rm -rf "$OUT/share" "$OUT/lib/pkgconfig"
GLIBC=$(ldd --version | head -1 | sed 's/.* //')
SDL_COMMIT=$(git -C "$ROOT/third_party/SDL" rev-parse HEAD 2>/dev/null || echo "source kit")
cat >"$OUT/BUILD-INFO.json" <<EOF
{
  "schema": 1,
  "glibc": "$GLIBC",
  "compiler": "$(cc --version | head -1)",
  "sdl": "$(sed -n 's/^#define SDL_\(MAJOR\|MINOR\|MICRO\)_VERSION *\([0-9]*\).*/\2/p' "$OUT/include/SDL3/SDL_version.h" | paste -sd.) ($SDL_COMMIT)",
  "ffmpeg": "$FFMPEG_VERSION",
  "ffmpeg_sha256": "$FFMPEG_SHA256",
  "ffmpeg_configure": "$(echo "$FFMPEG_CONFIGURE" | sed "s|$OUT|<prefix>|g")",
  "openssl": "$OPENSSL_VERSION",
  "openssl_sha256": "$OPENSSL_SHA256",
  "openssl_configure": "$(echo "$OPENSSL_CONFIGURE" | sed "s|$OUT|<prefix>|g")"
}
EOF
cat >"$OUT/licenses/README.txt" <<EOF
Prebuilt libraries linked into hwder on your machine (tools/linux/build_deps.sh, details in ../BUILD-INFO.json):
- SDL $(sed -n 's/"sdl": "\(.*\)",/\1/p' "$OUT/BUILD-INFO.json" | tr -d ' ') - zlib license (SDL-LICENSE.txt).
- FFmpeg $FFMPEG_VERSION libavcodec/libavutil, H.264 decoder only, LGPL 2.1 or later (FFmpeg-COPYING.LGPLv2.1).
  Its unmodified source is ../sources/ffmpeg-$FFMPEG_VERSION.tar.xz; Setup links the game from these object
  archives on your machine, so you can rebuild FFmpeg from that source and relink.
- OpenSSL $OPENSSL_VERSION libcrypto - Apache License 2.0 (OpenSSL-LICENSE.txt).
EOF
rm -rf "$WORK"
echo "deps ready: $OUT"
ls -la "$OUT/lib"
