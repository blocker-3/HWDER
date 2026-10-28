# Building from source

Most players should use Setup instead (see the [README](../README.md#quickstart)).

The automated Setup workflow is also the recommended source build. To prepare without launching:

```
python tools/first_run.py --nsp "<game>.nsp" --keys "<prod.keys>"
```

The manual developer flow remains available. It requires installed Python x64 with cryptography
and lz4, plus VS2022 Build Tools with MSVC x64, Clang, CMake/Ninja and a Windows10/11 SDK.
The managed wizard does not require those global developer installations.

```
git clone --recursive https://github.com/sonsegajp/HWDER
cd HWDER

# 1. extract your dump once for the recompiler (exefs/ is all it needs; romfs/ is optional,
#    the runtime can read it from the NSP directly)
python tools/nx_extract.py "<game>.nsp" "<prod.keys>" data --no-romfs

# 2. ARM64 -> C (all five ExeFS modules, ~5 minutes, writes generated/)
cd tools && python -m recomp.gen_main && cd ..

# 3. build (clang-cl + Ninja inside the VS developer environment)
python tools/build.py

# 4. run using your own NSP and keys (never copy keys into a release)
set HWDER_NSP=<game>.nsp
set HWDER_KEYS=<prod.keys>
build\release\hwder.exe

# package: source/setup kit only; no linked exe/PDB/generated C
python tools/make_dist.py
```

If the game ever jumps to an address the recompiler did not know was code, the runtime logs it,
appends it to `extra_entries.txt` and exits; `tools/iterate.sh` automates the
regenerate-rebuild-rerun loop (it collects them in `data/extra_entries.txt`, which the generator
merges with the tracked list in `tools/recomp/extra_entries.txt`). The tracked list already covers
normal play.

## Public synthetic tests

`python tools/tests/run_synthetic.py --jobs 2` runs the cache/catalog codecs and
compiled A64 arithmetic regressions without a game dump, generated game code,
keys, saves, Vulkan device or private cache. See [test setup and coverage](synthetic-tests.md).
Source-package exclusion tests run automatically when the separate first-run
candidate is integrated; their current absence is reported explicitly.

Debugging switches are listed in [Environment variables](environment-variables.md); how the recompiler and runtime fit together is in [How HWDER works](how-it-works.md).
