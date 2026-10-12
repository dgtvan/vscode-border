#pragma once

#include <string>

// Tray menu > Capture Diagnostic Snapshot. Some monitor setups draw the
// border wrong in ways that are hard to describe after the fact and easy
// to lose once the user hides the borders and moves on -- so this freezes
// everything that could explain it into its own folder, for investigating
// later:
//
//   logs\snapshots\snapshot_<YYYYMMDD-HHMMSS>\ containing:
//     report.txt   - monitors (rects, work areas, DPI/scaling, display
//                    modes, display-config paths), taskbars, this process's
//                    DPI awareness, every tracked window with its target /
//                    DWM frame / overlay rects both as this app sees them
//                    and in physical pixels plus the resulting gap on each
//                    side, pixel samples of each border band taken from the
//                    screenshot, this app's own top-level windows, untracked
//                    VS Code windows, and the desktop z-order
//     screen.png   - the whole virtual desktop, in physical pixels
//     config.ini   - copy of the live config
//     <log>.log    - copy of the current session's log file
//
// Kept under logs\ (so tray > Open Log Folder leads to it) but in a
// subfolder, out of reach of the logger's 7-day pruning -- a snapshot is
// meant to outlive "later". Returns the folder path, or empty on failure.
std::wstring CaptureDiagnosticSnapshot();
