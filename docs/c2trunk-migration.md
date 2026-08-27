# c2trunk 服务端 → 新 skynet 仓库 迁移规划

> 调研时间：2026-08-27
> 调研对象：`F:\workspace\c2trunk\server\`（在用的 skynet 游戏服务端）
> 迁移目标：本仓库（`f:\workspace_github\skynet`，cloudwu/skynet 的 fork）

## 一、项目全貌

SLG 策略游戏《战团争锋》服务端，微服务架构。

- **技术栈**：定制版 Skynet(2.6.1) + Golang(网关/服务发现) + TypeScript→Lua
- **游戏服务**：`hall / world / battle / union / cache / rank / warworld / march`，另配 Golang 的 `grpcgate` 网关进程
- **通信**：Skynet IPC（进程内）、自定义帧协议 gRPC（跨进程）、EntityMQ（实体消息队列）
- **数据库**：MongoDB / Redis / Etcd 全部经 gRPC 代理访问（不走 skynet 原生接口）

## 二、组件盘点（按来源划分）

| 层 | 内容 | 存放位置 / 状态 |
|---|---|---|
| 引擎 | 定制 skynet 2.6.1（cloudwu 魔改 + jemalloc + lprof + 定制 luaclib + sngo Go 绑定） | 源码在 `D:\workspace\skynet-engine`（内部 git 仓库 192.168.1.210）；编译产物 `engine/skynet+cservice+luaclib` 只在部署机，工作区仅有 `engine/version` |
| 框架 Lua | `boot/loader·preload·setup_paths`、`share/lib/core/*`、`lib/base/*`（service/server_event/hot_update/setup_*/monitor）、`service/*` | 工作区只有部分（loader/preload 在，setup_paths、share/lib/core、service/ 等缺失） |
| TS 业务源码 | `agent/ hall/ world/ framework/ lib/ share/ service/init/` | 本地有较多，但 `agent/activity_*` 等活动模块缺失（完整版在 SVN/部署机） |
| 协议 | `logic/pb`：base/grpc/entity_mq/grpcgate/tcaplus/cache/chat/factory/role/battle/echo.proto | 本地齐全；gengpc 生成 TS d.ts，sproto 用于 RPC |
| 配置与启动 | `config/*.ini + *_grpcgate.yml + k8s_*`、`launcher/* + env + server.sh` | 本地齐全 |
| 数据 | `serverdata/`（gmcmds + mapdata fhdc） | 本地齐全 |
| 工具链 | `toolchains/`：tslua-go/tsgo/genrpc/gengpc/lua/xlgen 等（tslua 源码 `D:\workspace\tslua`，xlgen 源码 `D:\workspace\xlgen`） | 本地齐全 |
| 部署 | 私服 Docker gsvip(105) → 远程 `/app/server`，VS Code SFTP 自动同步（忽略 engine/toolchains/logic/ts）；k8s 生产多区 | `c2trunk\server\.vscode\sftp.json` |

## 三、关键发现与风险

1. **引擎是魔改版，不是 vanilla**：现有 `engine/version=2.6.1`，含自定义配置键（`auto_update_lua / mlog_perfhub / option_save_using_bytes / rpc_log_level / codecache`）和大量定制库。本仓库是 cloudwu/skynet **v1.8.0（vanilla）**——版本 gap 是迁移最大风险点。
2. **工作区是不完整检出**：`preload.lua` 引用的 `boot/setup_paths.lua`、`share/lib/core/*`、`lib/base/setup_*` 本地不存在；`logic/lua` 的 `service/` 目录、TS 活动模块也缺失。完整源码需从 SVN(192.168.1.210) 或部署机 `/app/server` 获取。
3. **强依赖自研工具链**：TS→Lua 靠 tslua/tsgo，RPC/协议靠 genrpc/gengpc，导表靠 xlgen——必须一并迁移，否则业务代码无法编译。
4. **数据库不走 skynet 原生接口**：MongoDB/Redis/Etcd 全部经 gRPC（grpcgate 代理）。
5. **Go 组件独立**：grpcgate（含 ace/mongo/redis/etcd/pulsar/tpns/tcaplus 等 handler）、服务发现、sngo 是独立大件。

## 四、移植内容清单（按重要优先排序）

### P0 — 引擎与运行骨架（先让框架能跑）
1. **引擎策略决策 + 编译**：新 fork v1.8.0 直接编译 or 合并定制引擎。需移植定制能力：定制 `3rd/luaclib`（lcjson/llz4/lfs/iconv/xxtea/luniq/misc/proc/webclient…）、lualib 定制（reload.lua 等）、sngo（Go 运行时绑定）、jemalloc、lprof、Lua 5.3/5.4。
2. **框架层 Lua**：`boot/loader.lua`、`boot/preload.lua`、`boot/setup_paths.lua`、`share/lib/core/*`（module/class/tslualib/traceback）、`lib/base/*`（service/server/server_event/hot_update/setup_logging/setup_timer/setup_time_zone/monitor/setup_xlsdb）。
3. **引擎产物与编译脚本**：`engine/skynet + cservice/ + luaclib/` 目录 + 构建脚本（对应原 `build_engine.sh`）。

### P1 — 编译与通信框架（业务代码前置）
4. **工具链**：tslua-go/tsgo（+ tsconfig.tslua-go.json）、genrpc、gengpc、xlgen，及 .vscode/tasks.json 构建任务。
5. **TS 框架层**：`framework/skynet`（ipc/skynet/http/cjson/ldump/lprof/lz4/map2d/md5/redis/sproto）、`framework/core`（engine/logger/i18n）、`share/framework/event/eventhub`、`lib/net`（grpc/entity_mq/mongo/redis/etcd/tpns/tcaplus/gscc/pulsar…）、`lib/router`、`lib/cache`。
6. **协议与代码生成**：`logic/pb` 全部 proto + `share/rpc`（define.ts/service.ts/channel.ts）+ sproto 运行时。
7. **Go 组件**：grpcgate（含各协议 handler + config + 构建脚本）+ 服务发现（etcd）+ sngo。

### P2 — 服务配置与启动（让各服务能拉起）
8. **配置**：`config/*.ini` + `*_grpcgate.yml` + `k8s_*` 变体（world/hall/cache/rank/union/warworld/battle/march）。
9. **启动脚本**：`launcher/*` + `env` + `server.sh`。
10. **数据**：`serverdata/`（gmcmds.lua + mapdata/fhdc）。

### P3 — 业务逻辑（游戏功能本体）
11. **agent/**：玩家实体核心（本地已有部分，需从 SVN 补全 `agent/activity_*` 等活动模块、core 等）。
12. **hall/world/union/rank/cache 等服务**：TS 源码 + 编译产物 lua。
13. **测试**：`logic/ts/src/test/` + jesta 框架。

### P4 — 部署与运维
14. **部署**：Docker/k8s 配置、SFTP 同步、shell 脚本、私服 gsvip 对接。
15. **监控与日志**：qlog/mlog/monitor 上报链路（`mlog_perfhub`、`qlog_addr` 等配置）。
16. **文档**：toolchains/docs（大量模块文档，可作移植核对依据）。

## 五、待决策事项

1. **引擎策略**：新仓库用 vanilla v1.8.0 重建（API 差异大、需重新适配），还是把现有 `skynet-engine`（2.6.1 定制版）整体作为引擎合入新仓库（改动最小、最稳）？
2. **完整源码获取**：从部署机 `/app/server` 或 SVN 拉取缺失的框架层（`service/`、`setup_paths`、`share/lib/core`、`agent/activity_*`）作为移植素材。
