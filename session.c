#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pty.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <signal.h>

#include "mux.h"

// ─────────────────────────────────────────────
// GLOBAL SESSION TABLE
// ─────────────────────────────────────────────

// All sessions live here. Daemon owns this.
// Client never touches this directly.
Session sessions[MAX_SESSIONS];
int     session_count = 0;

// ─────────────────────────────────────────────
// create_session()
//
// Opens a PTY pair, forks a bash process into the
// slave end, and records it in the session table.
// Returns the session id (1-based), or -1 on error.
// ─────────────────────────────────────────────

int create_session() {
    // find an empty slot in the session table
    int slot = -1;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        fprintf(stderr, "[session] No free session slots\n");
        return -1;
    }

    // open PTY pair
    int master_fd, slave_fd;
    struct winsize ws = {24, 80, 0, 0};  // default 24 rows x 80 cols
    if (openpty(&master_fd, &slave_fd, NULL, NULL, &ws) < 0) {
        perror("[session] openpty");
        return -1;
    }

    // fork bash into the slave end
    pid_t pid = fork();
    if (pid < 0) {
        perror("[session] fork");
        return -1;
    }

    if (pid == 0) {
        // ── CHILD: become bash ──

        close(master_fd);  // child doesn't need master

        setsid();  // new session so slave can become controlling terminal
        ioctl(slave_fd, TIOCSCTTY, 0);  // make slave the controlling terminal

        // wire stdin/stdout/stderr to the slave PTY
        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);
        close(slave_fd);  // already duplicated

        char *args[] = {"/bin/bash", NULL};
        execvp("/bin/bash", args);

        perror("[session] execvp");
        exit(1);
    }

    // ── PARENT: record the session ──
    close(slave_fd);  // parent only needs master

    sessions[slot].id        = slot + 1;
    sessions[slot].master_fd = master_fd;
    sessions[slot].bash_pid  = pid;
    sessions[slot].active    = 1;
    snprintf(sessions[slot].name, sizeof(sessions[slot].name),
             "session-%d", slot + 1);

    session_count++;

    fprintf(stderr, "[session] Created session %d (pid %d)\n",
            sessions[slot].id, pid);

    return sessions[slot].id;
}

// ─────────────────────────────────────────────
// kill_session()
//
// Kills the bash process and frees the slot.
// Returns 0 on success, -1 if session not found.
// ─────────────────────────────────────────────

int kill_session(int id) {
    int slot = id - 1;

    if (slot < 0 || slot >= MAX_SESSIONS) {
        fprintf(stderr, "[session] Invalid id %d\n", id);
        return -1;
    }
    if (!sessions[slot].active) {
        fprintf(stderr, "[session] Session %d not active\n", id);
        return -1;
    }

    // kill bash
    kill(sessions[slot].bash_pid, SIGKILL);
    waitpid(sessions[slot].bash_pid, NULL, WNOHANG);

    // close PTY master
    close(sessions[slot].master_fd);

    // free the slot
    sessions[slot].active = 0;
    session_count--;

    fprintf(stderr, "[session] Killed session %d\n", id);
    return 0;
}

// ─────────────────────────────────────────────
// list_sessions()
//
// Writes a human-readable list of active sessions
// into buf. Used by the LIST command response.
// ─────────────────────────────────────────────

void list_sessions(char *buf, int bufsize) {
    int pos   = 0;
    int found = 0;

    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].active) {
            pos += snprintf(buf + pos, bufsize - pos,
                            "  [%d] %s  (bash pid: %d)\n",
                            sessions[i].id,
                            sessions[i].name,
                            sessions[i].bash_pid);
            found = 1;
        }
    }

    if (!found)
        snprintf(buf, bufsize, "  No active sessions.\n");
}

// ─────────────────────────────────────────────
// get_session()
//
// Returns pointer to a session by id, or NULL.
// Used by daemon when it needs master_fd for attach.
// ─────────────────────────────────────────────

Session *get_session(int id) {
    int slot = id - 1;
    if (slot < 0 || slot >= MAX_SESSIONS) return NULL;
    if (!sessions[slot].active)            return NULL;
    return &sessions[slot];
}
