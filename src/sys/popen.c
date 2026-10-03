/*
 * popen.c — LuaFan v2 subprocess management (M12).
 *
 * Ground rules (documented once so the file stays short):
 *
 *   * The C layer offers a pull-based API mirroring fan.tcp: spawn returns
 *     a userdata whose :recv() yields until the next chunk of stdout/stderr
 *     arrives or the child exits. v1's callback shape (onread/onstderr/
 *     ondisconnected) is reimplemented as a thin Lua shim in
 *     lua/fan/popen.lua that spawns a coroutine to loop on :recv().
 *
 *   * The child is launched with a plain fork()+execvp() (no posix_spawn:
 *     it's simpler, portable, and lets us do the pty dance in-line).
 *
 *   * SIGCHLD is intentionally not caught. We reap the child inside
 *     :recv() when both stdout and stderr have hit EOF. This matches v1
 *     behaviour and avoids fighting other libraries that install their
 *     own SIGCHLD handlers.
 *
 *   * With `pty=true` a single pty pair replaces stdin+stdout (stderr is
 *     merged onto stdout, which is the pty convention). set_winsize is
 *     only valid on pty processes.
 */
/* _GNU_SOURCE is set project-wide via CMake; not redefined here. */
#include "popen.h"
#include "../platform.h"
#include "../runtime/loop.h"
#include "../runtime/coro.h"

#include <lauxlib.h>
#include <event2/event.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#  include <sys/syscall.h>
#endif
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* forkpty lives in <pty.h> (Linux/glibc) or <util.h> (BSD/macOS). */
#if defined(__linux__)
#  include <pty.h>
#else
#  include <util.h>
#endif

#define POPEN_MT       "fan.popen"
#define POPEN_READ_BUF 4096

/* Module-level main lua_State captured in register(). Used by
 * event-callback code that has to resume coroutines through
 * fan_coro_wake (which needs main_L to unref registry pins). */
static lua_State *g_popen_L = NULL;

/* One pending recv() per stream. When both stdout & stderr have EOF'd
 * and the child has been reaped, :recv() returns nil,"exit",code. */
typedef struct popen_s popen_t;

/* A queued chunk. In practice we buffer at most one read-buffer worth
 * because :recv() drains before the event loop can produce another
 * chunk, but a small ring keeps the code robust when the reader is
 * slower than the writer. */
typedef struct chunk_s {
    struct chunk_s *next;
    int             which;     /* 1 = stdout, 2 = stderr */
    size_t          len;
    char            data[];
} chunk_t;

struct popen_s {
    pid_t              pid;

    int                stdin_fd;
    int                stdout_fd;
    int                stderr_fd;    /* -1 when merged (pty) or !capture_stderr */

    struct event      *stdout_ev;
    struct event      *stderr_ev;

    int                is_pty;
    int                process_group;

    /* Reap state. Once both read events are gone we run waitpid() and
     * latch the result here; :recv() surfaces it. */
    int                stdout_eof;
    int                stderr_eof;   /* pre-set to 1 when there is no stderr fd */
    int                reaped;
    int                exit_code;
    const char        *exit_reason;  /* "exit" | signal name */

    /* Chunk queue drained by :recv(). Newest at tail. */
    chunk_t           *head;
    chunk_t           *tail;

    /* Parked reader coroutine (at most one at a time). */
    lua_State         *waiter;
    int                waiter_ref;

    int                closed;       /* idempotency for :close() and __gc */
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static popen_t *popen_check(lua_State *L, int idx) {
    popen_t **slot = (popen_t **)luaL_checkudata(L, idx, POPEN_MT);
    if (!*slot) luaL_error(L, "popen is closed");
    return *slot;
}

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void set_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFD, 0);
    if (fl >= 0) fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

static void close_inherited_fds(void) {
#if defined(__linux__) && defined(SYS_close_range)
    if (syscall(SYS_close_range, 3u, UINT_MAX, 0u) == 0) return;
#endif
    long max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 1048576) max_fd = 1024;
    for (int fd = 3; fd < max_fd; fd++) close(fd);
}

static void queue_chunk(popen_t *p, int which, const char *data, size_t len) {
    chunk_t *c = (chunk_t *)malloc(sizeof(chunk_t) + len);
    if (!c) return;   /* LCOV_EXCL_LINE  drop chunk on OOM */
    c->next  = NULL;
    c->which = which;
    c->len   = len;
    memcpy(c->data, data, len);
    if (p->tail) p->tail->next = c; else p->head = c;
    p->tail = c;
}

static void free_chunks(popen_t *p) {
    while (p->head) {
        chunk_t *n = p->head->next;
        free(p->head);
        p->head = n;
    }
    p->tail = NULL;
}

/* Try to reap the child once both stdout & stderr have EOF'd. Latches
 * exit_reason + exit_code. Idempotent. */
static void reap_child(popen_t *p) {
    if (p->reaped || p->pid <= 0) return;
    if (!p->stdout_eof || !p->stderr_eof) return;

    int status = 0;
    /* v1 retried WNOHANG up to 10ms; we do the same so a child that
     * closes its pipes and then does one last teardown syscall still
     * gets reaped inside :recv() without stalling the event loop. */
    for (int i = 0; i < 10; i++) {
        pid_t r = waitpid(p->pid, &status, WNOHANG);
        if (r == p->pid) {
            if (WIFEXITED(status)) {
                p->exit_code   = WEXITSTATUS(status);
                p->exit_reason = "exit";
            } else if (WIFSIGNALED(status)) {
                p->exit_code   = 128 + WTERMSIG(status);
                p->exit_reason = "signal";
            } else {                                        /* LCOV_EXCL_START */
                /* Stopped/continued statuses: not reachable via WNOHANG
                 * without WUNTRACED; kept for safety. */
                p->exit_code   = -1;
                p->exit_reason = "unknown";
            }                                               /* LCOV_EXCL_STOP */
            p->reaped = 1;
            p->pid = -1;
            return;
        }
        if (r < 0) {                                        /* LCOV_EXCL_START */
            /* ECHILD: already reaped (shouldn't happen); treat as done. */
            p->exit_code = -1;
            p->exit_reason = "reaped";
            p->reaped = 1;
            p->pid = -1;
            return;
        }                                                   /* LCOV_EXCL_STOP */
        /* r == 0: not yet exited. Yield 1ms and retry. */
        usleep(1000);
    }
    /* Still running after 10ms: leave reaped=0. The next EOF/recv cycle
     * will retry. (In practice only reachable if the child is in D-state.) */
}

/* Wake the parked reader coroutine with whatever is now available.
 * Handles three states:
 *   1. Queued chunk    -> resume with (data, "stdout"|"stderr")
 *   2. Reaped + no data -> resume with (nil, reason, code)
 *   3. Both EOF but not yet reaped -> attempt reap, else stay parked
 *      (a later on_readable / reap cycle will retry).
 */
static void wake_waiter(popen_t *p) {
    if (!p->waiter) return;

    lua_State *co  = p->waiter;
    int        ref = p->waiter_ref;

    if (p->head) {
        p->waiter = NULL;
        p->waiter_ref = LUA_NOREF;
        chunk_t *c = p->head;
        p->head = c->next;
        if (!p->head) p->tail = NULL;
        lua_pushlstring(co, c->data, c->len);
        lua_pushstring(co, c->which == 1 ? "stdout" : "stderr");
        free(c);
        fan_coro_wake(g_popen_L, co, ref, 2);
        return;
    }
    if (p->stdout_eof && p->stderr_eof) {
        if (!p->reaped) reap_child(p);
        if (p->reaped) {
            p->waiter = NULL;
            p->waiter_ref = LUA_NOREF;
            lua_pushnil(co);
            lua_pushstring(co, p->exit_reason ? p->exit_reason : "exit");
            lua_pushinteger(co, p->exit_code);
            fan_coro_wake(g_popen_L, co, ref, 3);
        }
    }
    /* else: still parked, waiting for more data or reap completion. */
}

/* ------------------------------------------------------------------ */
/* Read event callbacks                                                */
/* ------------------------------------------------------------------ */

static void on_readable(evutil_socket_t fd, short what, void *arg);

static void arm_read_event(struct event **slot, struct event_base *base,
                           int fd, void *arg) {
    *slot = event_new(base, fd, EV_READ | EV_PERSIST, on_readable, arg);
    if (*slot) event_add(*slot, NULL);
}

/* Shared body for stdout and stderr; disambiguates by comparing fd. */
static void on_readable(evutil_socket_t fd, short what, void *arg) {
    (void)what;
    popen_t *p = (popen_t *)arg;
    int which = (fd == p->stdout_fd) ? 1 : 2;

    char buf[POPEN_READ_BUF];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
        queue_chunk(p, which, buf, (size_t)n);
    } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        /* EOF or unrecoverable error. Tear down the event and mark EOF. */
        if (which == 1) {
            if (p->stdout_ev) { event_free(p->stdout_ev); p->stdout_ev = NULL; }
            p->stdout_eof = 1;
        } else {
            if (p->stderr_ev) { event_free(p->stderr_ev); p->stderr_ev = NULL; }
            p->stderr_eof = 1;
        }
        reap_child(p);
    } else {                                    /* LCOV_EXCL_START */
        /* EAGAIN: spurious wakeup, wait for the next EV_READ. */
        return;
    }                                           /* LCOV_EXCL_STOP */

    /* Something changed; poke the parked reader (if any). */
    wake_waiter(p);
}

/* ------------------------------------------------------------------ */
/* Spawn                                                               */
/* ------------------------------------------------------------------ */

/* Split a plain "cat -n foo" style command string on whitespace.
 * Simple, matches v1: no shell metachar handling — pass an array to get
 * anything more elaborate. Returns argv terminated with NULL, or NULL
 * on failure; caller frees argv[0] (the strdup) and argv itself. */
static char **split_cmd_string(const char *s, char **buf_out) {
    char *dup = strdup(s);
    if (!dup) return NULL;                         /* LCOV_EXCL_LINE */
    int cap = 8;
    char **argv = (char **)malloc(sizeof(char *) * (size_t)cap);
    if (!argv) { free(dup); return NULL; }         /* LCOV_EXCL_LINE */
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(dup, " \t", &save);
         tok;
         tok = strtok_r(NULL, " \t", &save)) {
        if (n + 1 >= cap) {
            cap *= 2;
            char **nn = (char **)realloc(argv, sizeof(char *) * (size_t)cap);
            if (!nn) { free(dup); free(argv); return NULL; }   /* LCOV_EXCL_LINE */
            argv = nn;
        }
        argv[n++] = tok;
    }
    argv[n] = NULL;
    *buf_out = dup;
    return argv;
}

/* Build argv from a Lua table (top of stack). Returns malloc'd argv,
 * NULL-terminated. Caller frees argv (strings are pointers into Lua). */
static char **build_argv_from_table(lua_State *L, int tidx) {
#if LUA_VERSION_NUM >= 502
    lua_Integer n = luaL_len(L, tidx);
#else
    lua_Integer n = (lua_Integer)lua_objlen(L, tidx);
#endif
    if (n <= 0) return NULL;
    char **argv = (char **)malloc(sizeof(char *) * (size_t)(n + 1));
    if (!argv) return NULL;                         /* LCOV_EXCL_LINE */
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, tidx, (int)i);
        argv[i - 1] = (char *)luaL_checkstring(L, -1);
        lua_pop(L, 1);
    }
    argv[n] = NULL;
    return argv;
}

/* Build envp from the `env` table + inherited environ. Missing entry
 * inherits; supplied entry overrides. Returned array is malloc'd; each
 * user entry is a fresh strdup ("K=V") — caller frees strings then array. */
static char **build_envp(lua_State *L, int tidx) {
    extern char **environ;
    if (lua_isnoneornil(L, tidx)) return NULL;
    luaL_checktype(L, tidx, LUA_TTABLE);

    /* Collect user entries first (they win over inherited env). */
    int cap = 16, count = 0;
    char **out = (char **)malloc(sizeof(char *) * (size_t)cap);
    if (!out) return NULL;                                            /* LCOV_EXCL_LINE */

    lua_pushnil(L);
    while (lua_next(L, tidx) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TSTRING) {
            const char *k = lua_tostring(L, -2);
            const char *v = lua_tostring(L, -1);
            size_t klen = strlen(k), vlen = strlen(v);
            char *entry = (char *)malloc(klen + 1 + vlen + 1);
            if (!entry) { lua_pop(L, 2); goto err; }                  /* LCOV_EXCL_LINE */
            memcpy(entry, k, klen);
            entry[klen] = '=';
            memcpy(entry + klen + 1, v, vlen);
            entry[klen + 1 + vlen] = '\0';
            if (count + 1 >= cap) {
                cap *= 2;
                char **nn = (char **)realloc(out, sizeof(char *) * (size_t)cap);
                if (!nn) { free(entry); lua_pop(L, 2); goto err; }    /* LCOV_EXCL_LINE */
                out = nn;
            }
            out[count++] = entry;
        }
        lua_pop(L, 1);
    }

    /* Append inherited environ entries whose KEY isn't already shadowed. */
    for (int i = 0; environ[i]; i++) {
        const char *e = environ[i];
        const char *eq = strchr(e, '=');
        size_t klen = eq ? (size_t)(eq - e) : strlen(e);
        int shadowed = 0;
        for (int j = 0; j < count; j++) {
            const char *o = out[j];
            const char *oeq = strchr(o, '=');
            size_t olen = oeq ? (size_t)(oeq - o) : strlen(o);
            if (olen == klen && memcmp(o, e, klen) == 0) { shadowed = 1; break; }
        }
        if (!shadowed) {
            if (count + 1 >= cap) {
                cap *= 2;
                char **nn = (char **)realloc(out, sizeof(char *) * (size_t)cap);
                if (!nn) goto err;                                    /* LCOV_EXCL_LINE */
                out = nn;
            }
            /* Inherited pointer, do NOT free it later — mark by aliasing. */
            out[count++] = (char *)e;
        }
    }
    out[count] = NULL;
    return out;

err:                                                                    /* LCOV_EXCL_START */
    /* Free only entries we malloc'd; inherited env pointers aliased in. */
    for (int i = 0; i < count; i++) {
        char *p = out[i];
        int inherited = 0;
        for (int j = 0; environ[j]; j++) if (environ[j] == p) { inherited = 1; break; }
        if (!inherited) free(p);
    }
    free(out);
    return NULL;
                                                                        /* LCOV_EXCL_STOP */
}

static void free_owned_envp(char **envp) {
    extern char **environ;
    if (!envp) return;
    for (int i = 0; envp[i]; i++) {
        int inherited = 0;
        for (int j = 0; environ[j]; j++) if (environ[j] == envp[i]) { inherited = 1; break; }
        if (!inherited) free(envp[i]);
    }
    free(envp);
}

/* fan.popen.spawn{ command=..., ... } */
static int l_spawn(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    /* --- command --- */
    lua_getfield(L, 1, "command");
    char **argv = NULL, *argv_strdup = NULL;
    int ct = lua_type(L, -1);
    if (ct == LUA_TSTRING) {
        argv = split_cmd_string(lua_tostring(L, -1), &argv_strdup);
    } else if (ct == LUA_TTABLE) {
        argv = build_argv_from_table(L, lua_gettop(L));
    } else {
        return luaL_error(L, "popen.spawn: command must be a string or an array");
    }
    if (!argv || !argv[0]) {
        free(argv); free(argv_strdup);
        return luaL_error(L, "popen.spawn: command is empty");
    }
    lua_pop(L, 1);

    /* --- flags --- */
    int capture_stderr = 1;
    lua_getfield(L, 1, "capture_stderr");
    if (!lua_isnil(L, -1)) capture_stderr = lua_toboolean(L, -1);
    lua_pop(L, 1);

    int process_group = 0;
    lua_getfield(L, 1, "process_group");
    if (!lua_isnil(L, -1)) process_group = lua_toboolean(L, -1);
    lua_pop(L, 1);

    int want_pty = 0;
    lua_getfield(L, 1, "pty");
    if (!lua_isnil(L, -1)) want_pty = lua_toboolean(L, -1);
    lua_pop(L, 1);

    /* --- env --- */
    lua_getfield(L, 1, "env");
    char **envp = build_envp(L, lua_gettop(L));
    lua_pop(L, 1);

    /* --- pipe / pty setup ------------------------------------------- */
    int p_in[2]  = {-1, -1};
    int p_out[2] = {-1, -1};
    int p_err[2] = {-1, -1};
    int pty_master = -1, pty_slave = -1;

    if (want_pty) {
        if (openpty(&pty_master, &pty_slave, NULL, NULL, NULL) < 0) {   /* LCOV_EXCL_START */
            free(argv); free(argv_strdup); free_owned_envp(envp);
            lua_pushnil(L);
            lua_pushfstring(L, "openpty: %s", strerror(errno));
            return 2;
        }                                                               /* LCOV_EXCL_STOP */
    } else {
        if (pipe(p_in) < 0 || pipe(p_out) < 0
                || (capture_stderr && pipe(p_err) < 0)) {               /* LCOV_EXCL_START */
            int save = errno;
            for (int i = 0; i < 2; i++) {
                if (p_in[i]  >= 0) close(p_in[i]);
                if (p_out[i] >= 0) close(p_out[i]);
                if (p_err[i] >= 0) close(p_err[i]);
            }
            free(argv); free(argv_strdup); free_owned_envp(envp);
            lua_pushnil(L);
            lua_pushfstring(L, "pipe: %s", strerror(save));
            return 2;
        }                                                               /* LCOV_EXCL_STOP */
    }

    /* --- fork ------------------------------------------------------- */
    pid_t pid = fork();
    if (pid < 0) {                                                      /* LCOV_EXCL_START */
        int save = errno;
        if (want_pty) { close(pty_master); close(pty_slave); }
        else {
            close(p_in[0]);  close(p_in[1]);
            close(p_out[0]); close(p_out[1]);
            if (capture_stderr) { close(p_err[0]); close(p_err[1]); }
        }
        free(argv); free(argv_strdup); free_owned_envp(envp);
        lua_pushnil(L);
        lua_pushfstring(L, "fork: %s", strerror(save));
        return 2;
    }                                                                   /* LCOV_EXCL_STOP */

    if (pid == 0) {
        /* ---- CHILD ---- */
        /* gcov cannot instrument this branch: execvp replaces the process
         * image before any gcov flush can run. Correctness is validated
         * end-to-end (pty attach, stdout capture, exit codes). */
        /* LCOV_EXCL_START */
        if (process_group) setpgid(0, 0);
        if (want_pty) {
            /* setsid + TIOCSCTTY makes the pty our controlling terminal. */
            setsid();
            ioctl(pty_slave, TIOCSCTTY, 0);
            dup2(pty_slave, STDIN_FILENO);
            dup2(pty_slave, STDOUT_FILENO);
            dup2(pty_slave, STDERR_FILENO);
            if (pty_slave > 2) close(pty_slave);
            close(pty_master);
        } else {
            dup2(p_in[0],  STDIN_FILENO);
            dup2(p_out[1], STDOUT_FILENO);
            if (capture_stderr) dup2(p_err[1], STDERR_FILENO);
            /* Close all pipe fds in the child; dup2 targets already
             * carry the payload. */
            close(p_in[0]);  close(p_in[1]);
            close(p_out[0]); close(p_out[1]);
            if (capture_stderr) { close(p_err[0]); close(p_err[1]); }
        }
        close_inherited_fds();
        if (envp) {
#if defined(__linux__) && defined(__GLIBC__)
            execvpe(argv[0], argv, envp);
#else
            /* macOS/BSD lack execvpe: swap `environ` and use execvp. Safe
             * inside the child since it has its own address space. */
            extern char **environ;
            environ = envp;
            execvp(argv[0], argv);
#endif
        } else {
            execvp(argv[0], argv);
        }
        /* execvp only returns on failure. */
        _exit(127);
        /* LCOV_EXCL_STOP */
    }

    /* ---- PARENT ---- */
    /* No fan_loop_reinit_after_fork here: reinit is only needed in the
     * child, and this fork's child is about to execvp anyway (dropping
     * all inherited fds via CLOEXEC + explicit dup2). */
    if (want_pty) {
        close(pty_slave);
    } else {
        close(p_in[0]);
        close(p_out[1]);
        if (capture_stderr) close(p_err[1]);
    }

    free(argv); free(argv_strdup); free_owned_envp(envp);

    /* --- build userdata --------------------------------------------- */
    popen_t **slot = (popen_t **)lua_newuserdata(L, sizeof(popen_t *));
    *slot = NULL;
    popen_t *p = (popen_t *)calloc(1, sizeof(popen_t));
    if (!p) {                                                           /* LCOV_EXCL_START */
        if (want_pty) close(pty_master);
        else { close(p_in[1]); close(p_out[0]); if (capture_stderr) close(p_err[0]); }
        kill(pid, SIGKILL); waitpid(pid, NULL, 0);
        return luaL_error(L, "out of memory");
    }                                                                   /* LCOV_EXCL_STOP */
    *slot = p;
    p->pid = pid;
    p->process_group = process_group;
    p->is_pty = want_pty;
    p->waiter_ref = LUA_NOREF;

    if (want_pty) {
        p->stdin_fd  = pty_master;   /* one fd, bidirectional */
        p->stdout_fd = pty_master;
        p->stderr_fd = -1;
        p->stderr_eof = 1;           /* no stderr stream */
    } else {
        p->stdin_fd  = p_in[1];
        p->stdout_fd = p_out[0];
        p->stderr_fd = capture_stderr ? p_err[0] : -1;
        if (!capture_stderr) p->stderr_eof = 1;
    }

    set_nonblock(p->stdin_fd);
    set_nonblock(p->stdout_fd);
    if (p->stderr_fd >= 0) set_nonblock(p->stderr_fd);
    set_cloexec(p->stdin_fd);
    set_cloexec(p->stdout_fd);
    if (p->stderr_fd >= 0) set_cloexec(p->stderr_fd);

    luaL_getmetatable(L, POPEN_MT);
    lua_setmetatable(L, -2);

    struct event_base *base = fan_loop_current_base();
    arm_read_event(&p->stdout_ev, base, p->stdout_fd, p);
    if (p->stderr_fd >= 0) arm_read_event(&p->stderr_ev, base, p->stderr_fd, p);

    return 1;
}

/* ------------------------------------------------------------------ */
/* Methods                                                             */
/* ------------------------------------------------------------------ */

/* popen:recv() -> data, "stdout"|"stderr"  |  nil, "exit", code
 * Yields until the next chunk lands or the child exits. */
static int l_recv(lua_State *L) {
    popen_t *p = popen_check(L, 1);

    /* Fast path: a chunk is already queued. */
    if (p->head) {
        chunk_t *c = p->head;
        p->head = c->next;
        if (!p->head) p->tail = NULL;
        lua_pushlstring(L, c->data, c->len);
        lua_pushstring(L, c->which == 1 ? "stdout" : "stderr");
        free(c);
        return 2;
    }

    /* Both streams EOF + reaped: report the exit. Also handle the case
     * where EOF happened but reap_child couldn't reap yet. */
    if (p->stdout_eof && p->stderr_eof) {
        if (!p->reaped) reap_child(p);
        lua_pushnil(L);
        lua_pushstring(L, p->exit_reason ? p->exit_reason : "exit");
        lua_pushinteger(L, p->exit_code);
        return 3;
    }

    /* Park the coroutine and yield; on_readable will wake us. */
    if (!lua_isyieldable(L)) {
        return luaL_error(L, "popen:recv must be called from a coroutine");
    }
    if (p->waiter) return luaL_error(L, "popen:recv already in progress");
    int ref = fan_coro_park(L);
    p->waiter = L;
    p->waiter_ref = ref;
    return lua_yield(L, 0);
}

/* popen:send(data) -> written | nil, err */
static int l_send(lua_State *L) {
    popen_t *p = popen_check(L, 1);
    size_t n = 0;
    const char *s = luaL_optlstring(L, 2, NULL, &n);
    if (p->closed || p->stdin_fd < 0) {
        lua_pushnil(L); lua_pushliteral(L, "stdin is closed"); return 2;
    }
    if (!s || n == 0) { lua_pushinteger(L, 0); return 1; }
    ssize_t w = write(p->stdin_fd, s, n);
    if (w < 0) {
        if (errno == EAGAIN || errno == EINTR) { lua_pushinteger(L, 0); return 1; }
        lua_pushnil(L); lua_pushstring(L, strerror(errno)); return 2;
    }
    lua_pushinteger(L, (lua_Integer)w);
    return 1;
}

/* popen:close_stdin() — pty processes share stdin/stdout, so we refuse
 * to close it there (would kill reads too). */
static int l_close_stdin(lua_State *L) {
    popen_t *p = popen_check(L, 1);
    if (p->is_pty) {
        return luaL_error(L, "close_stdin: not supported on pty processes");
    }
    if (p->stdin_fd >= 0) { close(p->stdin_fd); p->stdin_fd = -1; }
    lua_pushboolean(L, 1);
    return 1;
}

/* popen:set_winsize(rows, cols) — pty only. */
static int l_set_winsize(lua_State *L) {
    popen_t *p = popen_check(L, 1);
    if (!p->is_pty) {
        return luaL_error(L, "set_winsize: only valid on pty processes");
    }
    int rows = (int)luaL_checkinteger(L, 2);
    int cols = (int)luaL_checkinteger(L, 3);
    if (rows <= 0 || cols <= 0 || rows > 65535 || cols > 65535) {
        return luaL_error(L, "set_winsize: rows/cols must be in 1..65535");
    }
    struct winsize ws = {0};
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    if (ioctl(p->stdout_fd, TIOCSWINSZ, &ws) < 0) {
        lua_pushnil(L); lua_pushstring(L, strerror(errno)); return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* Internal teardown; safe to call multiple times. */
static void popen_release(popen_t *p) {
    if (!p || p->closed) return;
    p->closed = 1;

    if (p->stdout_ev) { event_free(p->stdout_ev); p->stdout_ev = NULL; }
    if (p->stderr_ev) { event_free(p->stderr_ev); p->stderr_ev = NULL; }

    if (p->is_pty) {
        if (p->stdout_fd >= 0) { close(p->stdout_fd); p->stdout_fd = -1; }
        p->stdin_fd = -1;   /* same fd */
    } else {
        if (p->stdin_fd  >= 0) { close(p->stdin_fd);  p->stdin_fd  = -1; }
        if (p->stdout_fd >= 0) { close(p->stdout_fd); p->stdout_fd = -1; }
        if (p->stderr_fd >= 0) { close(p->stderr_fd); p->stderr_fd = -1; }
    }

    if (p->pid > 0) {
        pid_t target = p->process_group ? -p->pid : p->pid;
        kill(target, SIGTERM);
        int status = 0;
        pid_t r = waitpid(p->pid, &status, WNOHANG);
        if (r == 0) { usleep(10000); r = waitpid(p->pid, &status, WNOHANG); }
        if (r == 0) {
            kill(target, SIGKILL);
            r = waitpid(p->pid, &status, 0);
        }
        if (r == p->pid) {
            if (WIFEXITED(status)) {
                p->exit_code = WEXITSTATUS(status);
                p->exit_reason = "exit";
            } else if (WIFSIGNALED(status)) {
                p->exit_code = 128 + WTERMSIG(status);
                p->exit_reason = "signal";
            }
            p->reaped = 1;
        }
        p->pid = -1;
    }

    /* close() tears down the read events as well as the child. Mark both
     * streams complete so a later recv() returns the latched exit tuple
     * instead of parking forever waiting for events that were removed. */
    p->stdout_eof = 1;
    p->stderr_eof = 1;
    if (!p->reaped) {
        p->reaped = 1;
        p->exit_code = -1;
        p->exit_reason = "closed";
    }

    free_chunks(p);
}

static int l_close(lua_State *L) {
    popen_t *p = popen_check(L, 1);
    popen_release(p);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_getpid(lua_State *L) {
    popen_t *p = popen_check(L, 1);
    if (p->pid > 0) lua_pushinteger(L, p->pid);
    else            lua_pushnil(L);
    return 1;
}

static int l_is_alive(lua_State *L) {
    popen_t *p = popen_check(L, 1);
    if (p->pid <= 0) { lua_pushboolean(L, 0); return 1; }
    /* Non-blocking wait; if it returns the child's pid we're done. */
    int status = 0;
    pid_t r = waitpid(p->pid, &status, WNOHANG);
    if (r == p->pid) {
        p->pid = -1;
        if (WIFEXITED(status))       { p->exit_code = WEXITSTATUS(status); p->exit_reason = "exit"; }
        else if (WIFSIGNALED(status)){ p->exit_code = 128 + WTERMSIG(status); p->exit_reason = "signal"; }
        p->reaped = 1;
        lua_pushboolean(L, 0);
    } else {
        lua_pushboolean(L, 1);
    }
    return 1;
}

static int l_gc(lua_State *L) {
    popen_t **slot = (popen_t **)luaL_checkudata(L, 1, POPEN_MT);
    popen_t *p = *slot;
    *slot = NULL;
    if (!p) return 0;
    popen_release(p);
    free(p);
    return 0;
}

static int l_tostring(lua_State *L) {
    popen_t **slot = (popen_t **)luaL_checkudata(L, 1, POPEN_MT);
    popen_t *p = *slot;
    if (!p) lua_pushliteral(L, "fan.popen<closed>");
    else lua_pushfstring(L, "fan.popen<pid=%d%s>",
                         (int)p->pid, p->is_pty ? ",pty" : "");
    return 1;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static const luaL_Reg popen_methods[] = {
    {"recv",         l_recv},
    {"send",         l_send},
    {"close_stdin",  l_close_stdin},
    {"set_winsize",  l_set_winsize},
    {"close",        l_close},
    {"getpid",       l_getpid},
    {"is_alive",     l_is_alive},
    {NULL, NULL},
};

static const luaL_Reg popen_funcs[] = {
    {"spawn", l_spawn},
    {NULL, NULL},
};

void fan_popen_register(lua_State *L) {
    g_popen_L = fan_coro_main(L);  /* stable main thread, not the require() coroutine */
    luaL_newmetatable(L, POPEN_MT);
    lua_pushcfunction(L, l_gc);       lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, l_tostring); lua_setfield(L, -2, "__tostring");
    lua_newtable(L);
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, popen_methods, 0);
#else
    luaL_register(L, NULL, popen_methods);
#endif
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    lua_newtable(L);
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, popen_funcs, 0);
#else
    luaL_register(L, NULL, popen_funcs);
#endif
    lua_setfield(L, -2, "popen");
}

void fan_popen_clear_lua_state(void) {
    /* Companion to fan_popen_register — see runtime/coro.h teardown contract.
     * Prevents subprocess stdout / stderr callbacks from waking coroutines
     * on a torn-down lua_State. */
    g_popen_L = NULL;
}
