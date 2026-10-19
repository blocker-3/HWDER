# Third-party code

Git submodules (`git submodule update --init --recursive`):

| Path | Source | License |
|---|---|---|
| SDL | https://github.com/libsdl-org/SDL (tag release-3.2.30; host gamepads, gyro, rumble; Linux windows/audio/dialogs) | Zlib |
| imgui | https://github.com/ocornut/imgui | MIT |
| spirv-headers | https://github.com/KhronosGroup/SPIRV-Headers | MIT |
| vulkan-headers | https://github.com/KhronosGroup/Vulkan-Headers | Apache-2.0 / MIT |

The HWDER-specific code includes the recompiler, the kernel and service layer, the
Maxwell command processor, the SASS to SPIR-V shader translator, the Vulkan renderer, the
texture cache, the audio renderer and the video path.

Native Linux links system shared OpenSSL 3 (libcrypto, Apache-2.0) and FFmpeg
libavcodec/libavutil for its native H.264 decoder. These libraries and headers are
not bundled in the source/setup kit. Use a compatible LGPL FFmpeg build without
--enable-gpl or --enable-nonfree; the native decoder alone does not determine a
distribution build's license. Preserve library notices and satisfy their
license requirements for any future binary distribution. No binary distribution
is produced by this port. See [OpenSSL licensing](https://www.openssl.org/source/license.html)
and [FFmpeg legal guidance](https://ffmpeg.org/legal.html).
