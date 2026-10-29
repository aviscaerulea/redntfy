# redntfy

[![日本語](https://img.shields.io/badge/lang-日本語-red)](README.md)
[![English](https://img.shields.io/badge/lang-English-blue)](README.en.md)
[![Release](https://img.shields.io/github/v/release/aviscaerulea/redntfy)](https://github.com/aviscaerulea/redntfy/releases/latest)
[![License](https://img.shields.io/github/license/aviscaerulea/redntfy)](LICENSE)
[![Build](https://github.com/aviscaerulea/redntfy/actions/workflows/release.yml/badge.svg)](https://github.com/aviscaerulea/redntfy/actions/workflows/release.yml)

A lightweight resident app that delivers Redmine ticket notifications and an issue list from the system tray.
Measured physical memory usage is about 7 MB.
A sister tool, [gcalntfy](https://github.com/aviscaerulea/gcalntfy), notifies you of Google Calendar events the same way.

## Features

- Ticket notifications: polls Redmine and notifies you of new or updated tickets, via Windows notification and sound
- System tray: view the open-ticket list and change settings from the tray icon
  - List format: freely change which items each row shows and in what order
  - Sorting: sort by updated date, due date, created date, priority, and more
  - Pins: keep important tickets in the list
  - Hiding: excludes tickets you do not need to watch from notifications and the pending count
  - Browser display: clicking a row opens the ticket in the browser
  - Search: highlights tickets containing the text you type
  - Keyboard: select and open tickets with the keyboard alone
  - Shortcut key: open and close the list from any app with a key combination
  - Ticket registration: create a ticket assigned to yourself from the list, without opening Redmine
  - Row menu: right-click to copy a ticket, change its priority or due date, or close it
- Narrowing the target: Redmine custom queries (one or more) freely choose which tickets are notified and listed
- Zero-configuration start: targets tickets assigned to you and your groups with just a URL and an API access key

### System tray

Hovering over or clicking the tray icon opens the open-ticket list.
The tray icon shows a red badge when there are unread tickets.

The list highlights unread tickets and tickets with a due date in bold, and high-priority tickets with a red background.
Newly created tickets are marked with ✨, and tickets updated by someone else with 🆙.

Right-clicking the tray icon opens the tray menu, which provides various settings.

## Installation

### Requirements

- Windows 10/11
- A Redmine API access key

### Steps

#### From the release ZIP

Download the zip from [Releases](https://github.com/aviscaerulea/redntfy/releases/latest), extract it to any folder, and run `redntfy.exe`.

#### From Scoop

```powershell
scoop bucket add aviscaerulea https://github.com/aviscaerulea/scoop-bucket
scoop install redntfy
```

#### Uninstallation

Uninstalling leaves the following three behind. Delete them manually if you no longer need them.

- The data folder `%LOCALAPPDATA%\redntfy`
- The settings registry key `HKEY_CURRENT_USER\SOFTWARE\redntfy`
- The Start menu shortcut `redntfy.lnk`

If you registered the app to start with Windows, turn that off from the tray menu before deleting them.

## Usage

1. Obtain an API access key from "My account" in Redmine
2. Place `redntfy.local.toml` containing the connection settings next to `redntfy.exe`

   ```toml
   [redmine]
   url     = "https://redmine.example.com"
   api_key = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
   ```

3. Launch `redntfy.exe`

Once launched, the app starts notifying you of tickets assigned to you.
To narrow which tickets are notified, see the [setup guide](https://aviscaerulea.github.io/redntfy/) with screenshots (written in Japanese).

## Configuration

Settings live in `redntfy.toml` next to `redntfy.exe`.
Keys written in `redntfy.local.toml` override the same keys in `redntfy.toml`.

You can change the connection target, which tickets are notified, how often the app polls, how the list looks, and more.
See the comments in `redntfy.toml` for each item.

## Limitations

- Changes to the configuration file take effect only after a restart
- Only global custom queries, which do not belong to a project, can be used to choose the notified tickets
- If the connection settings are wrong, the app stops notifications and shows how to fix them on the tray icon

## License

The application icon uses the official Redmine logo.
The logo is a work by Martin Herr, licensed under [CC BY-SA 2.5](https://creativecommons.org/licenses/by-sa/2.5/).
