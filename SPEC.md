# Digital Clock (Win10, C++17/Win32)
## SPEC and requirements
- Floating, always-on-top clock window that can be dragged anywhere on screen
- Thin metal-gray frame of equal width on all sides
- auto-sizes to fit the text and is not user-resizable.
- Multi-city time display; shows one line per city using UTC offsets.
- Cities can be added/edited/deleted at runtime via context menu dialogs; list can be persisted to and reloaded from `config/cities.txt`.
- Time synchronization via UDP NTP client; defaults to `pool.ntp.org` but the server is user-configurable (`config/ntp.txt`) and can be refreshed on demand.
- Auto detect daylight saving time for cities
- Pure C++ Win32 with no external dependencies; suitable for Windows 10 desktop.

## Build & Run
### Prerequisites:
- Visual Studio 2022 Build Tools with Windows 10 SDK;
- `.vscode/settings.json` provides MSVC/WinSDK paths used by tasks (no PATH setup needed). 
- Default setting:
  - Visual Studio 2022 : "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\MSVC\14.44.35207"
  - Windows 10 SDK : "C:\Program Files (x86)\Windows Kits\10"
  - Windows 10 SDK : version : "10.0.26100.0"

### Build:
#### In VS Code:
run **Build digital-clock (Debug)** or **Build digital-clock (Release)**. 
#### Outputs:
 `output\digital-clock.debug.exe` (Debug) and `output\digital-clock.exe` (Release).

### Run:
Launch either binary directly.
Window starts topmost at initial position 100x100.

## Controls
- Drag: left-click and drag anywhere on the window.
- Right-click: opens context menu with add/edit/delete city, save/reload config, open config in Notepad, NTP sync, NTP server edit (with Reset), exit.
- NTP: `Sync time (NTP)` triggers immediate sync; message box shows success/failure (startup sync is silent). Reset restores `pool.ntp.org`.
- DST: Auto-detects daylight saving for common cities (New York, Los Angeles, Chicago, San Francisco, Toronto, Mexico City, London, Berlin, Paris, Sydney, Auckland) using region rules; other cities use their fixed UTC offset.

## Config files (created on first save/sync)
- `config/cities.txt` - one city per line, format `Name|OffsetMinutes` (offset in minutes from UTC, e.g., `Shanghai|480`). Invalid lines are ignored. Defaults: Auckland (+720), Shanghai (+480).
- `config/ntp.txt` - single line with server host or IP. Defaults to `pool.ntp.org` if missing/empty (Reset uses this default).

## Runtime behavior
- Display updates every second; NTP sync kicks off at startup and when requested. When NTP data is available, the clock keeps time using monotonic ticks and falls back to `GetSystemTimeAsFileTime` if NTP is absent. DST adjustment adds +60 minutes when active per city rule above.
- Window styles: topmost, tool window, layered (slightly transparent); custom metal-gray frame is drawn inside the client area.
- Colors: dark background with green text for readability.
