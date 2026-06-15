#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>

#include "mux.h"

// ─────────────────────────────────────────────
// RAW MODE
// ─────────────────────────────────────────────

static struct termios orig_termios;

static void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

// ─────────────────────────────────────────────
// send_cmd_get_resp()
// sends a command and reads response
// while still in attached state (non-blocking
// from daemon's perspective — it handles it
// as a command even while attached)
// ─────────────────────────────────────────────

static Response send_cmd(int sock_fd, Command cmd) {
    Response resp = {0};
    write(sock_fd, &cmd, sizeof(cmd));
    read(sock_fd, &resp, sizeof(resp));
    return resp;
}

// ─────────────────────────────────────────────
// handle_prefix_key()
//
// Keybinds (after Ctrl+B):
//
//   d       detach
//   -       split horizontal
//   |       split vertical
//   o       next pane
//   p       prev pane
//   x       kill current pane
//   c       new session
//   n       next session
//
// Returns 1 = detach, 0 = continue
// ─────────────────────────────────────────────

static int handle_prefix_key(int sock_fd) {
    // wait up to 1 second for next key
    fd_set tmp;
    FD_ZERO(&tmp);
    FD_SET(STDIN_FILENO, &tmp);
    struct timeval tv = {1, 0};

    if (select(STDIN_FILENO + 1, &tmp, NULL, NULL, &tv) <= 0)
        return 0;  // timeout — ignore

    char key;
    if (read(STDIN_FILENO, &key, 1) <= 0) return 0;

    Command  cmd  = {0};
    Response resp = {0};

    switch (key) {

        case 'd':
        case 'D':
            return 1;  // detach

        case '-':
            // split horizontal
            cmd.type = CMD_SPLIT_H;
            resp = send_cmd(sock_fd, cmd);
            if (!resp.success)
                fprintf(stderr, "\r\n[split failed: %s]\r\n", resp.message);
            else
                fprintf(stderr, "\r\n[%s]\r\n", resp.message);
            break;

        case '|':
            // split vertical
            cmd.type = CMD_SPLIT_V;
            resp = send_cmd(sock_fd, cmd);
            if (!resp.success)
                fprintf(stderr, "\r\n[split failed: %s]\r\n", resp.message);
            else
                fprintf(stderr, "\r\n[%s]\r\n", resp.message);
            break;

        case 'o':
        case '\t':
            // next pane
            cmd.type = CMD_NEXT_PANE;
            resp = send_cmd(sock_fd, cmd);
            fprintf(stderr, "\r\n[%s]\r\n", resp.message);
            break;

        case 'p':
            // prev pane
            cmd.type = CMD_PREV_PANE;
            resp = send_cmd(sock_fd, cmd);
            fprintf(stderr, "\r\n[%s]\r\n", resp.message);
            break;

        case 'x':
            // kill current pane
            cmd.type = CMD_KILL_PANE;
            resp = send_cmd(sock_fd, cmd);
            if (!resp.success) return 1;  // no panes left, detach
            fprintf(stderr, "\r\n[%s]\r\n", resp.message);
            break;

        case 'c':
        case 'C': {
            // new session + switch to it
            cmd.type = CMD_NEW;
            resp = send_cmd(sock_fd, cmd);
            if (resp.success) {
                int new_id = resp.session_id;
                cmd.type       = CMD_ATTACH;
                cmd.session_id = new_id;
                resp = send_cmd(sock_fd, cmd);
                fprintf(stderr, "\r\n[new session %d]\r\n", new_id);
            }
            break;
        }

        case 'n':
        case 'N':
            // next session
            cmd.type = CMD_NEXT_WIN;
            resp = send_cmd(sock_fd, cmd);
            fprintf(stderr, "\r\n[%s]\r\n", resp.message);
            break;

        case '?':
            // show keybinds
            fprintf(stderr,
                "\r\n"
                "[mux keybinds]\r\n"
                "  Ctrl+B d   detach\r\n"
                "  Ctrl+B -   split horizontal\r\n"
                "  Ctrl+B |   split vertical\r\n"
                "  Ctrl+B o   next pane\r\n"
                "  Ctrl+B p   prev pane\r\n"
                "  Ctrl+B x   kill pane\r\n"
                "  Ctrl+B c   new session\r\n"
                "  Ctrl+B n   next session\r\n"
                "  Ctrl+B ?   this help\r\n"
                "\r\n");
            break;

        default:
            // unknown — ignore silently
            break;
    }

    return 0;
}

// ─────────────────────────────────────────────
// send_resize()
// tells daemon our current terminal size
// ─────────────────────────────────────────────

static void send_resize(int sock_fd) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0) return;
    Command cmd = {
        .type = CMD_RESIZE,
        .rows = ws.ws_row,
        .cols = ws.ws_col,
    };
    Response resp;
    write(sock_fd, &cmd, sizeof(cmd));
    read(sock_fd, &resp, sizeof(resp));
}

// ─────────────────────────────────────────────
// run_attach_loop()
// ─────────────────────────────────────────────

// global sock_fd for signal handler
static int g_sock_fd = -1;

static void sigwinch_handler(int sig) {
    (void)sig;
    if (g_sock_fd >= 0) send_resize(g_sock_fd);
}

void run_attach_loop(int sock_fd) {
    char buf[BUF_SIZE];

    g_sock_fd = sock_fd;

    atexit(disable_raw_mode);
    enable_raw_mode();

    // send initial terminal size
    send_resize(sock_fd);

    // handle window resize via signal
    signal(SIGWINCH, sigwinch_handler);

    while (1) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        FD_SET(sock_fd, &fds);

        int ready = select(sock_fd + 1, &fds, NULL, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) continue;  // SIGWINCH interrupted select
            break;
        }

        // keyboard input
        if (FD_ISSET(STDIN_FILENO, &fds)) {
            int n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break;

            // Ctrl+B = 0x02
            if (n == 1 && (unsigned char)buf[0] == 0x02) {
                int detach = handle_prefix_key(sock_fd);
                if (detach) {
                    disable_raw_mode();
                    printf("\r\n[detached]\r\n");
                    return;
                }
                continue;
            }

            write(sock_fd, buf, n);
        }

        // output from daemon/bash
        if (FD_ISSET(sock_fd, &fds)) {
            int n = read(sock_fd, buf, sizeof(buf));
            if (n <= 0) {
                disable_raw_mode();
                printf("\r\n[session ended]\r\n");
                break;
            }
            write(STDOUT_FILENO, buf, n);
        }
    }

    disable_raw_mode();
}

// ─────────────────────────────────────────────
// CONNECT / ENSURE DAEMON
// ─────────────────────────────────────────────

static int connect_to_daemon() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd); return -1;
    }
    return fd;
}

static void ensure_daemon_running() {
    int fd = connect_to_daemon();
    if (fd >= 0) { close(fd); return; }

    fprintf(stderr, "Starting daemon...\n");
    pid_t pid = fork();
    if (pid == 0) {
        // redirect stderr to log
        int log = open("/tmp/mux.log",
                       O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log >= 0) { dup2(log, STDERR_FILENO); close(log); }

        // close stdin/stdout so we don't hold the terminal
        close(STDIN_FILENO);
        close(STDOUT_FILENO);

        // become our own session leader here,
        // so run_daemon doesn't need to double-fork
        setsid();

        run_daemon(); // run_daemon should NOT fork again
        exit(0);
    }
    // parent: don't wait, let child run as daemon
    // reap it so no zombie
    waitpid(pid, NULL, WNOHANG);

    for (int i = 0; i < 20; i++) {
        usleep(100000);
        fd = connect_to_daemon();
        if (fd >= 0) { close(fd); fprintf(stderr, "Daemon ready.\n"); return; }
    }
    fprintf(stderr, "Error: daemon failed. Check /tmp/mux.log\n");
    exit(1);
}

// ─────────────────────────────────────────────
// MAIN
// ─────────────────────────────────────────────

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("mux — terminal multiplexer\n\n");
        printf("Usage:\n");
        printf("  mux new            create session and attach\n");
        printf("  mux list           list sessions\n");
        printf("  mux attach <id>    attach to session\n");
        printf("  mux kill <id>      kill session\n");
        printf("\nInside (Ctrl+B prefix):\n");
        printf("  Ctrl+B d    detach\n");
        printf("  Ctrl+B -    split horizontal\n");
        printf("  Ctrl+B |    split vertical\n");
        printf("  Ctrl+B o    next pane\n");
        printf("  Ctrl+B p    prev pane\n");
        printf("  Ctrl+B x    kill pane\n");
        printf("  Ctrl+B c    new session\n");
        printf("  Ctrl+B n    next session\n");
        printf("  Ctrl+B ?    help\n");
        return 1;
    }

    ensure_daemon_running();

    if (strcmp(argv[1], "new") == 0) {
        int sock_fd = connect_to_daemon();

        Command  cmd  = {.type = CMD_NEW};
        Response resp = {0};
        write(sock_fd, &cmd, sizeof(cmd));
        read(sock_fd, &resp, sizeof(resp));

        if (!resp.success) {
            fprintf(stderr, "Error: %s\n", resp.message);
            close(sock_fd); return 1;
        }
        int new_id = resp.session_id;
        printf("Created session %d. Attaching...\n", new_id);

        memset(&cmd,  0, sizeof(cmd));
        memset(&resp, 0, sizeof(resp));
        cmd.type       = CMD_ATTACH;
        cmd.session_id = new_id;
        write(sock_fd, &cmd, sizeof(cmd));
        read(sock_fd, &resp, sizeof(resp));

        if (!resp.success) {
            fprintf(stderr, "Error: %s\n", resp.message);
            close(sock_fd); return 1;
        }
        run_attach_loop(sock_fd);
        close(sock_fd);

    } else if (strcmp(argv[1], "list") == 0) {
        int sock_fd = connect_to_daemon();
        Command  cmd  = {.type = CMD_LIST};
        Response resp = {0};
        write(sock_fd, &cmd, sizeof(cmd));
        read(sock_fd, &resp, sizeof(resp));
        close(sock_fd);
        printf("Active sessions:\n%s", resp.message);

    } else if (strcmp(argv[1], "attach") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: mux attach <id>\n"); return 1; }
        int sock_fd = connect_to_daemon();
        Command cmd = { .type = CMD_ATTACH, .session_id = atoi(argv[2]) };
        Response resp = {0};
        write(sock_fd, &cmd, sizeof(cmd));
        read(sock_fd, &resp, sizeof(resp));
        if (!resp.success) {
            fprintf(stderr, "Error: %s\n", resp.message);
            close(sock_fd); return 1;
        }
        run_attach_loop(sock_fd);
        close(sock_fd);

    } else if (strcmp(argv[1], "kill") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: mux kill <id>\n"); return 1; }
        int sock_fd = connect_to_daemon();
        Command cmd = { .type = CMD_KILL, .session_id = atoi(argv[2]) };
        Response resp = {0};
        write(sock_fd, &cmd, sizeof(cmd));
        read(sock_fd, &resp, sizeof(resp));
        close(sock_fd);
        printf("%s\n", resp.message);

    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        return 1;
    }

    return 0;
}
