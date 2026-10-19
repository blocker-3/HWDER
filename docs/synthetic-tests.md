# Public synthetic regression

Run `python tools/tests/run_synthetic.py --jobs 2` from a source checkout on
Windows x64 with Python 3.11+, clang-cl, the VS C++/Windows SDK workload, CMake
3.24+ and Ninja. Initialize only `third_party/spirv-headers` and
`third_party/vulkan-headers` at their committed revisions. The build goes through
`tools/build.py --synthetic-tests`, takes the common host build lock and uses
`build/synthetic`. It never reads `generated/`, game files, keys, saves or private
caches. Coordinate quiet profile windows before running it in a shared workspace.

The Windows workflow pins checkout/setup actions, Python and Python package wheel hashes;
the hosted Windows compiler/SDK image remains maintained by GitHub. It runs on
push, pull request and manual dispatch. It requests read-only repository access,
persists no checkout credentials, uses no secrets or private artifact/cache
downloads, and publishes no build or release artifacts. This candidate has only
been run locally; a hosted Actions run requires separately authorized publication.

Coverage:

- Existing warmup-cache tests exercise canonical codec round trips, malformed
  payloads, bounded parsing, exact shader identity, private-history retention,
  failed atomic replacement and preservation of previous cache bytes.
- Existing catalog tests exercise bounded metadata, incompatible identities,
  integrity failures, source resolution, path traversal rejection, shader
  preparation and atomic replacement using entirely synthetic fixtures.
- New recompiler tests compile 16 hand-encoded A64 arithmetic/variable-shift
  instructions using the real translator and CPU header. An independent Python
  integer model checks results, NZCV preservation/changes and source-register
  preservation over 8,896 edge/random execution cases. Fixtures are public ISA
  values rather than instructions sampled from a game dump.
- When the separately reviewed first-run candidate is integrated, its tracked
  `test_first_run.py` suite runs automatically. That suite has 23 synthetic tests,
  including cache invalidation/recovery, coordinator locking, bounded container
  parsing and source-package exclusions with per-file hashes. Its absence is
  reported as an explicit skip and workflow warning; partial integration fails.
  Install `python -m pip install --require-hashes --only-binary=:all: -r tools/requirements-ci.txt`
  in the pinned CPython 3.11 Windows x64 environment before running that optional
  suite. Other Python/platform versions need their own reviewed wheel hashes.
  Use `--python-only` to run it without C++
  compilation. No code is imported from another local branch or worktree.

Full game-derived differential tests (`tools/recomp/difftest.py`), runtime/GPU
behavior, whole-source kit production, private generation/build recovery with
real input, and Linux cache/catalog IO are outside these CI claims. In particular,
the legacy packager on main is never invoked or treated as a validated source kit.
