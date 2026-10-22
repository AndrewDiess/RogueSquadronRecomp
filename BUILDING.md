# Building

You need your own ROM of the game: Star Wars: Rogue Squadron (USA) (Rev 1), 16 MB, big-endian `.z64`, SHA-1
`c12e1c6fb47a67da463c50b705ec682ec24ae80d`. Everything derived from it (the recompiled code, the translated audio
microcode) is generated locally and is never committed.

## Requirements (Windows)
- Visual Studio 2022 with the C++ workload and the "C++ Clang Compiler for Windows" component (clang-cl).
- CMake and Ninja (both come with Visual Studio).
- N64Recomp and RSPRecomp, built from https://github.com/N64Recomp/N64Recomp (this release was made with commit
  `ffb39cd`).

## Steps
1. Clone this repository with its submodules:
   ```
   git clone --recurse-submodules https://github.com/AndrewDiess/RogueSquadronRecomp.git
   ```
   `lib/rt64` and `lib/N64ModernRuntime` come from forks with this port's branches (`swrs-mit` and `swrs`).
2. Save your ROM as `baserom.z64` in the repository root.
3. Recompile the game code from the repository root: `N64Recomp recomp\swrs.toml`. This writes `RecompiledFuncs\`.
4. Translate the audio microcode: `RSPRecomp recomp\aspF5.toml`. This writes `rsp\aspF5.cpp`.
5. From a Visual Studio x64 developer prompt, with VS's LLVM folder
   (`...\VC\Tools\Llvm\x64\bin`) at the front of `PATH`:
   ```
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
   cmake --build build
   ```
6. Run `build\RogueSquadronRecompiled.exe` and select your ROM in the launcher.

The `recomp\` folder holds the inputs: `swrs.toml` (N64Recomp config, including a few instruction patches and
hooks explained in its comments), `swrs.syms.toml` (function addresses, sizes and names) and `aspF5.toml`
(RSPRecomp config for the audio microcode).
