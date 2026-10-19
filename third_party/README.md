# Third-party code

Git submodules (`git submodule update --init --recursive`):

| Path | Source | License |
|---|---|---|
| SDL | https://github.com/libsdl-org/SDL (tag release-3.2.30; host gamepads, gyro, rumble) | Zlib |
| imgui | https://github.com/ocornut/imgui | MIT |
| spirv-headers | https://github.com/KhronosGroup/SPIRV-Headers | MIT |
| vulkan-headers | https://github.com/KhronosGroup/Vulkan-Headers | Apache-2.0 / MIT |

`renderdoc/renderdoc_app.h` is a vendored RenderDoc API header under the MIT license
(copyright Baldur Karlsson; full notice embedded in the header). The header is required to
compile the optional capture integration. RenderDoc's DLL/capture programs are optional
developer tools and are not bundled in the source/setup kit.

The HWDER-specific code includes the recompiler, the kernel and service layer, the
Maxwell command processor, the SASS to SPIR-V shader translator, the Vulkan renderer, the
texture cache, the audio renderer and the video path.
