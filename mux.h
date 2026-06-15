#ifndef MUX_H
#define MUX_H

#include <sys/types.h>
#include <ncurses.h>

// ─────────────────────────────────────────────
// CONSTANTS
// ─────────────────────────────────────────────

#define MAX_CLIENTS 8
#define SOCKET_PATH  "/tmp/mux.sock"
#define MAX_SESSIONS 16
#define MAX_PANES    8
#define BUF_SIZE     4096
#define STATUS_HEIGHT 1   // height of bottom status bar

// ─────────────────────────────────────────────
// PROTOCOL
// ─────────────────────────────────────────────

typedef enum {
    CMD_NEW,
    CMD_LIST,
    CMD_ATTACH,
    CMD_KILL,
    CMD_KILL_PANE,
    CMD_SPLIT_H,    // split active pane horizontally
    CMD_SPLIT_V,    // split active pane vertically
    CMD_NEXT_PANE,  // move focus to next pane
    CMD_PREV_PANE,  // move focus to prev pane
    CMD_NEXT_WIN,   // next session/window
    CMD_RESIZE,     // terminal was resized
} CommandType;

typedef struct {
    CommandType type;
    int session_id;
    int rows, cols;   // used by CMD_RESIZE
} Command;

typedef struct {
    int  success;
    int  session_id;
    char message[1024];
} Response;

// ─────────────────────────────────────────────
// SESSION
// ─────────────────────────────────────────────

typedef struct {
    int   id;
    char  name[64];
    int   master_fd;
    pid_t bash_pid;
    int   active;
} Session;

// ─────────────────────────────────────────────
// PANE
// represents one rectangular terminal region
// each pane has its own session (PTY + bash)
// ─────────────────────────────────────────────

typedef struct {
    int session_id;   // which session this pane shows
    int row, col;     // top-left corner on screen
    int rows, cols;   // dimensions
    int active;       // 1 = this slot is used
} Pane;

// ─────────────────────────────────────────────
// FUNCTION DECLARATIONS
// ─────────────────────────────────────────────

// session.c
int      create_session();
int      kill_session(int id);
void     list_sessions(char *buf, int bufsize);
Session *get_session(int id);

// pane.c
void pane_init(int rows, int cols);
int  pane_split_h(int pane_idx);
int  pane_split_v(int pane_idx);
int  pane_next(int current);
int  pane_prev(int current);
void pane_resize_session(int pane_idx, int master_fd);

extern Pane panes[];
extern int  pane_count;
extern int  active_pane;

// daemon.c
void run_daemon();

// client.c
void run_attach_loop(int sock_fd);

#endif
