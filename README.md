# Star Wars: Rogue Squadron - Recompiled

A native PC port of *Star Wars: Rogue Squadron* (Nintendo 64, US version 1.1), made by statically recompiling the
game's code with [N64Recomp](https://github.com/N64Recomp/N64Recomp) and running it on
[N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime). Graphics are drawn by a fork of
[RT64](https://github.com/rt64/rt64) that adds support for the game's custom Factor 5 graphics microcode.

**This repository and its releases contain no game code or assets. You need your own copy of the game (a ROM
dumped from your cartridge) to build or play it.**

This project is not affiliated with or endorsed by Lucasfilm, LucasArts, Factor 5 or Nintendo. Star Wars and
Rogue Squadron are trademarks of their respective owners.

## Status
- The game boots and plays through the intro, menus, cutscenes and missions.
- Widescreen: the "Expand" aspect ratio widens in-flight gameplay with the HUD anchored to the screen edges; the
  intro, cutscenes and menus stay 4:3.
- Audio runs the game's own audio microcode, translated with RSPRecomp, including its GSM speech decoding.
- Rumble works on controllers that support it.

## Download
Windows builds are on the [Releases](https://github.com/AndrewDiess/RogueSquadronRecomp/releases) page. Extract the
zip, run `RogueSquadronRecompiled.exe` and select your ROM.

## Required game version
Star Wars: Rogue Squadron (USA) (Rev 1), 16 MB, SHA-1 `c12e1c6fb47a67da463c50b705ec682ec24ae80d`.

## How it was made
This project was developed with the help of an AI coding assistant (Claude, by Anthropic), with all changes
reviewed and tested by the maintainer.

The graphics support was written for this project:
- The game uses Factor 5's own graphics microcode ("F5Rogue"), which RT64 doesn't support. Its behavior was
  documented by reading the microcode itself, and the RT64 support was written from that description (MIT
  licensed, like RT64).
- It was checked against a reference renderer that runs the game's own microcode through RSPRecomp, comparing
  their output triangle by triangle.

## Building
See [BUILDING.md](BUILDING.md).

## Credits and licenses
- Based on [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp) (GPL-3.0): the launcher, menus,
  input, audio output and RT64 integration come from it.
- [N64Recomp](https://github.com/N64Recomp/N64Recomp) and [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime).
- [RT64](https://github.com/rt64/rt64) (MIT), used through a fork with the F5Rogue microcode support and some
  fixes.
- [RmlUi](https://github.com/mikke89/RmlUi), [lunasvg](https://github.com/sammycage/lunasvg),
  [FreeType](https://freetype.org/) and the other libraries under `lib/`, each under its own license.

This project as a whole is licensed under the GNU General Public License version 3, see [COPYING](COPYING).
