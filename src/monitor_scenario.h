#pragma once

#include <string>

// Order-independent, reasonably stable identifier for the current set of
// *active* monitors, built from each one's PNP hardware id (manufacturer +
// model, not a serial number, so two identical monitor models would
// collide -- accepted trade-off for a simple, dependency-free signature).
// The same physical monitor/projector setup maps back to the same key
// across reconnects and reboots, and different setups (laptop panel alone,
// laptop + a specific external, that external alone via "second screen
// only", a different external, etc.) map to different keys -- so callers
// can remember UI placement "per monitor scenario" instead of one global
// position that only ever fits whichever setup was active last.
std::wstring GetMonitorScenarioKey();

struct SavedHudPlacement {
    bool found = false;
    int x = 0;
    int y = 0;
    int width = 0;
};

// Looks up a previously-saved project-list-HUD placement for `scenarioKey`
// (see GetMonitorScenarioKey). `found` is false if this scenario has never
// been saved before.
SavedHudPlacement LoadHudPlacement(const std::wstring& scenarioKey);

// Persists x/y/width for `scenarioKey`, overwriting any previous entry for
// that key. Stored in project_list_hud_positions.ini next to config.ini.
void SaveHudPlacement(const std::wstring& scenarioKey, int x, int y, int width);
