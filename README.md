# copilot-sessions

`copilot-sessions` is a small terminal UI for browsing and resuming GitHub Copilot CLI sessions stored in a Copilot session-store SQLite database.

Current project version is defined by `VERSION` in the `Makefile` and is shown in the TUI header/help view.

![copilot-sessions screenshot](./copilot-sessions.png)

It presents sessions in a compact split-pane layout with:

- folder-based filtering
- a scrollable session list
- a selected-session summary panel
- a detail modal for turns, checkpoints, refs, and touched files
- a full-path preview toggle for long folder entries

The app is written in C++ and uses:

- [notcurses](https://github.com/dankamongmen/notcurses) for the TUI
- SQLite for reading the Copilot session store

## What it does

`copilot-sessions` loads sessions from the local Copilot session database and lets you quickly answer questions like:

- what sessions exist for a given folder
- what a session was about
- when it happened
- which files and refs it touched
- what the user and assistant said during the session
- how to quickly resume a session back in Copilot CLI

The interface is optimized for keyboard (vim-like) navigation and fast scanning in a terminal.

## Requirements

- C++20 compiler (by choice, not because required :))
- `pkg-config`
- `notcurses`
- `sqlite3`

## Build

```bash
make
```

## Run

```bash
make run
```

For a debug build:

```bash
make BUILD=debug
```

The app starts with the Copilot session database resolved in this order:

```text
${COPILOT_HOME}/session-store.db
$HOME/.copilot/session-store.db
```

You can switch to a different session store at runtime from command mode with
`:open <db path>`.

## Controls

- `Tab`, `h`, `l` — switch focus between folders and sessions; entering folders enables full path mode
- `j`, `k` — move selection
- `u`, `d` — page up/down in the main browser
- `gg`, `ge` — jump to top or bottom in the focused browser pane
- `su`, `sc` — sort stored sessions by last update time or creation time (default descending)
- `sua`, `sud`, `sca`, `scd` — sort ascending or descending explicitly
- `:` — open command mode
- `y` — yank the selected session ID to the clipboard
- `c` — continue or resume the selected session in GitHub Copilot CLI
- `PageUp`, `PageDown` — jump by page
- `Space` — toggle full folder path preview for the selected folder
- `Enter` — open session detail modal
- `r` — reload from the database
- `q` — quit

### Command mode

- `:help` — open the in-app help modal
- `:theme dark` — switch to the dark color theme
- `:theme light` — switch to the light color theme
- `:open <db path>` — open a different Copilot session-store SQLite database

### Modals

- `j`, `k` — scroll
- `u`, `d` — page up/down
- `gg` — jump to top
- `ge` — jump to bottom
- `c` — continue or resume the selected session in GitHub Copilot CLI from the session detail modal
- `Enter`, `Esc`, `q` — close

## Notes

- Missing folders in the CWD list are marked with `!` and highlighted.
- Resuming a session starts Copilot from that session's stored CWD.
- Resuming a session whose original folder no longer exists shows a warning modal and blocks resume until the folder exists again.
- Yanking a session ID shows a confirmation modal with the full copied ID.
- Long folder names are truncated in the sidebar and can be expanded with the path preview toggle; entering the folder pane always enables that mode.
- The app starts in the dark theme and can switch between dark and light themes from command mode.
- `:open` accepts absolute paths, relative paths, and `~/...` paths.
- Stored sessions are sorted by last update time by default; the list shows both update and creation timestamps.
- The UI is designed for a reasonably sized terminal and asks for at least `72x18`.
