/*
 * sys/posix.c — LuaFan v2 POSIX system services (M6).
 *
 * See sys/posix.h for the API contract.
 *
 * v2 hardening over v1 (see docs/luafan-v2-plan.md §1, "sys/posix"):
 *   - kill() refuses dangerous defaults: pid == 0 (process group), pid == -1
 *     (broadcast), and pid == 1 (init) unless the caller explicitly passes
 *     force=true as the 3rd argument. The 3rd argument may be a boolean
 *     (backwards-friendly) or an options table {force=true}.
 *   - getinterfaces() uses the correct sockaddr length for IPv6 netmasks
 *     (sockaddr_in6, not sockaddr_in), so IPv6 subnets are reported.
 *   - setprogname() is gated to Linux (glibc/musl expose __progname as a
 *     writable pointer). Non-Linux builds get a no-op that returns
 *     nil, "not supported".
 *   - Signal name -> number map is exposed as fan.posix.signals so callers
 *     don't hard-code integers.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "posix.h"
#include "../platform.h"
#include "../runtime/loop.h"

#include <lauxlib.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(FAN_PLATFORM_LINUX) || defined(FAN_PLATFORM_ANDROID)
#  include <sched.h>
#endif

/* ---- small helpers ------------------------------------------------------- */
static int push_errno(lua_State *L, const char *ctx) {
    int e = errno;
    lua_pushnil(L);
    if (ctx) lua_pushfstring(L, "%s: %s", ctx, strerror(e));
    else     lua_pushstring(L, strerror(e));
    return 2;
}


/* ---- process ------------------------------------------------------------- */
static int l_getpid(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)getpid());
    return 1;
}

static int l_fork(lua_State *L) {
    pid_t r = fork();
    if (r == (pid_t)-1) return push_errno(L, "fork");
    if (r == 0) {
        /* child: re-initialise libevent so kqueue/epoll fds inherited
         * from the parent are dropped. Safe even if the base has never
         * been created. */
        fan_loop_reinit_after_fork();
    }
    lua_pushinteger(L, (lua_Integer)r);
    return 1;
}

static int l_waitpid(lua_State *L) {
    pid_t pid  = (pid_t)luaL_optinteger(L, 1, -1);
    int   opts = (int)luaL_optinteger(L, 2, 0);
    int stat = 0;
    pid_t r = waitpid(pid, &stat, opts);
    if (r == (pid_t)-1) return push_errno(L, "waitpid");
    lua_pushinteger(L, (lua_Integer)r);
    lua_pushinteger(L, (lua_Integer)stat);
    return 2;
}

/* kill() with dangerous-PID guardrails. */
static int l_kill(lua_State *L) {
    pid_t pid = (pid_t)luaL_checkinteger(L, 1);
    int   sig = (int)luaL_optinteger(L, 2, SIGTERM);

    /* Accept a boolean or {force=true} table as the 3rd argument. */
    int force = 0;
    int argt = lua_type(L, 3);
    if (argt == LUA_TBOOLEAN) {
        force = lua_toboolean(L, 3);
    } else if (argt == LUA_TTABLE) {
        lua_getfield(L, 3, "force");
        force = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }

    if (!force) {
        const char *why = NULL;
        if (pid == 0)         why = "kill(pid=0) targets the whole process group";
        else if (pid == -1)   why = "kill(pid=-1) broadcasts to every process";
        else if (pid == 1)    why = "kill(pid=1) targets init";
        if (why) {
            lua_pushnil(L);
            lua_pushfstring(L, "refused: %s; pass force=true to override", why);
            return 2;
        }
    }

    if (kill(pid, sig) == -1) return push_errno(L, "kill");
    lua_pushboolean(L, 1);
    return 1;
}

static int l_setpgid(lua_State *L) {
    pid_t pid  = (pid_t)luaL_optinteger(L, 1, 0);
    pid_t pgid = (pid_t)luaL_optinteger(L, 2, 0);
    if (setpgid(pid, pgid) == -1) return push_errno(L, "setpgid");
    lua_pushinteger(L, 0);
    return 1;
}

static int l_getpgid(lua_State *L) {
    pid_t pid = (pid_t)luaL_optinteger(L, 1, 0);
    pid_t r = getpgid(pid);
    if (r == (pid_t)-1) return push_errno(L, "getpgid");
    lua_pushinteger(L, (lua_Integer)r);
    return 1;
}

static int l_setsid(lua_State *L) {
    pid_t r = setsid();
    if (r == (pid_t)-1) return push_errno(L, "setsid");
    lua_pushinteger(L, (lua_Integer)r);
    return 1;
}

/* ---- CPU / affinity ------------------------------------------------------ */
static int cpu_count_online(void) {
#if defined(_SC_NPROCESSORS_CONF)
    long n = sysconf(_SC_NPROCESSORS_CONF);
    return (n > 0) ? (int)n : 1;
#else
    return 1;
#endif
}

static int l_getcpucount(lua_State *L) {
    lua_pushinteger(L, cpu_count_online());
    return 1;
}

#if defined(FAN_PLATFORM_LINUX) || defined(FAN_PLATFORM_ANDROID)
static int l_getaffinity(lua_State *L) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (sched_getaffinity(0, sizeof(mask), &mask) == -1)
        return push_errno(L, "sched_getaffinity");
    uint64_t bits = 0;
    int n = cpu_count_online();
    if (n > 64) n = 64;   /* cap at 64 bits for the Lua integer */
    for (int i = 0; i < n; i++) {
        if (CPU_ISSET(i, &mask)) bits |= ((uint64_t)1) << i;
    }
    lua_pushinteger(L, (lua_Integer)bits);
    return 1;
}

static int l_setaffinity(lua_State *L) {
    uint64_t bits = (uint64_t)luaL_checkinteger(L, 1);
    cpu_set_t mask;
    CPU_ZERO(&mask);
    int n = cpu_count_online();
    if (n > 64) n = 64;
    int any = 0;
    for (int i = 0; i < n; i++) {
        if (bits & (((uint64_t)1) << i)) {
            CPU_SET(i, &mask);
            any = 1;
        }
    }
    if (!any) {
        lua_pushnil(L);
        lua_pushliteral(L, "sched_setaffinity: empty mask");
        return 2;
    }
    if (sched_setaffinity(0, sizeof(mask), &mask) == -1)
        return push_errno(L, "sched_setaffinity");
    lua_pushboolean(L, 1);
    return 1;
}
#else
static int l_getaffinity(lua_State *L) {
    lua_pushnil(L);
    lua_pushliteral(L, "getaffinity: not supported on this platform");
    return 2;
}
static int l_setaffinity(lua_State *L) {
    lua_pushnil(L);
    lua_pushliteral(L, "setaffinity: not supported on this platform");
    return 2;
}
#endif

/* ---- getinterfaces ------------------------------------------------------- */
static socklen_t sa_len_for(const struct sockaddr *sa) {
    if (!sa) return 0;
    if (sa->sa_family == AF_INET)  return (socklen_t)sizeof(struct sockaddr_in);
    if (sa->sa_family == AF_INET6) return (socklen_t)sizeof(struct sockaddr_in6);
    return 0;
}

static void push_addr_field(lua_State *L, const struct sockaddr *sa,
                            const char *key) {
    if (!sa) return;
    if (sa->sa_family != AF_INET && sa->sa_family != AF_INET6) return;
    socklen_t sl = sa_len_for(sa);
    if (!sl) return;
    char host[NI_MAXHOST];
    if (getnameinfo(sa, sl, host, sizeof(host), NULL, 0, NI_NUMERICHOST) == 0) {
        lua_pushstring(L, host);
        lua_setfield(L, -2, key);
    }
}

static int l_getinterfaces(lua_State *L) {
    struct ifaddrs *ifaddr = NULL;
    if (getifaddrs(&ifaddr) == -1) return push_errno(L, "getifaddrs");

    lua_newtable(L);
    int count = 1;
    for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        /* Skip entries that lack an address (nothing useful to report). */
        if (!ifa->ifa_addr) continue;
        int fam = ifa->ifa_addr->sa_family;
        if (fam != AF_INET && fam != AF_INET6) continue;

        lua_newtable(L);
        if (ifa->ifa_name) {
            lua_pushstring(L, ifa->ifa_name);
            lua_setfield(L, -2, "name");
        }
        lua_pushstring(L, fam == AF_INET ? "inet" : "inet6");
        lua_setfield(L, -2, "type");

        push_addr_field(L, ifa->ifa_addr,    "host");
        push_addr_field(L, ifa->ifa_netmask, "netmask");

        /* ifa_broadaddr / ifa_dstaddr share a union in glibc; we surface the
         * one that makes sense per interface flags. */
        if (ifa->ifa_flags & IFF_BROADCAST && ifa->ifa_broadaddr) {
            push_addr_field(L, ifa->ifa_broadaddr, "broadcast");
        } else if (ifa->ifa_flags & IFF_POINTOPOINT && ifa->ifa_dstaddr) {
            push_addr_field(L, ifa->ifa_dstaddr, "dst");
        }

        lua_rawseti(L, -2, count++);
    }
    freeifaddrs(ifaddr);
    return 1;
}

/* ---- readdir (M14.E: v1 `config` module needs a directory scanner without
 *      pulling in LuaFileSystem). Returns an array table of raw entry names
 *      including "." and "..". Caller filters. Empty directory returns {};
 *      failure returns (nil, errmsg, errno). Non-recursive; caller iterates.
 *
 *      Chose an all-at-once table over an iterator/userdata because config.d
 *      typically has a handful of files; the simplicity is worth more than
 *      the streaming property. Callers that need incremental iteration can
 *      still use this — the returned table works with `ipairs`. */
static int l_readdir(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    DIR *d = opendir(path);
    if (!d) return push_errno(L, "opendir");
    lua_newtable(L);
    int i = 0;
    struct dirent *ent;
    errno = 0;
    while ((ent = readdir(d)) != NULL) {
        i++;
        lua_pushstring(L, ent->d_name);
        lua_rawseti(L, -2, i);
        errno = 0;
    }
    /* readdir returns NULL on end-of-dir with errno unchanged; if errno was
     * set mid-loop we captured a real error. Close first so the errno report
     * reflects readdir, not closedir. */
    int saved = errno;
    closedir(d);
    if (saved != 0) {
        errno = saved;
        return push_errno(L, "readdir");
    }
    return 1;
}

/* ---- setprogname (Linux only, safe pointer swap) -------------------------- */
#if defined(FAN_PLATFORM_LINUX)
extern char *__progname;
static int l_setprogname(lua_State *L) {
    static char fan_progname[128];
    size_t sz = 0;
    const char *name = luaL_checklstring(L, 1, &sz);
    if (sz >= sizeof(fan_progname)) sz = sizeof(fan_progname) - 1;
    memcpy(fan_progname, name, sz);
    fan_progname[sz] = '\0';
    __progname = fan_progname;
    return 0;
}
#else
static int l_setprogname(lua_State *L) {
    (void)luaL_checkstring(L, 1);
    lua_pushnil(L);
    lua_pushliteral(L, "setprogname: not supported on this platform");
    return 2;
}
#endif

/* ---- registration -------------------------------------------------------- */
static const luaL_Reg posix_lib[] = {
    {"getpid",         l_getpid},
    {"fork",           l_fork},
    {"waitpid",        l_waitpid},
    {"kill",           l_kill},
    {"setpgid",        l_setpgid},
    {"getpgid",        l_getpgid},
    {"setsid",         l_setsid},
    {"getcpucount",    l_getcpucount},
    {"getaffinity",    l_getaffinity},
    {"setaffinity",    l_setaffinity},
    {"getinterfaces",  l_getinterfaces},
    {"setprogname",    l_setprogname},
    {"readdir",        l_readdir},
    {NULL, NULL},
};

static void push_signal_table(lua_State *L) {
    lua_newtable(L);
#define SIG_ENTRY(name)                              \
    do { lua_pushinteger(L, name);                   \
         lua_setfield(L, -2, #name); } while (0)
    SIG_ENTRY(SIGHUP);
    SIG_ENTRY(SIGINT);
    SIG_ENTRY(SIGQUIT);
    SIG_ENTRY(SIGKILL);
    SIG_ENTRY(SIGTERM);
    SIG_ENTRY(SIGUSR1);
    SIG_ENTRY(SIGUSR2);
    SIG_ENTRY(SIGPIPE);
    SIG_ENTRY(SIGCHLD);
    SIG_ENTRY(SIGSTOP);
    SIG_ENTRY(SIGCONT);
#undef SIG_ENTRY
}

static void push_wait_flags(lua_State *L) {
    lua_newtable(L);
    lua_pushinteger(L, WNOHANG);   lua_setfield(L, -2, "WNOHANG");
    lua_pushinteger(L, WUNTRACED); lua_setfield(L, -2, "WUNTRACED");
#ifdef WCONTINUED
    lua_pushinteger(L, WCONTINUED); lua_setfield(L, -2, "WCONTINUED");
#endif
}

void fan_posix_register(lua_State *L) {
    /* module table 'fan' is on top of the stack when the caller invokes us
     * (mirrors every other fan_*_register). */
    lua_newtable(L);
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, posix_lib, 0);
#else
    luaL_register(L, NULL, posix_lib);
#endif
    push_signal_table(L);
    lua_setfield(L, -2, "signals");
    push_wait_flags(L);
    lua_setfield(L, -2, "wait");
    lua_setfield(L, -2, "posix");
}
