Metal Gear Solid for Mac (unofficial)
=====================================
Plays the GOG PC version of Metal Gear Solid and its VR Missions natively on a Mac: no Windows,
Wine, CrossOver or emulator. The game's Windows program is translated into native Apple Silicon
code on your own Mac, from your own copy of the game, and runs on a Mac replacement for the
Windows and DirectX parts it uses. 3D is drawn with Metal at your screen's full resolution.

This package contains NO game code or data. It holds the translator, the Mac runtime and a build
script; the script runs them on your copy of the game and puts two apps into your game folder.

**[Download MacOS Patch](https://github.com/PepeForFun/MGS1_MacOS_Patch/releases/download/Release/MGS_MacOS_Patch.zip)**

Requirements
------------
- A Mac with Apple Silicon (M1 or newer). Tested on macOS 27; macOS 14 or newer may work.
- Metal Gear Solid from GOG.com (the PC version: the game folder holds mgsi.exe and mgsvr.exe)
- Apple's free Command Line Tools (the build script offers to install them)
- About 1.5 GB of free space for the build, and a few minutes

Setup
-----
1. Get the game folder (the one with mgsi.exe in it):
   on the Mac: unpack the GOG installer with innoextract (https://constexpr.org/innoextract/,
   or "brew install innoextract" with Homebrew). In Terminal, in the folder with the installer:
         innoextract innoextract -d ~/<uuser>/<destination folder>" ~/<user>/<source folder>/"<installation package.exe"
2. Copy this folder ("Metal Gear Solid Mac Patch") into the game folder, next to mgsi.exe.
3. Double-click "Build.command" in it. The first time, macOS refuses to open it: close the
   message, open System Settings > Privacy & Security, scroll down and click "Open Anyway",
   then confirm. 
4. A Terminal window shows the build. When it says "Done", the game folder holds
   "Metal Gear Solid.app" and "Metal Gear Solid VR Missions.app", and a README for playing.
   The apps must stay in the game folder.

Playing
-------
- The first start asks you to accept the game's licence (EULA.RTF in the game folder).
- Keyboard: the game's own defaults (arrow keys move, Esc = Start, Space = confirm, ...);
  the full list is in "Metal Gear Solid for Mac - README.txt" in the game folder.
- Cmd+F switches full screen on and off; each app remembers the choice. Cmd+Q quits.
- A game controller connected before the game starts is offered to the game as a joystick (not tested).
- Saves go to the SavedGames folder in the game folder, settings to mgs.cfg and mgsvideo.cfg.

Known limits
------------
- Made and tested with the GOG release (mgsi.exe of 2,883,584 bytes). Other releases of the PC
  game are untested; the build script says so if your files differ.
- The first areas, codec calls, the pause menu and VR training missions have been played; a full
  play-through has not been finished on this version yet. 

How it works
------------
tools/recomp.py decodes the game's x86 program and writes it out as C (in build/, on your Mac);
runtime/ replaces the Windows, DirectDraw, Direct3D, DirectSound and DirectInput functions the
game calls with Cocoa, Metal, CoreAudio and GameController code; the Makefile compiles both into
one program and tools/make_app.sh wraps it into an app. Source revision: d3aec6a.

Legal
-----
Unofficial, non-commercial fan work. Not affiliated with or endorsed by Konami or GOG.
Metal Gear Solid is a trademark of Konami. No game content is included. The build folder, the
apps and any HD textures you make are derived from your copy of the game: keep them for your own
use and do not share them.
