#include <stdio.h>
#include <string.h>
#include <ncurses.h>
#include "mux.h"
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>

// ─────────────────────────────────────────────
// GLOBAL PANE TABLE (daemon owns this)
// ─────────────────────────────────────────────

Pane panes[MAX_PANES];
int  pane_count   = 0;
int  active_pane  = 0;

// ─────────────────────────────────────────────
// pane_init()
//
// Called once at daemon start.
// Creates the first pane filling the whole screen
// (minus the status bar at the bottom).
// ─────────────────────────────────────────────

void pane_init(int rows, int cols) {
    memset(panes, 0, sizeof(panes));
    pane_count  = 0;
    active_pane = 0;

    // first pane: full screen minus status bar
    panes[0].row        = 0;
    panes[0].col        = 0;
    panes[0].rows       = rows - STATUS_HEIGHT;
    panes[0].cols       = cols;
    panes[0].session_id = -1;   // no session yet
    panes[0].active     = 1;
    pane_count          = 1;
}


// ─────────────────────────────────────────────
// pane_get_term_size()
// helper — reads actual terminal size from
// the session's PTY master fd
// ─────────────────────────────────────────────

static void pane_get_size(int *rows, int *cols) {
    struct winsize ws;
    // try to get real terminal size
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    } else {
        *rows = 24;
        *cols = 80;
    }
}


// ─────────────────────────────────────────────
// pane_split_h() — split top/bottom
//
//  ┌──────┐       ┌──────┐
//  │      │       │  A   │
//  │  A   │  ──►  ├──────┤
//  │      │       │  B   │
//  └──────┘       └──────┘
// ─────────────────────────────────────────────

int pane_split_h(int pane_idx) {
    if (pane_count >= MAX_PANES)    return -1;
    if (!panes[pane_idx].active)    return -1;

    int rows, cols;
    pane_get_size(&rows, &cols);

    Pane *orig = &panes[pane_idx];

    // need at least 4 rows to split
    if (orig->rows < 4) return -1;

    int half = orig->rows / 2;

    // new pane: bottom half
    Pane *newp   = &panes[pane_count];
    newp->row    = orig->row + half;
    newp->col    = orig->col;
    newp->rows   = orig->rows - half;
    newp->cols   = orig->cols;
    newp->active = 1;
    newp->session_id = -1;

    // original pane: shrinks to top half
    orig->rows = half;

    (void)rows; (void)cols;

    pane_count++;
    return pane_count - 1;
}

// ─────────────────────────────────────────────
// pane_split_v() — split left/right
//
//  ┌──────┐       ┌───┬──┐
//  │      │       │   │  │
//  │  A   │  ──►  │ A │B │
//  │      │       │   │  │
//  └──────┘       └───┴──┘
// ─────────────────────────────────────────────

int pane_split_v(int pane_idx) {
    if (pane_count >= MAX_PANES)   return -1;
    if (!panes[pane_idx].active)   return -1;

    Pane *orig = &panes[pane_idx];

    // need at least 10 cols to split
    if (orig->cols < 10) return -1;

    int half = orig->cols / 2;

    Pane *newp   = &panes[pane_count];
    newp->row    = orig->row;
    newp->col    = orig->col + half;
    newp->rows   = orig->rows;
    newp->cols   = orig->cols - half;
    newp->active = 1;
    newp->session_id = -1;

    orig->cols = half;

    pane_count++;
    return pane_count - 1;
}

// ─────────────────────────────────────────────
// pane_next() / pane_prev()
// ─────────────────────────────────────────────

int pane_next(int current) {
    for (int i = 1; i < MAX_PANES; i++) {
        int idx = (current + i) % MAX_PANES;
        if (panes[idx].active) return idx;
    }
    return current; // only one pane left
}

int pane_prev(int current) {
    for (int i = 1; i < MAX_PANES; i++) {
        int idx = (current - i + MAX_PANES) % MAX_PANES;
        if (panes[idx].active) return idx;
    }
    return current;
}

// ─────────────────────────────────────────────
// pane_resize_session()
// tells bash the actual size of its pane
// ─────────────────────────────────────────────

void pane_resize_session(int pane_idx, int master_fd) {
    if (master_fd < 0) return;
    struct winsize ws;
    ws.ws_row    = (unsigned short)panes[pane_idx].rows;
    ws.ws_col    = (unsigned short)panes[pane_idx].cols;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
    ioctl(master_fd, TIOCSWINSZ, &ws);
}
