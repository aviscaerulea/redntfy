# redntfy

[![日本語](https://img.shields.io/badge/lang-日本語-red)](README.md)
[![English](https://img.shields.io/badge/lang-English-blue)](README.en.md)
[![Release](https://img.shields.io/github/v/release/aviscaerulea/redntfy)](https://github.com/aviscaerulea/redntfy/releases/latest)
[![License](https://img.shields.io/github/license/aviscaerulea/redntfy)](LICENSE)
[![Build](https://github.com/aviscaerulea/redntfy/actions/workflows/release.yml/badge.svg)](https://github.com/aviscaerulea/redntfy/actions/workflows/release.yml)

A lightweight resident app that notifies you of Redmine ticket updates via Windows toast notifications and lets you browse your open tickets from the system tray.

## Features

- Starts tracking tickets assigned to you with just a URL and an API key; polls on a schedule and shows toast notifications for new or updated tickets
- Notification targets are freely adjustable via Redmine saved queries (query_ids)
- Displays an open-ticket list from the tray, formatted with icons for due dates, assignees, and projects
- Hovering over the tray icon also shows the list (the delay is adjustable, and the feature can be turned off)
- The order and items of each list row are fully customizable via placeholders in the configuration
- Pins keep important tickets in the list (they stay even after being closed)
- Tickets you do not need to watch can be hidden, excluding them from notifications and the pending count
- Instant refresh, filters, and sort orders are available from the tray menu
- Notification sounds are loudness-normalized, and auto-muted while a microphone or camera is in use (e.g. during meetings)
- Checks GitHub Releases for a newer version at startup
- Lightweight: about 7 MB of physical memory while resident

## Installation

### Requirements

- Windows 10/11
- A Redmine API access key

### Steps

Using Scoop.

```powershell
scoop bucket add aviscaerulea https://github.com/aviscaerulea/scoop-bucket
scoop install redntfy
```

To install manually from a zip archive, download the zip from [Releases](https://github.com/aviscaerulea/redntfy/releases/latest), extract it to any folder, and run `redntfy.exe`.

## Usage

Initial setup:

A step-by-step guide with screenshots is available in the [setup guide](https://aviscaerulea.github.io/redntfy/) (written in Japanese).

1. Obtain an API key from "My account" → "API access key" in Redmine
2. Create `redntfy.local.toml` next to `redntfy.exe` and write the connection settings

   ```toml
   [redmine]
   url     = "https://redmine.example.com"
   api_key = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
   ```

3. Launch `redntfy.exe`

It is fine to launch without `redntfy.local.toml`. The app generates a template automatically, opens the configuration file, and guides you to the page where the key can be obtained.

That is all it takes — the app starts tracking open tickets assigned to you (and your groups).
If you want to tune the tracking scope yourself, create custom queries in Redmine and set `query_ids`.

1. In Redmine, filter the issue list **without specifying a project** and save it as a custom query
   - Queries saved under a specific project cannot be referenced via the API, so they must be created as global queries
2. Take note of `N` in the resulting URL `/issues?query_id=N` (one per query if you track multiple)
3. Add `query_ids = [12, 34]` to `redntfy.local.toml` and restart

Everyday operations:

- Left-click the tray icon to show the open-ticket list
- Resting the cursor on the tray icon shows the same list (0.25 s by default; it closes automatically when the cursor leaves, and focus returns to the previous window)
- Toggle hover display via "Show list on hover" in the right-click menu (ON by default, persists across restarts; while ON, the count tooltip is not shown)
- Left-click a row to open the ticket in the browser and mark it as read; right-click to cycle pin → hidden → normal
- Hidden tickets are shown in gray and excluded from notifications and the pending count ("Exclude hidden tickets" in the menu removes them from the list entirely)
- Right-click the tray icon to access the menu for instant refresh, filters, and sort orders

## Configuration

Runtime settings live in `redntfy.toml` next to `redntfy.exe`.
Placing `redntfy.local.toml` overrides values on a per-key basis, which is useful for separating connection settings or per-environment differences.
Detected state is saved to `state.json`, pins to `pins.json`, and hidden tickets to `hidden.json`; all persist across restarts.

The main configuration keys are listed below. See the comments in `redntfy.toml` for details.

| Section | Key | Description |
| --- | --- | --- |
| `[app]` | `schedule` | Polls per hour for each of the 24 hours |
| `[app]` | `list_limit` | Number of rows in the list (default 20) |
| `[app]` | `list_format` | Row format of the list (placeholder based) |
| `[app]` | `hover_delay_ms` | Delay before the list appears on hover (milliseconds, 0-5000; default 250, 0 for immediate) |
| `[app]` | `bug_trackers` | Tracker-name patterns that get a 💥 icon |
| `[app]` | `duck_targets` | Process names to mute while a sound plays |
| `[redmine]` | `url`, `api_key` | Connection settings (required) |
| `[redmine]` | `query_ids` | Saved-query ids to track (if omitted, tracks tickets assigned to you) |
| `[loudness]` | `enabled`, `target` | Loudness normalization for the notification sound |
| `[update]` | `enabled` | Update check at startup |

### With and without query_ids

`query_ids` is optional. Tracking behaves as follows depending on whether it is set.

| Aspect | Set | Not set |
| --- | --- | --- |
| Tracked tickets | Union of the specified saved queries | Open tickets assigned to you (and your groups) |
| Preparation | Requires creating custom queries in Redmine | Works with just `url` and `api_key` |
| Scope tuning | Freely adjustable via query filters | Fixed (no filtering) |
| Screen opened from notifications/list | The first query's issue list | The issue list filtered by assignee = me |

A good way to start is without `query_ids`, then create custom queries and set it once you want to tune the tracking scope.
Switching the setting does not flood you with notifications.

## Limitations

- Only Redmine global saved queries are supported (queries under a specific project cannot be referenced via the API)
- Group-assignee detection covers all groups only with admin privileges; otherwise it only checks your own groups
- The hover list does not work while the tray icon is in the hidden-icons overflow area (drag the icon onto the taskbar to pin it)
- The configuration file is not hot-reloaded; a restart is required to apply changes
- If the configuration is incomplete, the app stays resident in a guidance mode without notifications (follow the tray icon and its tooltip, fix the settings, and restart)

## Build

Visual Studio Build Tools, vcpkg, go-task, PowerShell 7, and git are required.

```powershell
task build      # Standard build (out/redntfy.exe)
task release    # Release build with zip packaging
```

## License

The application icon uses the official Redmine logo.
The logo is a work by Martin Herr, licensed under [CC BY-SA 2.5](https://creativecommons.org/licenses/by-sa/2.5/).
