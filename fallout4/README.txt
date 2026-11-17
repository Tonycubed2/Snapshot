Snapshot 1.0.0 for Fallout 4
============================

Troubleshooting snapshot for bug reports. Hold L for 8 seconds in game; when it is done a message box
says "Data Collected".

Requires: Fallout 4 1.11.x (current Steam version), F4SE, Address Library for F4SE Plugins.

Output folder:
  Documents\My Games\Fallout4\F4SE\Snapshot\Snapshot_<date>_<time>\
    Snapshot report.txt  - game time, weather, open menus, player (position, cell, location, every actor
                           value, factions, perks with rank, spells, active effects, inventory with equipped),
                           companions (Followers quest aliases and commanded actors), NPCs in the same
                           interior or within 10,000 units outdoors (stats, factions, AI packages, current
                           package, spells, effects), running quests (stage, flags, objectives, aliases and
                           who fills them), completed quests, every NPC in memory with location and
                           factions, load order, F4SE plugins
    Logs\F4SE\           - copy of Documents\My Games\Fallout4\F4SE (all F4SE and plugin logs, crash logs)
    Logs\Logs\           - copy of Documents\My Games\Fallout4\Logs (Papyrus script logs)
    Saves\               - the 3 newest saves with their .f4se co-saves

Not in the Fallout 4 report (the Skyrim one has them): playing scenes per quest, bounties.

Settings: Data\F4SE\Plugins\Snapshot.ini (key as a Windows key code, hold time, saves, radius).

Build (Developer Command Prompt for VS 2022, xmake installed), one line at a time:
  cd /d C:\dllmaker\fallout4-snapshot
  xmake config -y
  xmake build -y
Output: build\windows\x64\releasedbg\Snapshot.dll and .pdb

Install (its own MO2 mod):
  F4SE\Plugins\Snapshot.dll
  F4SE\Plugins\Snapshot.pdb
  F4SE\Plugins\Snapshot.ini
