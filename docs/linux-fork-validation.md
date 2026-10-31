# Linux fork validation

The fork's starting point, `07a6d6f`, is identical to `blocker-3/HWDER` main
when inspected on 2026-10-31. It already includes the native Linux runtime,
SDL3 window/input/audio integration, POSIX host services, FFmpeg decoding,
OpenSSL crypto, Linux setup and release packaging. This branch validates and
repairs that implementation rather than replacing it with another port.

## Changes

- Include `<algorithm>` explicitly in `nvdrv.cpp`. Clang 23 with libstdc++ 16
  could not find the initializer-list overload of `std::max` through the
  previous transitive includes. This blocked both host contract compilation
  and the native runtime build.
- Add an Ubuntu 24.04 regression workflow that builds the complete host test
  aggregate, runs host and synthetic tests, and compiles and links the entire
  runtime with the existing public guest fixture. No game files or keys are
  required. The fixture executable must never be run or published.
- Keep the existing Windows implementation and workflows.

## Reproduce locally

Initialize the pinned submodules and install the native dependencies described
in [linux-native.md](linux-native.md). Then run:

```sh
python3 tools/build.py --host-tests --jobs 4 --build-dir build/host-linux
ctest --test-dir build/host-linux --output-on-failure --no-tests=error
python3 tools/tests/run_synthetic.py --jobs 4
python3 tools/tests/link_check_fixture.py --output build/link-check-generated
python3 tools/build.py --generated-dir build/link-check-generated --build-dir build/link-check --jobs 4
```

The last command is a compile/link check only. A playable build requires local
extraction and recompilation from the user's game dump and keys via
`tools/first_run.py`. Public fixture validation does not establish gameplay,
GPU rendering, controller operation, or release-tarball installation.

## Local results (2026-10-31)

Environment: EndeavourOS x86-64, Clang 23.1.4, libstdc++ 16, OpenSSL 3.6.5,
libavcodec 63.1.102 and libavutil 61.1.102. All four pinned public submodules
were initialized. The build lock used `XDG_STATE_HOME=/tmp/hwder-state`
because the validation environment restricts writes outside the workspace.

- Complete `host_tests` aggregate built; CTest: 7/7 passed.
- Public synthetic runner: 62 Python tests, four platform skips; CTest: 6/6 passed.
- Full runtime and pinned SDL compiled and linked with the public guest fixture;
  output inspected as an x86-64 Linux ELF, never executed.
- Exact production FFmpeg decoder probe passed: 51 I/P/B pictures and 23,040
  pixel checks, including reset, resize, malformed-input and destruction cases.
- Full Python discovery: 164 tests, six skips, one failure and one error, both in
  `test_managed_tools.Managed`: `test_relative_toolset_path_with_parent_steps_is_complete`
  and `test_update_in_a_new_kit_folder_reuses_tools_and_adopts_old_per_kit_install`.
  These concern the Windows private-tool certificate/relocation path and remain
  unresolved; the public regression runner above succeeds.
- Workflow YAML parses and `git diff --check` passes. The new Ubuntu workflow
  has not been executed as part of these local results.

No game dump, keys, generated game implementation or save was provided. Actual
first-run generation, gameplay, live graphics/audio/input and release packaging
were not validated. Windows was not built on this Linux host.

## Follow-up with local game inputs (2026-10-31)

A local base-game NSP and keys subsequently became available. Setup recognized
`hwde-0100ae00096ea000-tested-base`, generated all five modules (83,412 guest
functions), compiled all 751 build steps, and published a native Linux game
executable. No game-derived files or keys are committed.

On Omega, the GTX 1060's driver reports `shaderFloat16=false`. The renderer
unconditionally requests that feature, so device creation returns
`VK_ERROR_FEATURE_NOT_PRESENT` (-8). This is a remaining hardware compatibility
blocker for that GPU, despite its reported Vulkan 1.4 API version.

Selecting the Intel HD Graphics 530 through `VK_DRIVER_FILES` allowed startup.
The runtime opened PipeWire audio output and presented hundreds of frames,
with logged 300-frame windows averaging 16.67 ms and no reported fence timeouts
or skipped draws. This establishes execution and frame presentation, not visual
correctness, controller operation, battle performance or full gameplay validation.
The Intel workaround does not resolve the GTX 1060 compatibility issue.
