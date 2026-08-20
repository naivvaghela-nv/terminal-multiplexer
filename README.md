# mux — A Terminal Multiplexer in C

A `tmux`-inspired terminal multiplexer built from scratch in C, implementing session persistence, pane splitting, and a client-daemon architecture over Unix domain sockets.

## Overview

`mux` lets you create detachable terminal sessions that keep running in the background even after you disconnect — the core feature that makes tools like `tmux` and `screen` indispensable for remote work and long-running processes. This implementation handles PTY allocation, process management, and terminal I/O multiplexing entirely at the syscall level, with no external multiplexing libraries.

## Architecture

The system is split into a persistent background **daemon** and a lightweight **client**, communicating over a Unix domain socket at `/tmp/mux.sock`.

```
┌─────────┐        Unix Socket        ┌──────────────────┐
│  client │ ◄─────────────────────►   │      daemon       │
│ (mux)   │      (commands + I/O)     │ (forked, detached) │
└─────────┘                           └──────────┬─────────┘
                                                  │
                                     ┌────────────┼────────────┐
                                     │            │            │
                                 ┌───▼───┐    ┌───▼───┐    ┌───▼───┐
                                 │Session│    │Session│    │Session│
                                 │ (PTY  │    │ (PTY  │    │ (PTY  │
                                 │+bash) │    │+bash) │    │+bash) │
                                 └───────┘    └───────┘    └───────┘
```

- **`client.c`** — Handles terminal raw-mode I/O, keybind detection, and the attach loop that proxies keystrokes to the daemon and renders output back to the terminal. Auto-spawns the daemon on first run if it isn't already active.
- **`daemon.c`** — A `select()`-driven event loop that owns all sessions and clients, routes PTY output to attached clients, and dispatches the command protocol (new/attach/kill/split/switch/resize).
- **`session.c`** — PTY lifecycle management: opens a PTY pair via `openpty()`, forks `/bin/bash` into the slave end, and tracks each session's master fd and PID.
- **`pane.c`** — Screen-space geometry for split panes (horizontal/vertical), independent of session logic, allowing panes to be resized and reassigned to different sessions.
- **`mux.h`** — Shared protocol definitions (`Command`/`Response` structs), constants, and function declarations linking the four translation units.

## Features

- **Session persistence** — sessions survive client disconnects; reattach anytime with `mux attach <id>`
- **Pane splitting** — horizontal and vertical splits, each pane backed by its own independent bash session
- **Daemon auto-start** — the daemon self-forks and daemonizes (double-fork, `setsid()`) transparently on first client connection
- **Dynamic resize propagation** — `SIGWINCH` on the client triggers a `CMD_RESIZE` that is forwarded to the PTY via `TIOCSWINSZ`
- **tmux-style prefix keybinds** (`Ctrl+B` prefix):

  | Key | Action           | Key | Action          |
  |-----|------------------|-----|-----------------|
  | `d` | Detach           | `x` | Kill pane       |
  | `-` | Split horizontal | `c` | New session     |
  | `\|`| Split vertical   | `n` | Next session    |
  | `o` | Next pane        | `?` | Show help       |
  | `p` | Prev pane        |     |                 |

## Technical Highlights

- **Raw terminal mode** via `termios` — disables canonical mode, echo, and signal generation so all keystrokes (including control sequences) reach the application layer
- **PTY-based process isolation** — each session is a genuine pseudo-terminal with its own controlling process group (`setsid()` + `TIOCSCTTY`), so job control and signals behave correctly inside bash
- **`select()`-based I/O multiplexing** on both client and daemon sides, avoiding busy-waiting while juggling multiple file descriptors (stdin, socket, PTY masters)
- **Zombie-free process handling** — `SIGCHLD` ignored in the daemon; client reaps the forked daemon with `waitpid(WNOHANG)`
- **Binary socket protocol** — fixed-size `Command`/`Response` structs sent directly over the Unix socket, keeping the IPC layer simple and dependency-free

## Build & Run

```bash
make            # builds ./mux
./mux new       # create a session and attach
./mux list      # list active sessions
./mux attach 1  # reattach to session 1
./mux kill 1    # kill session 1
```

Requires `gcc`, `libncurses`, and `libutil` (for `openpty`).

## Project Status

Core session/pane/daemon functionality is implemented and working. Built as a systems-programming exercise focused on PTY handling, IPC, and terminal control — deliberately scoped to a base implementation without extra polish (no config file, no status bar rendering yet, `ncurses` linked but not yet driving a UI layer).

## Roadmap

This is an active project, not a finished one. Planned next steps:

- [ ] `ncurses`-driven status bar (session/pane indicator, currently linked but unused)
- [ ] Config file support (custom keybinds, prefix key)
- [ ] Persistent scrollback / ring buffer per pane
- [ ] Session renaming and pane titles
- [ ] Mouse support for pane focus switching
- [ ] Copy mode (vim-style scrollback search/select)

## Tech Stack

C · POSIX (`pty.h`, `termios.h`, `sys/socket.h`, `sys/select.h`) · Unix domain sockets · `ncurses`
