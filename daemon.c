#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>

#include "mux.h"

// ─────────────────────────────────────────────
// CLIENT TABLE
// ─────────────────────────────────────────────

typedef enum {
    CLIENT_IDLE,
    CLIENT_ATTACHED,
} ClientState;

typedef struct {
    int         fd;
    ClientState state;
    int         session_id;   // which session this client is on
    int         term_rows;    // terminal dimensions reported by client
    int         term_cols;
} Client;

static Client clients[MAX_CLIENTS];

// ─────────────────────────────────────────────
// handle_attach_data()
//
// Proxies data between the client socket and
// the session's PTY master_fd.
// Called from the main select() loop whenever
// either fd has data.
// ─────────────────────────────────────────────

static void handle_attach_data(int ci, fd_set *ready) {
    Client *c = &clients[ci];
    Session *s = get_session(c->session_id);
    char buf[BUF_SIZE];

    if (!s) {
        close(c->fd);
        c->fd = -1;
        c->state = CLIENT_IDLE;
        return;
    }

    // bash → client
    if (FD_ISSET(s->master_fd, ready)) {
        int n = read(s->master_fd, buf, sizeof(buf));
        if (n <= 0) {
            fprintf(stderr, "[daemon] Bash exited (session %d)\n",
                    c->session_id);
            // tell client, then clean up
            write(c->fd, "\r\n[pane exited]\r\n", 17);
            close(c->fd);
            c->fd = -1;
            c->state = CLIENT_IDLE;
            kill_session(c->session_id);
            return; // client fd is dead, don't touch it below
        }
        write(c->fd, buf, n);
    }

    // client keyboard → bash
    // guard: session may have just died above
    if (c->fd < 0) return;
    if (FD_ISSET(c->fd, ready)) {
        int n = read(c->fd, buf, sizeof(buf));
        if (n <= 0) {
            fprintf(stderr, "[daemon] Client detached (session %d)\n",
                    c->session_id);
            close(c->fd);
            c->fd = -1;
            c->state = CLIENT_IDLE;
            return;
        }
        write(s->master_fd, buf, n);
    }
}

// ─────────────────────────────────────────────
// switch_client_to_session()
//
// Moves a client to a different session.
// Updates session_id and resizes the PTY
// to match the client's terminal dimensions.
// ─────────────────────────────────────────────

static void switch_client_to_session(Client *c, int session_id) {
    c->session_id = session_id;
    Session *s = get_session(session_id);
    if (s && c->term_rows > 0) {
        struct winsize ws;
        ws.ws_row    = (unsigned short)c->term_rows;
        ws.ws_col    = (unsigned short)c->term_cols;
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;
        ioctl(s->master_fd, TIOCSWINSZ, &ws);
    }
}

// ─────────────────────────────────────────────
// handle_command()
// ─────────────────────────────────────────────

static void handle_command(int ci) {
    Client   *c = &clients[ci];
    Command   cmd;
    Response  resp;

    memset(&cmd,  0, sizeof(cmd));
    memset(&resp, 0, sizeof(resp));

    int n = read(c->fd, &cmd, sizeof(cmd));
    if (n <= 0) {
        fprintf(stderr, "[daemon] Client disconnected\n");
        close(c->fd);
        c->fd    = -1;
        c->state = CLIENT_IDLE;
        return;
    }

    switch (cmd.type) {

        case CMD_NEW: {
            int id = create_session();
            if (id < 0) {
                resp.success = 0;
                snprintf(resp.message, sizeof(resp.message),
                         "Failed to create session");
            } else {
                resp.success    = 1;
                resp.session_id = id;
                snprintf(resp.message, sizeof(resp.message),
                         "Created session %d", id);
            }
            write(c->fd, &resp, sizeof(resp));
            break;
        }

        case CMD_LIST: {
            resp.success = 1;
            list_sessions(resp.message, sizeof(resp.message));
            write(c->fd, &resp, sizeof(resp));
            break;
        }

        case CMD_KILL: {
            int r = kill_session(cmd.session_id);
            resp.success = (r == 0);
            snprintf(resp.message, sizeof(resp.message),
                     r == 0 ? "Killed session %d"
                             : "Session %d not found",
                     cmd.session_id);
            write(c->fd, &resp, sizeof(resp));
            break;
        }

        case CMD_ATTACH: {
            Session *s = get_session(cmd.session_id);
            if (!s) {
                resp.success = 0;
                snprintf(resp.message, sizeof(resp.message),
                         "Session %d not found", cmd.session_id);
                write(c->fd, &resp, sizeof(resp));
                break;
            }

            resp.success    = 1;
            resp.session_id = cmd.session_id;
            snprintf(resp.message, sizeof(resp.message),
                     "Attached to session %d", cmd.session_id);
            write(c->fd, &resp, sizeof(resp));

            c->state      = CLIENT_ATTACHED;
            c->session_id = cmd.session_id;

            // resize PTY to client's terminal size
            if (c->term_rows > 0) {
                struct winsize ws;
                ws.ws_row    = (unsigned short)c->term_rows;
                ws.ws_col    = (unsigned short)c->term_cols;
                ws.ws_xpixel = 0;
                ws.ws_ypixel = 0;
                ioctl(s->master_fd, TIOCSWINSZ, &ws);
            }

            fprintf(stderr, "[daemon] Client attached to session %d\n",
                    cmd.session_id);
            break;
        }

        // ── SPLIT_H: split active pane horizontally ──
        // creates a new session for the bottom pane
        // switches client to the new session immediately
        case CMD_SPLIT_H: {
            int new_idx = pane_split_h(active_pane);
            if (new_idx < 0) {
                resp.success = 0;
                snprintf(resp.message, sizeof(resp.message),
                         "Cannot split: pane too small or max panes reached");
                write(c->fd, &resp, sizeof(resp));
                break;
            }

            int id = create_session();
            if (id < 0) {
                resp.success = 0;
                snprintf(resp.message, sizeof(resp.message),
                         "Cannot split: failed to create session");
                write(c->fd, &resp, sizeof(resp));
                break;
            }

            panes[new_idx].session_id = id;
            active_pane = new_idx;

            // resize new session to its pane size
            Session *s = get_session(id);
            if (s) pane_resize_session(new_idx, s->master_fd);

            // switch this client to the new session
            switch_client_to_session(c, id);

            resp.success    = 1;
            resp.session_id = id;
            snprintf(resp.message, sizeof(resp.message),
                     "Split horizontal: pane %d session %d",
                     new_idx, id);
            write(c->fd, &resp, sizeof(resp));
            break;
        }

        // ── SPLIT_V: split active pane vertically ──
        case CMD_SPLIT_V: {
            int new_idx = pane_split_v(active_pane);
            if (new_idx < 0) {
                resp.success = 0;
                snprintf(resp.message, sizeof(resp.message),
                         "Cannot split: pane too small or max panes reached");
                write(c->fd, &resp, sizeof(resp));
                break;
            }

            int id = create_session();
            if (id < 0) {
                resp.success = 0;
                snprintf(resp.message, sizeof(resp.message),
                         "Cannot split: failed to create session");
                write(c->fd, &resp, sizeof(resp));
                break;
            }

            panes[new_idx].session_id = id;
            active_pane = new_idx;

            Session *s = get_session(id);
            if (s) pane_resize_session(new_idx, s->master_fd);

            switch_client_to_session(c, id);

            resp.success    = 1;
            resp.session_id = id;
            snprintf(resp.message, sizeof(resp.message),
                     "Split vertical: pane %d session %d",
                     new_idx, id);
            write(c->fd, &resp, sizeof(resp));
            break;
        }

        // ── NEXT_PANE ──
        case CMD_NEXT_PANE: {
            int next = pane_next(active_pane);
            active_pane = next;
            int sid = panes[next].session_id;

            switch_client_to_session(c, sid);

            resp.success    = 1;
            resp.session_id = sid;
            snprintf(resp.message, sizeof(resp.message),
                     "Pane %d (session %d)", next, sid);
            write(c->fd, &resp, sizeof(resp));
            break;
        }

        // ── PREV_PANE ──
        case CMD_PREV_PANE: {
            int prev = pane_prev(active_pane);
            active_pane = prev;
            int sid = panes[prev].session_id;

            switch_client_to_session(c, sid);

            resp.success    = 1;
            resp.session_id = sid;
            snprintf(resp.message, sizeof(resp.message),
                     "Pane %d (session %d)", prev, sid);
            write(c->fd, &resp, sizeof(resp));
            break;
        }

        // ── KILL_PANE: kill current pane's session ──
        case CMD_KILL_PANE: {
            int sid = panes[active_pane].session_id;
            kill_session(sid);
            panes[active_pane].active = 0;
            pane_count--;

            int prev = pane_next(active_pane); // use next, not prev
            if (prev != active_pane) {         // pane_next returns current if none found
                active_pane = prev;
                int nsid = panes[prev].session_id;
                switch_client_to_session(c, nsid);
                resp.success = 1;
                resp.session_id = nsid;
                snprintf(resp.message, sizeof(resp.message),
                        "Killed pane, now on pane %d", prev);
            } else {
                // no panes left
                resp.success = 0;
                snprintf(resp.message, sizeof(resp.message), "No panes left");
            }
            write(c->fd, &resp, sizeof(resp));
            break;
        }

        // ── NEXT_WIN: next session (ignoring panes) ──
        case CMD_NEXT_WIN: {
            int cur  = c->session_id;
            int next = -1;
            for (int i = 1; i <= MAX_SESSIONS; i++) {
                int try = (cur % MAX_SESSIONS) + i;
                if (try > MAX_SESSIONS) try -= MAX_SESSIONS;
                Session *ns = get_session(try);
                if (ns && try != cur) { next = try; break; }
            }
            if (next < 0) {
                resp.success = 0;
                snprintf(resp.message, sizeof(resp.message),
                         "No other sessions");
            } else {
                switch_client_to_session(c, next);
                resp.success    = 1;
                resp.session_id = next;
                snprintf(resp.message, sizeof(resp.message),
                         "Switched to session %d", next);
            }
            write(c->fd, &resp, sizeof(resp));
            break;
        }

        // ── RESIZE: client terminal was resized ──
        case CMD_RESIZE: {
            c->term_rows = cmd.rows;
            c->term_cols = cmd.cols;
            Session *s = get_session(c->session_id);
            if (s) {
                struct winsize ws;
                ws.ws_row    = (unsigned short)cmd.rows;
                ws.ws_col    = (unsigned short)cmd.cols;
                ws.ws_xpixel = 0;
                ws.ws_ypixel = 0;
                ioctl(s->master_fd, TIOCSWINSZ, &ws);
            }
            resp.success = 1;
            write(c->fd, &resp, sizeof(resp));
            break;
        }

        default:
            fprintf(stderr, "[daemon] Unknown command %d\n", cmd.type);
            break;
    }
}

// ─────────────────────────────────────────────
// run_daemon()
// ─────────────────────────────────────────────

void run_daemon() {
    // double fork to daemonize
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid > 0) exit(0);

    setsid();

    pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid > 0) exit(0);

    close(STDIN_FILENO);
    close(STDOUT_FILENO);

    fprintf(stderr, "[daemon] Started (pid %d)\n", getpid());

    signal(SIGCHLD, SIG_IGN);

    // init pane table with a default size
    // will be updated when first client connects with CMD_RESIZE
    pane_init(24, 80);

    // create unix socket
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("[daemon] socket"); exit(1); }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    unlink(SOCKET_PATH);
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[daemon] bind"); exit(1);
    }
    if (listen(server_fd, 8) < 0) {
        perror("[daemon] listen"); exit(1);
    }

    fprintf(stderr, "[daemon] Listening on %s\n", SOCKET_PATH);

    // init client table
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd         = -1;
        clients[i].term_rows  = 24;
        clients[i].term_cols  = 80;
    }

    // main event loop
    while (1) {
        fd_set fds;
        FD_ZERO(&fds);

        FD_SET(server_fd, &fds);
        int maxfd = server_fd;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd < 0) continue;
            FD_SET(clients[i].fd, &fds);
            if (clients[i].fd > maxfd) maxfd = clients[i].fd;
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd < 0)                  continue;
            if (clients[i].state != CLIENT_ATTACHED) continue;
            Session *s = get_session(clients[i].session_id);
            if (!s) continue;
            FD_SET(s->master_fd, &fds);
            if (s->master_fd > maxfd) maxfd = s->master_fd;
        }

        int ready = select(maxfd + 1, &fds, NULL, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("[daemon] select");
            break;
        }

        // new connection
        if (FD_ISSET(server_fd, &fds)) {
            int cfd = accept(server_fd, NULL, NULL);
            if (cfd >= 0) {
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].fd < 0) { slot = i; break; }
                }
                if (slot >= 0) {
                    clients[slot].fd         = cfd;
                    clients[slot].state      = CLIENT_IDLE;
                    clients[slot].session_id = 0;
                    clients[slot].term_rows  = 24;
                    clients[slot].term_cols  = 80;
                    fprintf(stderr, "[daemon] Client connected (slot %d)\n",
                            slot);
                } else {
                    fprintf(stderr, "[daemon] Max clients reached\n");
                    close(cfd);
                }
            }
        }

        // service existing clients
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd < 0) continue;
            if (clients[i].state == CLIENT_ATTACHED)
                handle_attach_data(i, &fds);
            else if (FD_ISSET(clients[i].fd, &fds))
                handle_command(i);
        }
    }
}
