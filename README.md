
# Systemd Service Manager

A lightweight GTK3 desktop GUI for managing systemd services on Ubuntu and any other systemd-based Linux distribution. Written in pure C, it inherits your current desktop theme with no hardcoded colours or fonts.

---

## Screenshots

The window is split into two panes:

- **Top pane** — sortable, filterable table of all systemd services
- **Bottom pane** — action buttons and a live output / journal log
## Screenshots

![Screenshot From 2026-04-16 10-05-53](Screenshot%20From%202026-04-16%2010-05-53.png)

![Screenshot From 2026-04-16 10-06-27](Screenshot%20From%202026-04-16%2010-06-27.png)
---

## Features

| Feature | Detail |
|---|---|
| Service list | All services (loaded, not-loaded, failed) with load state, active state, sub-state and description |
| Boot toggle | Checkbox per row to enable/disable a service at startup |
| Live search | Filter by name as you type |
| State filters | One-click filter: All / Active / Inactive / Failed / Enabled / Disabled |
| Actions | Start, Stop, Restart, Enable, Disable |
| Journal viewer | Shows last 80 lines of `journalctl` output for any selected service |
| Live output log | Every command and its result is shown in the bottom pane |
| Read-only mode | All mutating actions are greyed out when not running as root; journal still works |
| Theme inheritance | Uses GTK3 native widgets — follows your desktop theme automatically (GNOME, XFCE, MATE, etc.) |
| Non-blocking UI | Service list loads in a background thread; the UI never freezes |

---

## Requirements

| Requirement | Package (Ubuntu/Debian) |
|---|---|
| GTK3 runtime | `libgtk-3-0` (usually pre-installed) |
| GTK3 dev headers | `libgtk-3-dev` (build only) |
| GCC | `build-essential` |
| systemd | Pre-installed on Ubuntu 16.04+ |

Install build dependencies in one line:

```bash
sudo apt install build-essential libgtk-3-dev
```

---

## Building

```bash
gcc $(pkg-config --cflags gtk+-3.0) \
    -o systemd-manager systemd-manager.c \
    $(pkg-config --libs gtk+-3.0) \
    -lpthread -std=gnu11 -O2
```

Or use the included helper script:

```bash
bash build.sh
```

---

## Running

```bash
# Full control (start/stop/enable/disable)
sudo ./systemd-manager

# Read-only (browse and view journal without root)
./systemd-manager
```

> **Note:** `systemctl enable` and `disable` modify symlinks in `/etc/systemd/system`, which requires root. Starting and stopping services also requires root. Viewing the journal does not.

---

## Usage

### Browsing services

The table loads all services automatically on startup. Click any column header to sort. Use the search box to filter by name, or click one of the filter buttons (Active, Inactive, Failed, Enabled, Disabled) to narrow the list.

### Enabling / disabling at boot

Tick or untick the **Boot** checkbox in the first column, or select a service and click **✔ Enable** / **✘ Disable**. This runs `systemctl enable` or `systemctl disable` and refreshes the list.

### Starting / stopping a service

Select a service in the list, then click **▶ Start**, **■ Stop**, or **↺ Restart**. The command and its output appear in the log pane at the bottom.

### Viewing the journal

Select any service and click **📋 Journal**. The last 80 log lines from `journalctl -u <service>` are printed in the log pane. This works without root.

### Refreshing the list

Click the **⟳** refresh button in the title bar, or the list will refresh automatically after any start/stop/enable/disable action.

---

## File listing

```
systemd-manager.c   Main source file (~820 lines, C11)
build.sh            One-line build helper script
README.md           This file
```

---

## How theme inheritance works

No custom CSS, colours, or fonts are injected anywhere in the code. The application relies entirely on GTK3's theme engine:

- `GtkHeaderBar` — title bar drawn by the desktop theme
- `GtkSearchEntry`, `GtkToggleButton`, `GtkTreeView` — all standard GTK widgets
- The log text view gets the `"monospace"` style class so the active theme applies its own monospace font and colours
- Pango weight (`WEIGHT_SEMIBOLD` for active, `WEIGHT_BOLD` for failed) is used instead of colour overrides so it works in both light and dark themes

---

## Licence

GNU General Public License v3.0
