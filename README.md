# Snapshot

A troubleshooting snapshot for bug reports, by tonycubed2: hold L for 8 seconds in game and it writes a report of the game's state and copies the newest saves and all logs. This folder is the Skyrim SE/AE version (SKSE).

## Other games

| Folder | Game | Built with |
|---|---|---|
| [skyrimle/](skyrimle) | Skyrim Legendary Edition 1.9.32.0 (SKSE 1.7.3, 32-bit) | CMake; needs SKSE 1.7.3's source in `extern\skse` (not included here) |
| [fallout4/](fallout4) | Fallout 4 1.11.x (F4SE) | xmake; needs [CommonLibF4](https://github.com/libxse/commonlibf4) in `lib\commonlibf4` (not included here) |
| [starfield/](starfield) | Starfield 1.16.244 (SFSE) | xmake; needs [CommonLibSF](https://github.com/libxse/commonlibsf) in `lib\commonlibsf` (not included here) |

Each folder's README.txt lists what that game's report contains and anything it cannot include.

Troubleshooting snapshot for bug reports. Hold L for 8 seconds in game; when it is done a message box
says "Data Collected" and shows the folder it wrote.

Requires: SKSE, Address Library for SKSE Plugins. Skyrim SE 1.5.97 and AE.
Optional: SKSE Menu Framework (needed for the Progress Journal restore page).

## Progress Journal (new in 1.1.0)

A crash-safe running record of your progress. Every change is written to disk the moment it happens:
items and gold gained or lost, quest stages reached, skills / level / XP / perk points, perks taken,
spells learned, enemies killed, books read, locations discovered, and where you and each follower
stand (checked every 5 seconds, written when moved). Every save and every load is a checkpoint.

When you load a save and the journal holds progress made after it that was never saved (a crash, or a
quit without saving), a message box says so. Open **F1 > Snapshot > Progress Journal**, tick what you
want, and press **Restore selected**:
  - items and gold are added / removed again
  - quest stages are advanced with the quest's own SetStage (never moved back)
  - skills, level, XP, perk points and health/magicka/stamina are raised to the recorded values
  - perks and spells are added again
  - you, and each follower, are moved back to where you were

Not restored (listed for information): enemies killed, books read, locations discovered, and anything
a mod script was in the middle of. **Dismiss** leaves the save as it is. If you saved again after the
save you load, nothing is offered (loading an older save on purpose).

Journal file: `Documents\My Games\Skyrim Special Edition\SKSE\Snapshot\ProgressJournal.txt`

## What it collects
Snapshot report.txt:
  - game date/time, weather, open menus, game version
  - PLAYER: position, cell, worldspace, location chain, every actor value (current/base/permanent),
    factions with rank, perks, spells, active magic effects, equipped items, bounties, full inventory
  - FOLLOWERS (teammates, CurrentFollowerFaction, summons): same detail plus current AI package,
    all AI packages on the NPC, distance from player
  - NEARBY NPCs: indoors every NPC in the same interior, outdoors every NPC within 10,000 units;
    same detail as followers (position, stats, factions, packages, current package, spells, effects)
  - RUNNING QUESTS: stage, running/starting/stopping/completed/journal flags, every objective and
    its state, scenes playing, every alias and who fills it (with that ref's location)
  - COMPLETED QUESTS: name and final stage
  - EVERY NPC IN MEMORY: location, dead/disabled, factions with rank
  - LOAD ORDER (with ESL indexes) and every DLL in Data\SKSE\Plugins
Logs\SKSE\  : copy of everything in Documents\My Games\Skyrim Special Edition\SKSE (all SKSE and plugin logs, crash logs)
Logs\Logs\  : copy of Documents\My Games\Skyrim Special Edition\Logs (Papyrus script logs)
Saves\      : the 3 newest saves with their .skse co-saves
copy_errors.txt : only if some file could not be copied

Output folder:
  Documents\My Games\Skyrim Special Edition\SKSE\Snapshot\Snapshot_<date>_<time>\
Plugin log:
  Documents\My Games\Skyrim Special Edition\SKSE\Snapshot.log

## Settings

Data\SKSE\Plugins\Snapshot.ini
  - [General]: key, hold time, number of saves, outdoor radius
  - [Journal]: Progress Journal on/off, position check interval (default 5 s), minimum move before a position is written (default 256 units)

## Build

Output: build\Release\Snapshot.dll and .pdb

Developer Command Prompt for VS 2022, one line at a time:
  cd /d C:\dllmaker\Snapshot
  cmake --preset vs2022-windows
  cmake --build build --config Release

## Install (its own MO2 mod)
  SKSE\Plugins\Snapshot.dll
  SKSE\Plugins\Snapshot.pdb
  SKSE\Plugins\Snapshot.ini

## How AI was used

Snapshot was made by tonycubed2 working together with Anthropic's Claude (an AI assistant). tonycubed2
decided what the mod should do, set the requirements, built every DLL on his own PC and tested it in
his own heavily modded game. Claude wrote the C++ source code to those requirements, checked how the
game works against the game's own code and the CommonLibSSE NG headers, and fixed the build errors
tonycubed2 reported back.

## License

Copyright (C) 2026 tonycubed2

This program is free software: you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation, either version 3 of the License,
or (at your option) any later version. This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See [LICENSE](LICENSE) (the GNU General Public License v3) for details.

The Skyrim SE/AE version links [CommonLibSSE NG](https://github.com/alandtse/CommonLibVR/tree/ng), licensed
GPL-3.0-or-later WITH the Modding Exception AND the GPL-3.0 Linking Exception (with Corresponding Source).
Because it links CommonLibSSE NG, Snapshot is released under the GPL-3.0-or-later, and its complete
source code is in this repository. `src/SKSEMenuFramework.h` is the SKSE Menu Framework API header,
included unchanged so the source builds as is.

