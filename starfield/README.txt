Snapshot 1.0.0 for Starfield
============================

Troubleshooting snapshot for bug reports. Hold L for 8 seconds in game; when it is done a message box
says "Data Collected".

Requires: Starfield 1.16.244 (Steam), SFSE, Address Library for SFSE Plugins.

Output folder:
  Documents\My Games\Starfield\SFSE\Snapshot\Snapshot_<date>_<time>\
    Snapshot report.txt  - player (position, cell, location, every actor value, factions), companions
                           (player teammates), NPCs in the same interior or within 10,000 units outdoors
                           (stats, factions, current AI package), running quests (current stage and every
                           stage done), completed quests, every actor the game is running AI for with
                           location and factions, load order, SFSE plugins
    Logs\SFSE\           - copy of Documents\My Games\Starfield\SFSE (all SFSE and plugin logs)
    Logs\Logs\           - copy of Documents\My Games\Starfield\Logs (Papyrus script logs)
    Saves\               - the 3 newest saves, each with every file of the same name (co-saves)

Not in the Starfield report (the game is less mapped out than Skyrim / Fallout 4): quest objectives and
aliases, faction changes made by scripts (only the NPC record's factions), the NPC's list of AI packages
(only the one it is running), spells, active effects, inventory, perks, plugin name per record, NPCs the
game is not running AI for.

Settings: Data\SFSE\Plugins\Snapshot.ini (key as a Windows key code, hold time, saves, radius).

Build (Developer Command Prompt for VS 2022, xmake installed), one line at a time:
  cd /d C:\dllmaker\starfield-snapshot
  xmake config -y
  xmake build -y
Output: build\windows\x64\releasedbg\Snapshot.dll and .pdb

Install (its own MO2 mod, or into Data):
  SFSE\Plugins\Snapshot.dll
  SFSE\Plugins\Snapshot.pdb
  SFSE\Plugins\Snapshot.ini
