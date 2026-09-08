# luaprofile（CPU 调用树采样器）

> 归属：引擎仓 `skynet/3rd/luaprofile/`，随引擎编译分发（产物 `luaclib/profile.so`）。
> 用途：simple-server DX P4「CPU 采样分析」的底层 C 库；上层封装为 simple-server
> 的 `framework/debug/profiler.ts` 与控制台 `prof` / `profdump` 命令。

## 1. 来源

vendor 自 c2trunk 引擎仓 `D:\workspace\skynet-engine\skynet\3rd\luaprofile\`
（上游标注：`https://github.com/lsg2020/skynet/commit/4ace42e80814abfff6b8e64335061a206c674f96`，
即原 README 所引 commit）。

> **历程记录**：2026-09-08 实施时一度在本地 `f:\workspace\c2trunk\server` 找不到
> 该目录（引擎 submodule 未初始化），且用 GitHub API 查上游 commit 的 tree 时
> 被 findstr 过滤误导，遂**自研了一版**（simple-engine `1bcc174`，接口
> `require "profile"`）。同日用户指出旧引擎在 `D:\workspace\skynet-engine\`，
> 源码可得 → **切回 vendor 方案（计划决策 Q2 原定）**，自研版弃用（代码见该
> commit 历史）。弃用理由：c2trunk 生产验证过、`mark(co)` 可精准给新协程装
> hook、自带 `profile.lua` 封装可参考、火焰图工具链与 `icallpath` 聚合对得上。

## 2. 我们的补丁（相对上游）

| # | 补丁 | 理由 |
|---|---|---|
| 1 | **删除本地 `struct snlua` 定义**，profile context 改经引擎导出的 `void ** snlua_profile_slot(void *ud)` 存取（`_get_profile` / `_lstart` / `_lstop` / `_resolve_alloc` 四处） | 布局契约 → **符号契约**：字段被上游移动/改名时失败模式从"静默写坏内存"降级为"dlopen 显式报未定义符号"；契约收敛到引擎一处 |
| 2 | Makefile rule 同时编 `imap.c icallpath.c profile.c`（上游 makefile 只编 `imap.c profile.c`，**漏了 icallpath.c**，直接用会链接失败） | profile.c 引用了 icallpath 的路径聚合 |
| 3 | 编译加 `-I3rd/lua -I3rd/luaprofile -DUSE_RDTSC -DUSE_EXPORT_NAME -lpthread` | 上游 makefile 是独立构建，此处并入引擎 Makefile |

**引擎侧配套（`service-src/service_snlua.c`）**：`struct snlua` **首位**加
`void * profile_context;`（c2trunk 同款，注释标明勿调整字段顺序）+ 导出
`snlua_profile_slot()`。该字段**只经此函数存取**，C 库不感知结构体布局。

## 3. 构建

```bash
# 引擎 Makefile 已登记（LUA_CLIB 含 profile）
cd /app/simple-server/skynet && make linux
# 产物：luaclib/profile.so（经 shell/build_engine.sh 复制到 engine/luaclib/）
```

注意：改了 `service_snlua.c` 必须整体 `make linux`（`cservice/snlua.so` 与
`profile.so` 需同代——旧 snlua.so 没有 profile_context 字段时，profile.so 的
`snlua_profile_slot` 会读到野指针）。

## 4. Lua API（与 c2trunk 一致）

```lua
local c = require "profile.c"

c.start()        -- 开始采样（无返回值；重入靠上层 exists 计数，见 profile.lua）
c.mark()         -- 给当前协程装 hook；应在 coroutine.create/wrap 的包装函数首行调用
c.unmark(co)     -- 卸载指定协程 hook
c.dump()         -- -> time_us, root_node   （快照，采样继续）
c.stop()         -- 停止（**无返回值**——必须先 dump 再 stop）
```

`root_node` 是单表（根节点名 `total`），`children` 才是数组。节点字段：

| 字段 | 语义 |
|---|---|
| `name` | `"函数名 源文件:行"` 拼接串（火焰图可直接用） |
| `count` | 该路径累计调用次数（**含子孙取 max** 的聚合口径，见 `_dump_call_path`） |
| `value` | 累计耗时（微秒） |
| `rettime` | 返回耗时 |
| `alloc_count` | **分配字节数**（注意：与 `alloc_times`=次数 命名反直觉） |
| `alloc_times` | 分配次数 |
| `children` | 子路径数组（路径敏感：同函数不同调用路径是不同节点） |

配套封装（simple-server 主仓 TS 侧实现，参考上游 `profile.lua`）：
hook `coroutine.create/wrap` 在包装函数首行打 `c.mark()` + `exists` 计数互斥。

## 5. 注意事项 / 已知边界

1. **snlua.so 与 profile.so 必须同代**（见 §3 注意）。
2. **`c.stop()` 不返回数据**；数据出口只有 `dump()`。
3. **dump 期间 alloc 统计暂停**（`increment_alloc_count = false`，防自统计）。
4. **hook 与 skynet 的 trap/signal hook 共用槽位**；采样窗口建议 ≤30s。
5. `count`/`alloc` 的父节点取 max(children, self)，不是求和——写报表时注意口径。
6. rdtsc 依赖 x86_64（容器 OK）；非 x86 需去掉 `-DUSE_RDTSC`（回落 CLOCK_REALTIME 分支）。
7. 上游 `profile.c` 依赖 Lua 内部头（`lobject.h`/`lstate.h`）；我们 Lua 5.5.1 的
   可用性已由 simple-server 侧 P4.0 编译探针实测确认。
