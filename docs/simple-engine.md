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

### luaprofile（CPU 调用树采样器，vendor c2trunk + 符号契约补丁）

- 源码：`3rd/luaprofile/`（`profile.c` + `imap.c` + `icallpath.c` + 4 个头文件）；来源、补丁、API 与边界见 `3rd/luaprofile/README.md`
- 用途：simple-server DX P4「CPU 采样分析」的底层采样器（主仓侧封装 `framework/debug/profiler.ts` + 控制台 `prof` / `profdump`）
- **来源：vendor c2trunk 引擎仓**（`D:\workspace\skynet-engine\skynet\3rd\luaprofile`，上游标注 `lsg2020/skynet@4ace42e8`）。历程：2026-09-08 曾因"本地找不到源码"先自研一版（`1bcc174`），同日用户指出旧引擎真实位置后**切回 vendor，自研版弃用**（代码在该 commit 历史，决策记录见 README §1）
- 编译：`make linux` → `luaclib/profile.so`（Makefile `LUA_CLIB` 登记 + 独立 rule，`imap.c icallpath.c profile.c` 三文件一起编——上游 makefile 漏了 icallpath.c）
- **依赖 Lua 内部头**（`3rd/lua/lobject.h`、`lstate.h`）：遍历 `global_State->allgc` 给所有线程装 `CALL|RET` hook，并劫持 `lua_setallocf` 统计分配——**必须透传原 ud**（引擎 `lalloc` / `resumeX` / `signal_hook` 都把该 ud 当 `struct snlua` 用）
- **引擎侧改动（唯一一处，符号契约）**：`service-src/service_snlua.c` 的 `struct snlua` **首位**加 `void * profile_context;`（c2trunk 同款，勿调整字段顺序）+ 导出 `void ** snlua_profile_slot(struct snlua *l)`；C 库**删掉了本地 `struct snlua` 定义**、只经该函数存取——布局契约降级为符号契约，上游 bump 时失败模式为 dlopen 显式报错而非静默写坏内存。**注意 snlua.so 与 profile.so 必须同代**（旧 snlua.so 无该字段时，profile.so 会读到野指针）
- API（对齐 c2trunk `require "profile.c"`）：`start/stop/mark/unmark/dump`；`dump` 返回 `(time_us, root_node)`，根节点 `total` 的 `children` 为路径敏感调用树（`name/count/value/rettime/alloc_count(字节)/alloc_times(次数)/children`）；**stop 不返回数据，须先 dump**；`count/alloc` 父节点取 max(children,self) 非求和

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
- **跨线程 push 唤醒 worker（2026-09-16，perf-evaluation §十.6 T5）**：
  - 问题：`skynet_mq_push` 只把队列挂进 global queue、**不唤醒任何 worker**；worker 睡在
    `pthread_cond_wait(&m->cond)`，全仓只有 `thread_timer` 每 **2.5ms**（`usleep(2500)`）的
    `wakeup(m, m->count-1)` 与 `thread_socket` 事件会 signal ⇒ **空闲服务收到外部 push 的消息要等
    一个 tick**。实测代价：sngo 池调用闭合环每调用固定 +2.5ms（conc=1 时 p50 全钉在 2.57ms）。
  - 改动（3 处，共 +43 行）：`skynet_server.h` 声明 `skynet_wakeup_worker()`；`skynet_start.c`
    定义它（`start()` 登记 `G_MONITOR`；`m->sleep != 0` 时**持 `m->mutex`** 发 `cond_signal`——
    持锁才能杜绝 lost-wakeup；sleep==0 直接返回，消息热路径零锁开销，丢一次信号也只会退化为
    原行为，由 2.5ms tick 兜底）；`skynet_server.c` 的 `skynet_context_push` 在 `skynet_mq_push`
    之后调用它。
  - 收益面：所有跨线程 push 路径（sngo 池响应 / redis 订阅推送 / etcd watch / MQ 分片叫醒 /
    SIGTERM 关机指令）。实测：L2 闭合环 conc=1 **385 → 2 521 qps（6.5×）**，conc=8 redis 类
    1.3 万 → 2.1 万 qps、p99 2.6ms → 0.9ms。
- 定制统一收敛在 `skynet-src/`，与扩展编排（3rd 目录）互不干扰

## 五、维护

- 升级上游：`git fetch cloudwu` → merge 到本分支（保持扩展 rule 不被上游冲突覆盖）
- 发布：本分支 commit 后，simple-server 更新 `skynet` submodule gitlink 并同步容器重编
