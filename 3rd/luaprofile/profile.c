/*
 * profile.c -- simple-engine CPU call-tree sampler (DX P4.1).
 *
 * Why this exists: the plan (docs/plans/debug-system.md) called for vendoring
 * c2trunk's `3rd/luaprofile` (upstream lsg2020/skynet@4ace42e8). That source is
 * NOT obtainable in this environment (c2trunk engine submodule is not
 * initialised locally; upstream commit 4ace42e8 has no such directory), so this
 * is a from-scratch implementation with a compatible shape:
 *
 *   profile.start()            -> true | false, errmsg
 *   profile.stop()             -> time_us, nodes, info
 *   profile.dump()             -> time_us, nodes, info   (snapshot, keeps running)
 *   profile.mark()             -> true                   (coroutine creation point)
 *   profile.rescan()           -> n                      (re-install hooks on all threads)
 *
 * `nodes` is a call tree (path sensitive): every entry is
 *   { name, source, linedefined, count, value, self,
 *     alloc_count, alloc_bytes, children = { ... } }
 * `value` is inclusive microseconds, `self` is value minus children's value.
 *
 * Design notes (all four mechanisms were proven by the P4.0 probe first):
 *   - hooks are installed by walking global_State->allgc (Lua internal headers);
 *   - the allocator is hijacked with the ORIGINAL ud passed straight through
 *     (the engine casts that ud to struct snlua, so it must not be replaced);
 *   - unlike c2trunk's version we keep the sampler context in a static struct
 *     instead of inside `struct snlua`. That removes the ABI contract (no
 *     service_snlua.c patch, no snlua_profile_slot export) at the cost of
 *     "one sampling session per process", which is exactly how it is used.
 *   - timing uses clock_gettime(CLOCK_MONOTONIC); no rdtsc, so the build stays
 *     portable (slightly more overhead per hook, irrelevant in a 30s window).
 *
 * Build: see Makefile rule for $(LUA_CLIB_PATH)/profile.so (-I3rd/lua).
 * Requires the host to export the Lua API (-Wl,-E), same as every luaclib.
 */

#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lobject.h"
#include "lstate.h"

#define PF_MAX_NODE    16384
#define PF_MAX_THREAD  128
#define PF_MAX_DEPTH   512
#define PF_HASH_BUCKET 32768
#define PF_NAME_LEN    64
#define PF_SRC_LEN     160

typedef struct pf_node {
	int parent;
	int first_child;
	int next_sibling;
	char name[PF_NAME_LEN];
	char source[PF_SRC_LEN];
	int linedefined;
	long long count;
	long long value;        /* inclusive microseconds */
	long long alloc_count;
	long long alloc_bytes;
} pf_node;

typedef struct pf_frame {
	int node;
	long long enter_ns;
} pf_frame;

typedef struct pf_tstack {
	lua_State *th;
	int top;
	pf_frame frames[PF_MAX_DEPTH];
} pf_tstack;

/* hash-bucket chain (separate from the tree's next_sibling list) */
static int pf_hash_next[PF_MAX_NODE];

static struct {
	int running;
	int node_n;
	int hash[PF_HASH_BUCKET];
	pf_node nodes[PF_MAX_NODE];
	pf_tstack stacks[PF_MAX_THREAD];
	int stack_n;
	long long start_ns;
	long long overflow;
	int marked_node;
	int cur_node;
	lua_Alloc orig_alloc;
	void *orig_ud;
	lua_State *saved_th[PF_MAX_THREAD];
	lua_Hook saved_hook[PF_MAX_THREAD];
	int saved_mask[PF_MAX_THREAD];
	int saved_count[PF_MAX_THREAD];
	int saved_n;
} pf;

static long long pf_now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

static unsigned pf_hash(int parent, const char *src, int line) {
	unsigned h = 2166136261u;
	h ^= (unsigned)parent;
	h *= 16777619u;
	while (*src) {
		h ^= (unsigned char)(*src++);
		h *= 16777619u;
	}
	h ^= (unsigned)line;
	h *= 16777619u;
	return h & (PF_HASH_BUCKET - 1);
}

static void pf_reset(void) {
	int i;
	pf.node_n = 1;                 /* node 0 is the synthetic root */
	pf.stack_n = 0;
	pf.overflow = 0;
	pf.marked_node = -1;
	pf.cur_node = 0;
	pf.saved_n = 0;
	pf.start_ns = pf_now_ns();
	memset(pf.hash, 0xff, sizeof(pf.hash));
	memset(&pf.nodes[0], 0, sizeof(pf_node));
	pf.nodes[0].parent = -1;
	pf.nodes[0].first_child = -1;
	pf.nodes[0].next_sibling = -1;
	strcpy(pf.nodes[0].name, "ROOT");
	strcpy(pf.nodes[0].source, "");
	pf.nodes[0].linedefined = -1;
	for (i = 0; i < PF_MAX_THREAD; i++) {
		pf.stacks[i].th = NULL;
		pf.stacks[i].top = 0;
	}
}

static int pf_node_find(int parent, const char *src, int line) {
	unsigned h = pf_hash(parent, src, line);
	int i = pf.hash[h];
	while (i >= 0) {
		if (pf.nodes[i].parent == parent && pf.nodes[i].linedefined == line &&
			strcmp(pf.nodes[i].source, src) == 0) {
			return i;
		}
		i = pf_hash_next[i];
	}
	return -1;
}

static int pf_node_get(int parent, const char *src, int line, const char *name) {
	unsigned h;
	int id;
	if (parent < 0) {
		parent = 0;
	}
	id = pf_node_find(parent, src, line);
	if (id >= 0) {
		return id;
	}
	if (pf.node_n >= PF_MAX_NODE) {
		pf.overflow++;
		return -1;
	}
	id = pf.node_n++;
	memset(&pf.nodes[id], 0, sizeof(pf_node));
	pf.nodes[id].parent = parent;
	pf.nodes[id].first_child = -1;
	pf.nodes[id].next_sibling = -1;
	pf.nodes[id].linedefined = line;
	strncpy(pf.nodes[id].source, src, PF_SRC_LEN - 1);
	if (name != NULL) {
		strncpy(pf.nodes[id].name, name, PF_NAME_LEN - 1);
	} else {
		strcpy(pf.nodes[id].name, "?");
	}
	h = pf_hash(parent, src, line);
	pf_hash_next[id] = pf.hash[h];
	pf.hash[h] = id;
	/* link into the parent's child list */
	pf.nodes[id].next_sibling = pf.nodes[parent].first_child;
	pf.nodes[parent].first_child = id;
	return id;
}

static pf_tstack *pf_stack_get(lua_State *L) {
	int i;
	for (i = 0; i < pf.stack_n; i++) {
		if (pf.stacks[i].th == L) {
			return &pf.stacks[i];
		}
	}
	if (pf.stack_n >= PF_MAX_THREAD) {
		pf.overflow++;
		return NULL;
	}
	pf.stacks[pf.stack_n].th = L;
	pf.stacks[pf.stack_n].top = 0;
	pf.stack_n++;
	return &pf.stacks[pf.stack_n - 1];
}

static void pf_hook(lua_State *L, lua_Debug *ar) {
	pf_tstack *st;
	if (!pf.running) {
		return;
	}
	st = pf_stack_get(L);
	if (st == NULL) {
		return;
	}
	if (ar->event == LUA_HOOKCALL) {
		int parent;
		int id;
		long long t;
		if (st->top >= PF_MAX_DEPTH) {
			pf.overflow++;
			return;
		}
		lua_getinfo(L, "nSl", ar);
		parent = (st->top > 0) ? st->frames[st->top - 1].node : pf.marked_node;
		if (parent < 0) {
			parent = 0;
		}
		id = pf_node_get(parent, ar->short_src ? ar->short_src : "?", ar->linedefined, ar->name);
		t = pf_now_ns();
		st->frames[st->top].node = id;
		st->frames[st->top].enter_ns = t;
		st->top++;
		if (id >= 0) {
			pf.nodes[id].count++;
			pf.cur_node = id;
		}
	} else if (ar->event == LUA_HOOKRET) {
		if (st->top > 0) {
			long long t = pf_now_ns();
			int id = st->frames[st->top - 1].node;
			if (id >= 0) {
				pf.nodes[id].value += (t - st->frames[st->top - 1].enter_ns) / 1000;
			}
			st->top--;
			pf.cur_node = (st->top > 0) ? st->frames[st->top - 1].node : 0;
		}
	}
}

static void *pf_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
	if (pf.running && pf.cur_node >= 0 && pf.cur_node < pf.node_n) {
		pf.nodes[pf.cur_node].alloc_count++;
		if (nsize > osize) {
			pf.nodes[pf.cur_node].alloc_bytes += (long long)(nsize - osize);
		}
	}
	return pf.orig_alloc(ud, ptr, osize, nsize);
}

/* install hooks on every thread of this state; returns how many were hooked */
static int pf_install(lua_State *L) {
	global_State *g = G(L);
	GCObject *o;
	int n = 0;
	pf.saved_n = 0;
	for (o = g->allgc; o != NULL; o = o->next) {
		if (novariant(o->tt) == LUA_TTHREAD) {
			lua_State *th = gco2th(o);
			if (pf.saved_n < PF_MAX_THREAD) {
				pf.saved_th[pf.saved_n] = th;
				pf.saved_hook[pf.saved_n] = lua_gethook(th);
				pf.saved_mask[pf.saved_n] = lua_gethookmask(th);
				pf.saved_count[pf.saved_n] = lua_gethookcount(th);
				pf.saved_n++;
			}
			lua_sethook(th, pf_hook, LUA_MASKCALL | LUA_MASKRET, 0);
			n++;
		}
	}
	if (pf.orig_alloc == NULL) {
		void *ud = NULL;
		pf.orig_alloc = lua_getallocf(L, &ud);
		pf.orig_ud = ud;
	}
	lua_setallocf(L, pf_alloc, pf.orig_ud);
	return n;
}

static void pf_uninstall(void) {
	int i;
	for (i = 0; i < pf.saved_n; i++) {
		lua_sethook(pf.saved_th[i], pf.saved_hook[i], pf.saved_mask[i], pf.saved_count[i]);
	}
	pf.saved_n = 0;
}

static void pf_build_node(lua_State *L, int id) {
	int child;
	long long children_value = 0;
	lua_newtable(L);
	lua_pushstring(L, pf.nodes[id].name);
	lua_setfield(L, -2, "name");
	lua_pushstring(L, pf.nodes[id].source);
	lua_setfield(L, -2, "source");
	lua_pushinteger(L, pf.nodes[id].linedefined);
	lua_setfield(L, -2, "linedefined");
	lua_pushinteger(L, (lua_Integer)pf.nodes[id].count);
	lua_setfield(L, -2, "count");
	lua_pushinteger(L, (lua_Integer)pf.nodes[id].value);
	lua_setfield(L, -2, "value");
	lua_pushinteger(L, (lua_Integer)pf.nodes[id].alloc_count);
	lua_setfield(L, -2, "alloc_count");
	lua_pushinteger(L, (lua_Integer)pf.nodes[id].alloc_bytes);
	lua_setfield(L, -2, "alloc_bytes");
	lua_newtable(L);
	{
		int idx = 0;
		for (child = pf.nodes[id].first_child; child >= 0; child = pf.nodes[child].next_sibling) {
			children_value += pf.nodes[child].value;
			pf_build_node(L, child);
			lua_rawseti(L, -2, ++idx);
		}
	}
	lua_setfield(L, -2, "children");
	{
		long long self = pf.nodes[id].value - children_value;
		if (self < 0) {
			self = 0;
		}
		lua_pushinteger(L, (lua_Integer)self);
		lua_setfield(L, -2, "self");
	}
}

static int pf_snapshot(lua_State *L) {
	long long time_us = (pf_now_ns() - pf.start_ns) / 1000;
	int i;
	long long total_calls = 0;
	long long total_alloc = 0;
	long long total_bytes = 0;
	for (i = 1; i < pf.node_n; i++) {
		total_calls += pf.nodes[i].count;
		total_alloc += pf.nodes[i].alloc_count;
		total_bytes += pf.nodes[i].alloc_bytes;
	}
	lua_pushinteger(L, (lua_Integer)time_us);
	/* nodes: children of the synthetic root */
	lua_newtable(L);
	{
		int child;
		int idx = 0;
		for (child = pf.nodes[0].first_child; child >= 0; child = pf.nodes[child].next_sibling) {
			pf_build_node(L, child);
			lua_rawseti(L, -2, ++idx);
		}
	}
	lua_newtable(L);
	lua_pushinteger(L, (lua_Integer)(pf.node_n - 1));
	lua_setfield(L, -2, "nodes");
	lua_pushinteger(L, (lua_Integer)total_calls);
	lua_setfield(L, -2, "calls");
	lua_pushinteger(L, (lua_Integer)total_alloc);
	lua_setfield(L, -2, "alloc_count");
	lua_pushinteger(L, (lua_Integer)total_bytes);
	lua_setfield(L, -2, "alloc_bytes");
	lua_pushinteger(L, (lua_Integer)pf.overflow);
	lua_setfield(L, -2, "overflow");
	lua_pushboolean(L, pf.running);
	lua_setfield(L, -2, "running");
	return 3;
}

static int l_start(lua_State *L) {
	if (pf.running) {
		lua_pushboolean(L, 0);
		lua_pushstring(L, "already running");
		return 2;
	}
	pf_reset();
	pf.running = 1;
	pf_install(L);
	lua_pushboolean(L, 1);
	return 1;
}

static int l_stop(lua_State *L) {
	if (!pf.running) {
		lua_pushboolean(L, 0);
		lua_pushstring(L, "not running");
		return 2;
	}
	pf.running = 0;
	pf_uninstall();
	if (pf.orig_alloc != NULL) {
		lua_setallocf(L, pf.orig_alloc, pf.orig_ud);
	}
	pf.cur_node = 0;
	return pf_snapshot(L);
}

static int l_dump(lua_State *L) {
	if (!pf.running) {
		lua_pushboolean(L, 0);
		lua_pushstring(L, "not running");
		return 2;
	}
	return pf_snapshot(L);
}

static int l_mark(lua_State *L) {
	pf_tstack *st = pf_stack_get(L);
	pf.marked_node = (st != NULL && st->top > 0) ? st->frames[st->top - 1].node : 0;
	lua_pushboolean(L, 1);
	return 1;
}

static int l_rescan(lua_State *L) {
	int n;
	if (!pf.running) {
		lua_pushboolean(L, 0);
		lua_pushstring(L, "not running");
		return 2;
	}
	pf_uninstall();
	n = pf_install(L);
	lua_pushinteger(L, n);
	return 1;
}

static int l_running(lua_State *L) {
	lua_pushboolean(L, pf.running);
	return 1;
}

static const luaL_Reg pf_lib[] = {
	{"start", l_start},
	{"stop", l_stop},
	{"dump", l_dump},
	{"mark", l_mark},
	{"rescan", l_rescan},
	{"running", l_running},
	{NULL, NULL}
};

int luaopen_profile(lua_State *L) {
	luaL_newlib(L, pf_lib);
	return 1;
}
