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

### 新增扩展的约定

1. 源码放 `3rd/` 下（第三方）或 `lualib-src/`（自研单模块，参照 `bson`/`sproto` 先例）
2. 在顶层 `Makefile` 的 `LUA_CLIB` 登记模块名，并补独立 rule（多文件扩展参照 `md5.so`/`lpeg.so`/`sproto.so` 写法）
3. 本文件"扩展库编排"节补充来源与编译说明

## 三、构建

```bash
# Linux 容器/构建机（依赖 gcc/make；3rd/lua 为 vendored 源码，3rd/jemalloc 需先 make 或 submodule init）
make linux
```

产物：`skynet`（主程序）、`cservice/*.so`、`luaclib/*.so`（含 `cjson2.so`、`pb.so`）。simple-server 侧由 `shell/build_engine.sh` 将产物复制进 `engine/`。

## 四、引擎级定制（进行中/规划）

- graceful-shutdown（simple-server `docs/plans/graceful-shutdown.md` 阶段 4，P-B 路径）：
  - `skynet_start.c`：SIGINT/SIGTERM → `handle_shutdown()` → 向协调服务推 `PTYPE_TEXT "shutdown"`
  - `lua-skynet.c`：新增 `skynet.os_exit(code)`（进程干净退出）
- 定制统一收敛在 `skynet-src/`，与扩展编排（3rd 目录）互不干扰

## 五、维护

- 升级上游：`git fetch cloudwu` → merge 到本分支（保持扩展 rule 不被上游冲突覆盖）
- 发布：本分支 commit 后，simple-server 更新 `skynet` submodule gitlink 并同步容器重编
