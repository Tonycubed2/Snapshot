Snapshot 1.0.0 for Skyrim Legendary Edition
===========================================

Troubleshooting snapshot for bug reports. Hold L for 8 seconds in game; when it is done a message box
says "Data Collected".

Requires: Skyrim 1.9.32.0 (the last LE patch) and SKSE 1.7.3.

Output folder:
  Documents\My Games\Skyrim\SKSE\Snapshot\Snapshot_<date>_<time>\
    Snapshot report.txt  - game time, weather, open menus, player (position, cell, worldspace, location,
                           every actor value, factions, perks, bounties, spells, active effects, equipped and
                           worn items, inventory), followers (teammates, CurrentFollowerFaction, summons), NPCs in
                           the same interior or within 10,000 units outdoors (stats, factions, AI packages on the
                           NPC and the one it is running, spells, effects, combat target), running quests (stage,
                           flags, objectives, playing scenes, aliases and who fills them), completed quests,
                           every NPC in memory with location and factions, load order, SKSE plugins
    Logs\SKSE\           - copy of Documents\My Games\Skyrim\SKSE (all SKSE and plugin logs)
    Logs\Logs\           - copy of Documents\My Games\Skyrim\Logs (Papyrus script logs)
    Saves\               - the 3 newest saves with their .skse co-saves

The same report as the Skyrim SE/AE Snapshot. The addresses it reads (every form, the weather, sneaking,
combat) are listed at the top of src\main.cpp, each checked in TESV.exe 1.9.32.0 and in SKSE 1.7.3 / libSkyrim.

Settings: Data\SKSE\Plugins\Snapshot.ini (key as a Windows key code, hold time, saves, radius).

Build (Developer Command Prompt for VS 2022), one line at a time:
  cd /d C:\dllmaker\skyrimle-snapshot
  cmake --preset vs2022-win32
  cmake --build build --config Release
Output: build\Release\Snapshot.dll and .pdb

Install (its own MO2 mod):
  SKSE\Plugins\Snapshot.dll
  SKSE\Plugins\Snapshot.pdb
  SKSE\Plugins\Snapshot.ini
