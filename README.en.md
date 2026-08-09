# redntfy

[![日本語](https://img.shields.io/badge/lang-日本語-red)](README.md)
[![English](https://img.shields.io/badge/lang-English-blue)](README.en.md)
[![Release](https://img.shields.io/github/v/release/aviscaerulea/redntfy)](https://github.com/aviscaerulea/redntfy/releases/latest)
[![License](https://img.shields.io/github/license/aviscaerulea/redntfy)](LICENSE)
[![Build](https://github.com/aviscaerulea/redntfy/actions/workflows/release.yml/badge.svg)](https://github.com/aviscaerulea/redntfy/actions/workflows/release.yml)

A lightweight resident app that notifies you of Redmine ticket updates via Windows notifications and lets you browse your open tickets from the system tray.

Its main features are notifications for new or updated tickets and the open-ticket list opened from the tray icon.
Which tickets are notified and listed can be narrowed freely with Redmine custom queries.

Measured physical memory usage is about 7 MB.

A sister tool, [gcalntfy](https://github.com/aviscaerulea/gcalntfy), notifies you of Google Calendar events the same way.

## Features

- Ticket notifications: polls Redmine and notifies you of new or updated tickets, via Windows notification and sound
- System tray: view the open-ticket list and change settings from the tray icon
  - List format: the order and items of each row can be specified with placeholders
  - Pins: keep important tickets in the list (they stay even after being closed)
  - Hiding: excludes tickets you do not need to watch from notifications and the pending count
  - Browser display: clicking a row or the footer opens it in the browser
- Narrowing the target: Redmine custom queries (one or more) freely choose which tickets are notified and listed
- Zero-configuration start: targets tickets assigned to you and your groups with just a URL and an API access key

### System tray

The tray icon shows a red badge in the bottom-right corner when there are unread tickets. Keeping the cursor on the icon opens the same open-ticket list as a left click. Hover display can be toggled with "マウスホバーで一覧を自動表示" (Auto-show list on hover) in the tray menu.

The open-ticket list never takes focus, so it does not interrupt typing in the window you were using. It closes automatically when the cursor leaves both the icon and the list, and a left click toggles it open or closed. The list is mouse-only and cannot be operated with the keyboard.

Each row of the list shows the due date, assignee, project, and time since the last update, with icons. Unread tickets are shown in bold, and the pending count appears in the footer. Clicking a row opens the ticket in the browser and marks it as read, and right-clicking a row cycles through pinned, hidden, and normal.

Right-clicking the tray icon opens the tray menu, which provides various settings.

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

That is all it takes — the app starts notifying you of updates to open tickets assigned to you (and your groups).
If you want to narrow the target yourself, save a global custom query in Redmine.  
Then set the id of that query in `query_ids`.  
See the setup guide for the detailed steps.

## Configuration

Runtime settings live in `redntfy.toml` next to `redntfy.exe`.
Placing `redntfy.local.toml` overrides values of the same keys on a per-key basis.
That is useful for separating connection settings or per-environment differences.

Configurable items are the connection settings, the custom queries to target, the number of polls per hour for each hour of the day, the row count and row format of the list, the hover behavior, loudness normalization for the notification sound, and the update check at startup.
See the comments in `redntfy.toml` for the meaning and default value of each key.

### With and without `query_ids`

`query_ids` is optional. Which tickets are notified and listed changes as follows depending on whether it is set.

| Aspect | Set | Not set |
| --- | --- | --- |
| Target tickets | Union of the specified custom queries | Open tickets assigned to you (and your groups) |
| Preparation | Requires creating custom queries in Redmine | Works with just `url` and `api_key` |
| Narrowing the target | Freely adjustable via query filters | Fixed (no filtering) |
| Redmine screen opened from notifications/list | The first query's issue list | The issue list filtered by assignee = me |

An example with `query_ids` set.  
To go without it, omit the `query_ids` line.

```toml
[redmine]
url     = "https://redmine.example.com"
api_key = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"

# Ids of global saved queries, saved without specifying a project.
# When several ids are given, the union of all queries is tracked.
# The first id is used for the query screen opened from multi-issue notifications and the list footer.
query_ids = [12, 34]
```

A good way is to start without `query_ids`.
Once you want to narrow the target, create custom queries and set it.
Switching the setting does not flood you with notifications.

## Limitations

- `query_ids` accepts global custom queries only (queries saved under a specific project cannot be referenced via the API)
- With insufficient Redmine permissions, the group-assignee mark appears only for your own groups
- An icon in the hidden-icons overflow area cannot be opened by hover (left-click still works)
- The configuration file is not hot-reloaded; a restart is required to apply changes
- If the connection settings are incomplete, the app stays resident with notifications turned off (the tray icon shows how to fix it)

## License

The application icon uses the official Redmine logo.
The logo is a work by Martin Herr, licensed under [CC BY-SA 2.5](https://creativecommons.org/licenses/by-sa/2.5/).
