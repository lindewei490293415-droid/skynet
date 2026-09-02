/*
 * lua-inotify —— 极简 Linux inotify Lua 绑定（自写，随引擎编译，无外部 .so 依赖/ABI 漂移）。
 *
 * 用途：进程内 file_watcher 服务（热更 P5，docs/features/hot-update.md）的变更感知层。
 *
 * 用法：
 *   local inotify = require "inotify"
 *   local h = inotify.init{ blocking = false }     -- 默认非阻塞；事件在内核 fd 队列积压
 *   local wd = h:addwatch(path, inotify.IN_CLOSE_WRITE | inotify.IN_CREATE | inotify.IN_MOVED_TO)
 *   -- 周期（服务 timer tick）调 h:read() 取积压事件：
 *   --   { { wd=, mask=, name= }, ... }（读到 EAGAIN 停止；无事件为空表）
 *   h:rmwatch(wd)
 *   h:close()
 *
 * 事件掩码刻意用 IN_CLOSE_WRITE/IN_CREATE/IN_MOVED_TO（文件"写完关闭/移入"才报），
 * 规避 scp 分批写文件的"半读"（对比 c2trunk 用 IN_MODIFY + md5 hash 兜底半写）。
 *
 * 注：inotify 事件在 fd 队列积压，Lua 侧以 timer tick 周期 read（skynet 无普通 fd 事件
 * 循环接入）；事件到达即入内核队列，读取延迟 = tick 间隔（可 <100ms），无需全量 md5 轮询。
 */

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <sys/inotify.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>

#define INOTIFY_MT "inotify.handle*"

static int
l_checkfd(lua_State *L, int idx) {
    int *pfd = (int *)luaL_checkudata(L, idx, INOTIFY_MT);
    if (*pfd < 0) {
        return luaL_error(L, "inotify handle already closed");
    }
    return *pfd;
}

/* inotify.init{ blocking = bool } -> handle（默认非阻塞；失败返回 nil, errmsg） */
static int
linit(lua_State *L) {
    int flags = IN_NONBLOCK;
    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "blocking");
        if (lua_toboolean(L, -1)) {
            flags = 0;
        }
        lua_pop(L, 1);
    }
    int fd = inotify_init1(flags);
    if (fd < 0) {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }
    int *pfd = (int *)lua_newuserdatauv(L, sizeof(int), 0);
    *pfd = fd;
    luaL_getmetatable(L, INOTIFY_MT);
    lua_setmetatable(L, -2);
    return 1;
}

/* h:addwatch(path, mask) -> wd | nil, errmsg */
static int
laddwatch(lua_State *L) {
    int fd = l_checkfd(L, 1);
    const char *path = luaL_checkstring(L, 2);
    uint32_t mask = (uint32_t)luaL_checkinteger(L, 3);
    int wd = inotify_add_watch(fd, path, mask);
    if (wd < 0) {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }
    lua_pushinteger(L, wd);
    return 1;
}

/* 递归 addwatch 目录树（opendir/readdir；进程内 Lua 无 lfs，目录遍历下沉到 C）。
 * h:watchtree(root, mask) -> { [wd] = path, ... }（供事件 wd → 路径还原） */
static void
watch_tree_rec(int fd, const char *dirpath, uint32_t mask, lua_State *L) {
    int wd = inotify_add_watch(fd, dirpath, mask);
    if (wd >= 0) {
        lua_pushstring(L, dirpath);
        lua_rawseti(L, -2, wd);
    }
    DIR *d = opendir(dirpath);
    if (d == NULL) {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        size_t plen = strlen(dirpath);
        size_t nlen = strlen(ent->d_name);
        char *child = (char *)malloc(plen + nlen + 2);
        if (child == NULL) {
            continue;
        }
        memcpy(child, dirpath, plen);
        child[plen] = '/';
        memcpy(child + plen + 1, ent->d_name, nlen + 1);
        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            watch_tree_rec(fd, child, mask, L);
        }
        free(child);
    }
    closedir(d);
}

static int
lwatchtree(lua_State *L) {
    int fd = l_checkfd(L, 1);
    const char *root = luaL_checkstring(L, 2);
    uint32_t mask = (uint32_t)luaL_checkinteger(L, 3);
    lua_newtable(L);
    watch_tree_rec(fd, root, mask, L);
    return 1;
}

/* h:rmwatch(wd) */
static int
lrmwatch(lua_State *L) {
    int fd = l_checkfd(L, 1);
    int wd = (int)luaL_checkinteger(L, 2);
    inotify_rm_watch(fd, wd);
    return 0;
}

/* h:close() */
static int
lclose(lua_State *L) {
    int *pfd = (int *)luaL_checkudata(L, 1, INOTIFY_MT);
    if (*pfd >= 0) {
        close(*pfd);
        *pfd = -1;
    }
    return 0;
}

static int
lgc(lua_State *L) {
    int *pfd = (int *)luaL_checkudata(L, 1, INOTIFY_MT);
    if (*pfd >= 0) {
        close(*pfd);
        *pfd = -1;
    }
    return 0;
}

/*
 * h:read() -> 积压事件数组 { {wd=,mask=,name=}, ... }
 * 内核保证单次 read 返回完整事件序列（缓冲足够容纳至少一个完整事件）；
 * 缓冲 16KB >> 最大单事件（16 + PATH_MAX）→ 事件不会跨 read 截断。
 */
static int
lread(lua_State *L) {
    int fd = l_checkfd(L, 1);
    lua_newtable(L);
    int idx = 1;
    char buf[16384];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            break; /* EAGAIN 无事件 / 其它错误：本轮结束 */
        }
        char *p = buf;
        ssize_t remain = n;
        while (remain >= (ssize_t)sizeof(struct inotify_event)) {
            struct inotify_event *ev = (struct inotify_event *)p;
            size_t evsize = (size_t)sizeof(struct inotify_event) + ev->len;
            if (evsize > (size_t)remain) {
                break; /* 尾部分块不会发生（缓冲足够），防御性退出 */
            }
            lua_createtable(L, 0, 3);
            lua_pushinteger(L, ev->wd);
            lua_setfield(L, -2, "wd");
            lua_pushinteger(L, (lua_Integer)ev->mask);
            lua_setfield(L, -2, "mask");
            if (ev->len > 0) {
                /* name 含对齐 \0 填充（ev->len 含空字节），截断到首个 \0 */
                size_t nlen = 0;
                while (nlen < ev->len && ev->name[nlen] != '\0') {
                    nlen++;
                }
                lua_pushlstring(L, ev->name, nlen);
            } else {
                lua_pushstring(L, "");
            }
            lua_setfield(L, -2, "name");
            lua_rawseti(L, -2, idx);
            idx++;
            p += evsize;
            remain -= (ssize_t)evsize;
        }
    }
    return 1;
}

/* 常量逐个注册（新内核宏可能缺失于编译头文件，用 #ifdef 条件暴露） */
#define INOTIFY_PUSH_CONST(L, name) \
    do { lua_pushinteger(L, name); lua_setfield(L, -2, #name); } while (0)

static void
push_consts(lua_State *L) {
    INOTIFY_PUSH_CONST(L, IN_ACCESS);
    INOTIFY_PUSH_CONST(L, IN_MODIFY);
    INOTIFY_PUSH_CONST(L, IN_ATTRIB);
    INOTIFY_PUSH_CONST(L, IN_CLOSE_WRITE);
    INOTIFY_PUSH_CONST(L, IN_CLOSE_NOWRITE);
    INOTIFY_PUSH_CONST(L, IN_OPEN);
    INOTIFY_PUSH_CONST(L, IN_MOVED_FROM);
    INOTIFY_PUSH_CONST(L, IN_MOVED_TO);
    INOTIFY_PUSH_CONST(L, IN_CREATE);
    INOTIFY_PUSH_CONST(L, IN_DELETE);
    INOTIFY_PUSH_CONST(L, IN_DELETE_SELF);
    INOTIFY_PUSH_CONST(L, IN_MOVE_SELF);
    INOTIFY_PUSH_CONST(L, IN_UNMOUNT);
    INOTIFY_PUSH_CONST(L, IN_Q_OVERFLOW);
    INOTIFY_PUSH_CONST(L, IN_IGNORED);
    INOTIFY_PUSH_CONST(L, IN_ISDIR);
    INOTIFY_PUSH_CONST(L, IN_ONLYDIR);
    INOTIFY_PUSH_CONST(L, IN_DONT_FOLLOW);
    INOTIFY_PUSH_CONST(L, IN_MASK_ADD);
    INOTIFY_PUSH_CONST(L, IN_ONESHOT);
    INOTIFY_PUSH_CONST(L, IN_CLOSE);
    INOTIFY_PUSH_CONST(L, IN_MOVE);
#ifdef IN_EXCL_UNLINK
    INOTIFY_PUSH_CONST(L, IN_EXCL_UNLINK);
#endif
#ifdef IN_MASK_CREATE
    INOTIFY_PUSH_CONST(L, IN_MASK_CREATE);
#endif
}

static const luaL_Reg inotify_methods[] = {
    { "addwatch", laddwatch },
    { "watchtree", lwatchtree },
    { "rmwatch", lrmwatch },
    { "read", lread },
    { "close", lclose },
    { NULL, NULL },
};

static const luaL_Reg inotify_funcs[] = {
    { "init", linit },
    { NULL, NULL },
};

int luaopen_inotify(lua_State *L) {
    luaL_newlibtable(L, inotify_funcs);

    luaL_newmetatable(L, INOTIFY_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, inotify_methods, 0);
    lua_pushcfunction(L, lgc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    luaL_setfuncs(L, inotify_funcs, 0);

    push_consts(L);

    return 1;
}
