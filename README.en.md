# redntfy

[![日本語](https://img.shields.io/badge/lang-日本語-red)](README.md)
[![English](https://img.shields.io/badge/lang-English-blue)](README.en.md)

[![Release](https://img.shields.io/github/v/release/aviscaerulea/redntfy)](https://github.com/aviscaerulea/redntfy/releases/latest)
[![Build](https://github.com/aviscaerulea/redntfy/actions/workflows/release.yml/badge.svg)](https://github.com/aviscaerulea/redntfy/actions/workflows/release.yml)

A lightweight resident app that notifies you of Redmine ticket updates via Windows toast notifications and lets you browse your open tickets from the system tray.

## Features

- Notification targets are fully customizable via Redmine saved queries; polls on a schedule and shows toast notifications for new or updated tickets
- Displays an open-ticket list from the tray, formatted with icons for due dates, assignees, and projects
- Pins keep important tickets at the top of the list (they stay even after being closed)
- Instant refresh, filters, and sort orders are available from the tray menu
- Notification sounds are loudness-normalized, and auto-muted while a microphone or camera is in use (e.g. during meetings)
- Checks GitHub Releases for a newer version at startup
- Lightweight: about 7 MB of physical memory while resident

## Installation

### Requirements

- Windows 10/11
- A Redmine API access key
- Redmine global saved queries (created without specifying a project)

### Steps

Using Scoop.

```powershell
scoop bucket add aviscaerulea https://github.com/aviscaerulea/scoop-bucket
scoop install redntfy
```

To install manually from a zip archive, download the zip from [Releases](https://github.com/aviscaerulea/redntfy/releases/latest), extract it to any folder, and run `redntfy.exe`.

## Usage

Initial setup:

1. In Redmine, filter the issue list **without specifying a project** and save it as a custom query
   - Queries saved under a specific project cannot be referenced via the API, so they must be created as global queries
   - Take note of `N` in the resulting URL `/issues?query_id=N` (one per query if you track multiple)
2. Obtain an API key from "My account" → "API access key" in Redmine
3. Create `redntfy.local.toml` next to `redntfy.exe` and write the connection settings

   ```toml
   [redmine]
   url       = "https://redmine.example.com"
   api_key   = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
   query_ids = [12, 34]
   ```

4. Launch `redntfy.exe`

Everyday operations:

- Left-click the tray icon to show the open-ticket list
- Left-click a row to open the ticket in the browser and mark it as read; right-click to toggle a pin
- Right-click the tray icon to access the menu for instant refresh, filters, and sort orders

## Configuration

Runtime settings live in `redntfy.toml` next to `redntfy.exe`.
Placing `redntfy.local.toml` overrides values on a per-key basis, which is useful for separating connection settings or per-environment differences.
Detected state is saved to `state.json` and pins to `pins.json`; both persist across restarts.

The main configuration keys are listed below. See the comments in `redntfy.toml` for details.

| Section | Key | Description |
| --- | --- | --- |
| `[app]` | `schedule` | Polls per hour for each of the 24 hours |
| `[app]` | `list_limit` | Number of rows in the list (default 20) |
| `[app]` | `bug_trackers` | Tracker-name patterns that get a 💥 icon |
| `[app]` | `duck_targets` | Process names to mute while a sound plays |
| `[redmine]` | `url`, `api_key`, `query_ids` | Connection settings (required) |
| `[loudness]` | `enabled`, `target` | Loudness normalization for the notification sound |
| `[update]` | `enabled` | Update check at startup |

## Limitations

- Only Redmine global saved queries are supported (queries under a specific project cannot be referenced via the API)
- Group-assignee detection covers all groups only with admin privileges; otherwise it only checks your own groups
- The configuration file is not hot-reloaded; a restart is required to apply changes

## Build

Visual Studio Build Tools, vcpkg, go-task, PowerShell 7, and git are required.

```powershell
task build      # Standard build (out/redntfy.exe)
task release    # Release build with zip packaging
```

## Tech Stack

- C++20 / Win32 API (single translation unit)
- WinHTTP (Redmine REST API)
- C++/WinRT (Windows.UI.Notifications, Windows.Data.Json)
- WASAPI (notification sound playback)
- libebur128 (loudness measurement)
- toml++ (configuration file)

## License

The application icon uses the official Redmine logo.
The logo is a work by Martin Herr, licensed under [CC BY-SA 2.5](https://creativecommons.org/licenses/by-sa/2.5/).
