# luaprofile（自研 CPU 调用树采样器）

> 归属：引擎仓 `skynet/3rd/luaprofile/`，随引擎编译分发（产物 `luaclib/profile.so`）。
> 用途：DX（调试与 GM 体系）P4「CPU 采样分析」的底层 C 库，主仓侧封装见
> `simple-server` 的 `framework/debug/profiler.ts` 与控制台 `prof` / `profdump` 命令。

## 1. 来源与为什么自研

计划（simple-server `docs/plans/debug-system.md` §五 P4）原定 **vendor c2trunk
`3rd/luaprofile`**（上游 `lsg2020/skynet@4ace42e8` + c2trunk 自加的 `icallpath`
调用路径聚合）。2026-09-08 实施时发现**该源码不可得**：

- 本地 c2trunk 仓库的引擎 submodule 未初始化（无 `skynet/` 源码目录）；
- 上游 `lsg2020/skynet` 的 commit `4ace42e8` 的 tree 中**不存在** `3rd/luaprofile`
  （只有 `lualib/compat10/profile.lua`），即该 C 库并非来自该上游 commit。

因此改为**自研等价实现**：接口形态与 c2trunk 对齐（`start/stop/mark/dump`，
输出调用树），但内部按本项目的约束重新设计（见 §3）。

若日后能拿到 c2trunk 那份源码，可在本目录并列放置并在 Makefile 切换，
上层 `profiler.ts` 的接口契约不变。

## 2. 构建

```bash
# 引擎 Makefile 已登记（LUA_CLIB 含 profile）
cd /app/simple-server/skynet && make linux
# 产物：luaclib/profile.so（经 build_engine.sh 复制到 engine/luaclib/）
```

编译要点：
- `-I3rd/lua`：依赖 **Lua 内部头**（`lobject.h` / `lstate.h`），用于遍历
  `global_State->allgc` 给所有线程装 hook（P4.0 探针已验证 5.5.1 内部头可用）；
- 未定义的 `lua_*` 符号由宿主（skynet 主程序 `-Wl,-E`）解析，与其它 luaclib 一致；
- 计时用 `clock_gettime(CLOCK_MONOTONIC)`，**不用 rdtsc**——保持可移植，
  代价是每次 hook 多一次系统调用级开销（仅在采样窗口内，可接受）。

## 3. 与 c2trunk 版本的关键差异（设计决策）

| 项 | c2trunk `3rd/luaprofile` | 本项目自研版 | 理由 |
|---|---|---|---|
| context 存放 | 塞进 `struct snlua` 首字段（需改引擎 `service_snlua.c`，且要导出 `snlua_profile_slot()` 固化布局） | **静态结构体**（进程内单采样会话） | 彻底消除 ABI 契约与"静默写坏内存"的失败模式；采样本就是单会话（有重入互斥），无功能损失 |
| 引擎改动 | `struct snlua` 加字段 + 导出访问函数 | **零改动** | 引擎升级免漂移；也省掉"字段被上游移动"的长期维护成本 |
| 计时 | rdtsc（x86） | `CLOCK_MONOTONIC` | 可移植；容器 x86_64 下精度足够（ns） |
| 节点聚合 | `imap` + `icallpath` | 哈希表（parent + source + linedefined）+ 兄弟链 | 同样得到**路径敏感**调用树，代码量与依赖都更小 |
| 分配归因 | 经 snlua context 取当前节点 | hook 内维护 `cur_node` 静态值 | 同样能归因到当前栈顶节点 |

## 4. Lua API

```lua
local profile = require "profile"

profile.start()      -- -> true | false, "already running"
profile.mark()       -- 标记协程创建点（应在 coroutine.create/wrap 之前调用）
profile.rescan()     -- -> n  重新给所有线程装 hook（新建协程后需要）
profile.dump()       -- -> time_us, nodes, info   （快照，不停止）
profile.stop()       -- -> time_us, nodes, info   （停止并卸载 hook/还原 allocf）
profile.running()    -- -> boolean
```

`nodes` 为调用树数组（根的孩子），每项：
`{ name, source, linedefined, count, value, self, alloc_count, alloc_bytes, children }`

- `value`：含子调用的**累计**耗时（微秒）；
- `self`：`value` 减去所有孩子 `value`（自身耗时），为负则归零；
- `info`：`{ nodes, calls, alloc_count, alloc_bytes, overflow, running }`。

## 5. 注意事项 / 已知边界

1. **单会话**：进程内同一时刻只允许一个采样会话（`start` 重入返回 false）。
   多服务同时采样需串行化（主仓侧 `profiler.ts` 负责互斥与排队）。
2. **hook 与 skynet 的 trap/signal hook 共用槽位**：采样期间信号中断统计会互相
   覆盖（引擎既有行为，与 c2trunk 同）。采样窗口建议 ≤30s。
3. **新建协程需 `rescan()`**：hook 只装在采样开始那一刻的线程上；新协程不会
   自动带上 hook（P4.0 探针实测确认）。`mark()` 只负责把新协程挂到正确的父节点。
4. **allocf 劫持必须透传原 ud**：引擎把该 ud 当 `struct snlua` 使用
   （`lalloc` / `resumeX` / `signal_hook`），换成其它指针会立刻崩。
5. **函数 key 用 (source, linedefined)**：不用函数对象指针，避免 GC 后地址复用
   导致的节点错并；代价是每次 CALL/RET 做一次字符串比较（采样窗口内可接受）。
6. **容量固定**：`PF_MAX_NODE=16384`、`PF_MAX_THREAD=128`、`PF_MAX_DEPTH=512`；
   超出的部分丢弃并计入 `info.overflow`（不崩、不静默）。
