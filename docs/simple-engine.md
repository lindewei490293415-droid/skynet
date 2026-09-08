# simple-engine（simple-server 专用引擎分支）

> 分支定位：simple-server 实验项目的引擎源码仓（C 引擎本体 + 随引擎发布的第三方扩展）。
> 基线：cloudwu/skynet master（本 fork `v1.8.0-55`，含 fork 内 doc commit）。上游合并目标 = `cloudwu/skynet`。
> 消费方式：simple-server 以 git submodule 引用本分支 commit；产物（skynet/cservice/luaclib）编译后复制进主仓 `engine/`（不入库）。
> 相关决策：simple-server `docs/plans/engine-code-layout.md`（引擎侧合并、Go 侧独立）。

## 一、本分支承载什么

| 内容 | 位置 | 说明 |
|---|---|---|
| 引擎本体 | `skynet-src/`、`lualib-src/`、`service-src/` | vanilla skynet + 引擎级定制（定制一律在本分支进行） |
| 引擎内嵌 Lua 库 | `lualib/` | vanilla 自带 + 按需定制 |
| 第三方 C 扩展 | `3rd/luaclib/`、`3rd/lua-protobuf/` | 随引擎编译、产物进 `luaclib/` 的扩展（见下） |
| vanilla 自带 3rd | `3rd/lua`、`3rd/jemalloc`(submodule)、`3rd/lpeg`、`3rd/lua-md5` | 上游结构 |

## 二、扩展库编排

### lcjson2（Lua JSON，模块名 `cjson2`）

- 源码：`3rd/luaclib/lcjson2/`（`lua_cjson.c` + `strbuf.[ch]` + `fpconv.[ch]`，多文件合成一个 .so）
- 来源：c2trunk 引擎仓 `D:\workspace\skynet-engine\skynet\3rd\luaclib\lcjson2\`（lua-cjson 2.1.0 定制版，`luaopen_cjson2`）
- 编译：`make linux` → `luaclib/cjson2.so`（Makefile 独立 rule）

### lua-protobuf（Lua protobuf 运行时，模块名 `pb`）

- 源码：`3rd/lua-protobuf/pb.c` + `pb.h`
- 来源：starwing/lua-protobuf master commit `bbe744f`（2026-07-02）
- 编译：`make linux` → `luaclib/pb.so`（Makefile 独立 rule）
- 配套纯 Lua 解析器 `protoc.lua`（开发期用）目前留在 simple-server 侧，本分支不重复携带

### inotify（Linux inotify Lua 绑定，自研单模块）

- 源码：`lualib-src/lua-inotify.c`（自写极简绑定，非拷贝第三方 .so——规避 ABI 漂移与构建链外二进制）
- 用途：进程内热更 watcher（simple-server `hotupdate_watcher.lua`）的文件系统变更感知
- API：`inotify.init{blocking=bool}` / `h:watchtree(path, mask)`（C 层 opendir 递归 addwatch，返回 `{ [wd]=path }`） / `h:addwatch/rmwatch/read/close`；常量挂模块表
- 设计点：
  - 事件用 `IN_CLOSE_WRITE | IN_CREATE | IN_MOVED_TO`（文件写完/移入才报）——规避 scp 分批写的半读（对比 c2trunk 用 `IN_MODIFY` 需 hash 兜底）
  - `watchtree` 下沉目录遍历（进程内 Lua 无 lfs）；`IN_ISDIR|IN_CREATE` 新目录由 Lua 侧补 watchtree
  - 事件经内核 fd 队列积压，Lua 侧 timer tick 周期 `read()`（无 skynet 普通 fd 事件循环接入）
  - 事件名含对齐 `\0` 填充，read 内截断到首个 `\0`（实测踩坑）
- 编译：`make linux` → `luaclib/inotify.so`（Makefile `LUA_CLIB` 登记 + 独立 rule）

### luaprofile（CPU 调用树采样器，自研）

- 源码：`3rd/luaprofile/profile.c`（单文件）；背景、API 与边界见 `3rd/luaprofile/README.md`
- 用途：simple-server DX P4「CPU 采样分析」的底层采样器（主仓侧封装 `framework/debug/profiler.ts` + 控制台 `prof` / `profdump`）
- **来源：自研，不是 vendor**。原计划 vendor c2trunk `3rd/luaprofile`（上游标注 `lsg2020/skynet@4ace42e8` + c2trunk 自加 `icallpath`）；2026-09-08 核实**该源码不可得**——本地 c2trunk 引擎 submodule 未初始化、上游该 commit 的 tree 中无此目录（只有 `lualib/compat10/profile.lua`）。改为自研，接口形态（`start/stop/mark/dump` + 调用树）与 c2trunk 对齐
- 编译：`make linux` → `luaclib/profile.so`（Makefile `LUA_CLIB` 登记 + 独立 rule，`-I3rd/lua`）
- **依赖 Lua 内部头**（`3rd/lua/lobject.h`、`lstate.h`）：遍历 `global_State->allgc` 给所有线程装 `LUA_MASKCALL|LUA_MASKRET` hook，并劫持 `lua_setallocf` 统计分配——**必须透传原 ud**（引擎 `lalloc` / `resumeX` / `signal_hook` 都把该 ud 当 `struct snlua` 用，换成别的指针会立刻崩）。Lua 5.5.1 内部头可用性由 simple-server 侧 P4.0 编译探针实测确认（探针不在本仓，见 simple-server `tools/profile_probe/`）
- **不修改 `service-src/service_snlua.c`**：采样 context 存静态结构，不像 c2trunk 那样塞进 `struct snlua` 首字段（也就不需要导出 `snlua_profile_slot()`）。收益是**无 ABI 布局契约**、引擎升级不漂移；代价是进程内单采样会话（采样本就单会话 + 重入互斥）
- 计时：`clock_gettime(CLOCK_MONOTONIC)`（不用 rdtsc，保持可移植；精度 ns，采样窗口开销可接受）
- API：`start()/stop()/dump()/mark()/rescan()/running()`；节点按 `(parent, source, linedefined)` 聚合成**路径敏感**调用树，含 `count/value/self/alloc_count/alloc_bytes`

### 新增扩展的约定

1. 源码放 `3rd/` 下（第三方）或 `lualib-src/`（自研单模块，参照 `bson`/`sproto` 先例）
2. 在顶层 `Makefile` 的 `LUA_CLIB` 登记模块名，并补独立 rule（多文件扩展参照 `md5.so`/`lpeg.so`/`sproto.so` 写法）
3. 本文件"扩展库编排"节补充来源与编译说明

## 三、构建

```bash
# Linux 容器/构建机（依赖 gcc/make；3rd/lua 为 vendored 源码，3rd/jemalloc 需先 make 或 submodule init）
make linux
```

产物：`skynet`（主程序）、`cservice/*.so`、`luaclib/*.so`（含 `cjson2.so`、`pb.so`、`inotify.so`、`profile.so`）。simple-server 侧由 `shell/build_engine.sh` 将产物复制进 `engine/`。

## 四、引擎级定制（进行中/规划）

- graceful-shutdown（simple-server `docs/plans/graceful-shutdown.md` 阶段 4，P-B 路径）：
  - `skynet_start.c`：SIGINT/SIGTERM → `handle_shutdown()` → 向协调服务推 `PTYPE_TEXT "shutdown"`
  - `lua-skynet.c`：新增 `skynet.os_exit(code)`（进程干净退出）
- 定制统一收敛在 `skynet-src/`，与扩展编排（3rd 目录）互不干扰

## 五、维护

- 升级上游：`git fetch cloudwu` → merge 到本分支（保持扩展 rule 不被上游冲突覆盖）
- 发布：本分支 commit 后，simple-server 更新 `skynet` submodule gitlink 并同步容器重编
