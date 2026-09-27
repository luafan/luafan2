/*
 * fifo.c — LuaFan v2 named-pipe (FIFO) IPC.
 */
#include "fifo.h"
#include "../platform.h"
#include "../runtime/loop.h"
#include "../runtime/coro.h"

#include <lauxlib.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <event2/bufferevent.h>
#include <event2/buffer.h>

#define FIFO_MT "fan.fifo.f"

static lua_State *g_fifo_L = NULL;  /* main state for cross-callback unref */

typedef struct {
    struct bufferevent *bev;
    int   fd;            /* -1 sentinel when none (fd 0 is valid, v1 bug fix) */
    int   is_writer;
    lua_State *co;
    int   co_ref;
    int   want;          /* -1 any, n bytes */
    int   eof;
    int   closed;
    char *err;
} fifo_t;

static void fifo_set_err(fifo_t *f, const char *m) {
    free(f->err); f->err = m ? strdup(m) : NULL;
}

static void fifo_wake(fifo_t *f, int nargs) {
    lua_State *co = f->co; int ref = f->co_ref;
    f->co = NULL; f->co_ref = LUA_NOREF; f->want = 0;
    fan_coro_wake(g_fifo_L, co, ref, nargs);
}

static int fifo_try_complete(fifo_t *f) {
    if (!f->co || f->want == 0) return 0;
    struct evbuffer *in = bufferevent_get_input(f->bev);
    size_t avail = evbuffer_get_length(in);
    int ok = (f->want == -1) ? (avail > 0 || f->eof || f->err)
                             : ((int)avail >= f->want || f->eof || f->err);
    if (!ok) return 0;
    lua_State *co = f->co;
    if (f->err) { lua_pushnil(co); lua_pushstring(co, f->err); fifo_wake(f, 2); return 1; }
    size_t take = (f->want == -1) ? avail : (size_t)f->want;
    if (take > avail) take = avail;
    if (take == 0) { lua_pushnil(co); lua_pushstring(co, "eof"); fifo_wake(f, 2); return 1; }
    char *tmp = (char *)malloc(take);
    if (!tmp) { lua_pushnil(co); lua_pushstring(co, "oom"); fifo_wake(f, 2); return 1; }
    evbuffer_remove(in, tmp, take);
    lua_pushlstring(co, tmp, take);
    free(tmp);
    fifo_wake(f, 1);
    return 1;
}

static void fifo_readcb(struct bufferevent *bev, void *arg) {
    (void)bev; fifo_try_complete((fifo_t *)arg);
}

static void fifo_eventcb(struct bufferevent *bev, short what, void *arg) {
    (void)bev; fifo_t *f = (fifo_t *)arg;
    if (what & BEV_EVENT_EOF) f->eof = 1;
    if (what & BEV_EVENT_ERROR) fifo_set_err(f, "fifo error");
    if (f->co) fifo_try_complete(f);
}

/* fan.fifo.open{ name=, mode="r"|"w", create=bool } */
static int l_open(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "name");
    const char *name = luaL_checkstring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "mode");
    const char *mode = luaL_optstring(L, -1, "r");
    lua_pop(L, 1);
    lua_getfield(L, 1, "create");
    int create = lua_toboolean(L, -1);
    lua_pop(L, 1);

    int is_writer = (mode[0] == 'w');

    if (create) {
        if (mkfifo(name, 0666) != 0 && errno != EEXIST) {
            lua_pushnil(L);
            lua_pushfstring(L, "mkfifo failed: %s", strerror(errno));
            return 2;
        }
        /* if it exists but is not a fifo, that is an error */
        struct stat st;
        if (stat(name, &st) == 0 && !S_ISFIFO(st.st_mode)) {
            lua_pushnil(L);
            lua_pushstring(L, "path exists and is not a fifo");
            return 2;
        }
    }

    /* Open non-blocking. A writer open would block until a reader is present,
     * so use O_NONBLOCK; for a writer with no reader yet, O_NONBLOCK returns
     * ENXIO — callers open reader first (the common IPC pattern). */
    int flags = (is_writer ? O_WRONLY : O_RDONLY) | O_NONBLOCK;
    int fd = open(name, flags);
    if (fd < 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "open failed: %s", strerror(errno));
        return 2;
    }

    struct event_base *base = fan_loop_current_base();
    struct bufferevent *bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
    if (!bev) { close(fd); lua_pushnil(L); lua_pushstring(L, "bufferevent failed"); return 2; }

    fifo_t *f = (fifo_t *)lua_newuserdata(L, sizeof(*f));
    memset(f, 0, sizeof(*f));
    f->bev = bev; f->fd = fd; f->is_writer = is_writer; f->co_ref = LUA_NOREF;
    luaL_getmetatable(L, FIFO_MT);
    lua_setmetatable(L, -2);

    bufferevent_setcb(bev, fifo_readcb, NULL, fifo_eventcb, f);
    if (!is_writer) bufferevent_enable(bev, EV_READ);
    else bufferevent_enable(bev, EV_WRITE);
    return 1;
}

static int l_send(lua_State *L) {
    fifo_t *f = (fifo_t *)luaL_checkudata(L, 1, FIFO_MT);
    size_t len; const char *data = luaL_checklstring(L, 2, &len);
    if (f->closed || !f->bev) { lua_pushnil(L); lua_pushstring(L, "closed"); return 2; }
    if (!f->is_writer) { lua_pushnil(L); lua_pushstring(L, "not a writer"); return 2; }
    /* libevent's output buffer handles partial writes transparently (v1 bug) */
    if (bufferevent_write(f->bev, data, len) != 0) {
        lua_pushnil(L); lua_pushstring(L, "write failed"); return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_receive(lua_State *L) {
    fifo_t *f = (fifo_t *)luaL_checkudata(L, 1, FIFO_MT);
    int want = -1;
    if (!lua_isnoneornil(L, 2)) { want = (int)luaL_checkinteger(L, 2); if (want <= 0) return luaL_error(L, "size must be > 0"); }
    if (f->closed || !f->bev) { lua_pushnil(L); lua_pushstring(L, "closed"); return 2; }
    if (f->is_writer) { lua_pushnil(L); lua_pushstring(L, "not a reader"); return 2; }

    struct evbuffer *in = bufferevent_get_input(f->bev);
    size_t avail = evbuffer_get_length(in);
    if (f->err) { lua_pushnil(L); lua_pushstring(L, f->err); return 2; }
    if ((want == -1 && avail > 0) || (want > 0 && (int)avail >= want)) {
        size_t take = (want == -1) ? avail : (size_t)want;
        char *tmp = (char *)malloc(take);
        if (!tmp) return luaL_error(L, "oom");
        evbuffer_remove(in, tmp, take);
        lua_pushlstring(L, tmp, take);
        free(tmp);
        return 1;
    }
    if (f->eof) { lua_pushnil(L); lua_pushstring(L, "eof"); return 2; }

    int ref = fan_coro_park(L);
    if (ref == LUA_NOREF) return luaL_error(L, "receive must be called from a coroutine");
    f->co = L; f->co_ref = ref; f->want = want;
    return lua_yield(L, 0);
}

static int l_close(lua_State *L) {
    fifo_t *f = (fifo_t *)luaL_checkudata(L, 1, FIFO_MT);
    if (!f->closed && f->bev) { bufferevent_free(f->bev); f->bev = NULL; f->fd = -1; }
    f->closed = 1;
    return 0;
}

static int fifo_gc(lua_State *L) {
    fifo_t *f = (fifo_t *)luaL_checkudata(L, 1, FIFO_MT);
    if (f->bev) { bufferevent_free(f->bev); f->bev = NULL; }
    free(f->err); f->err = NULL;
    return 0;
}

static const luaL_Reg fifo_methods[] = {
    {"send", l_send},
    {"receive", l_receive},
    {"close", l_close},
    {NULL, NULL},
};

static const luaL_Reg fifo_funcs[] = {
    {"open", l_open},
    {NULL, NULL},
};

void fan_fifo_register(lua_State *L) {
    g_fifo_L = L;
    luaL_newmetatable(L, FIFO_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, fifo_methods, 0);
#else
    luaL_register(L, NULL, fifo_methods);
#endif
    lua_pushcfunction(L, fifo_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    lua_newtable(L);
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, fifo_funcs, 0);
#else
    luaL_register(L, NULL, fifo_funcs);
#endif
    lua_setfield(L, -2, "fifo");
}
