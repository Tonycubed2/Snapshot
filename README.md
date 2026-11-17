# Snapshot

An SKSE plugin for Skyrim SE/AE by tonycubed2.

Troubleshooting snapshot for bug reports. Hold L for 8 seconds in game; when it is done a message box
says "Data Collected" and shows the folder it wrote.

Requires: SKSE, Address Library for SKSE Plugins. Skyrim SE 1.5.97 and AE.

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

Data\SKSE\Plugins\Snapshot.ini (key, hold time, number of saves, outdoor radius).

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
