# Building

The build needs your own ROM of the game (US version 1.1, see README.md). Everything derived from it (the
recompiled code, the translated audio microcode) is generated locally and is never committed.

Currently some inputs live in the companion analysis project (`swrs`): the N64Recomp config `recomp\swrs.toml`
with its symbol file `build\swrs.syms.toml` (made by `tools\make_symbols.py`), the RSPRecomp config
`recomp\aspF5.toml` and the build script `tools\build_runtime.ps1`.

## Requirements (Windows)
- Visual Studio 2022 with the C++ workload and the "C++ Clang Compiler for Windows" component (clang-cl).
- CMake and Ninja (both come with Visual Studio).
- Python 3.
- N64Recomp and RSPRecomp, built from https://github.com/N64Recomp/N64Recomp.

## Steps
1. Clone this repository with its submodules (`git clone --recurse-submodules`). `lib/rt64` and
   `lib/N64ModernRuntime` come from forks with this port's branches (`swrs-mit` and `swrs`).
2. Recompile the game code: run N64Recomp with `recomp\swrs.toml` (it reads the ROM and the symbol file) and put
   the output in `RecompiledFuncs\`.
3. Translate the audio microcode: run RSPRecomp with `recomp\aspF5.toml`, which writes `rsp\aspF5.cpp`.
4. Configure and build with clang-cl and Ninja (RelWithDebInfo), e.g. with `tools\build_runtime.ps1 -Configure`
   from the `swrs` project, or:
   ```
   cmake -S . -B ..\swrs-recomp-build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
   cmake --build ..\swrs-recomp-build
   ```
   from a Visual Studio x64 developer prompt with VS's LLVM `bin` folder at the front of `PATH`.
5. Run `RogueSquadronRecompiled.exe` from the build folder and select your ROM in the launcher.
