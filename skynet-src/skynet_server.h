#ifndef SKYNET_SERVER_H
#define SKYNET_SERVER_H

#include <stdint.h>
#include <stdlib.h>

struct skynet_context;
struct skynet_message;
struct skynet_monitor;

uint32_t skynet_context_new(const char * name, const char * parm);
void skynet_context_grab(struct skynet_context *);
void skynet_context_reserve(struct skynet_context *ctx);
void skynet_context_release(struct skynet_context *);
uint32_t skynet_context_handle(struct skynet_context *);
int skynet_context_push(uint32_t handle, struct skynet_message *message);
// 引擎扩展（simple-engine，2026-09-16 perf T5）：唤醒正在 pthread_cond_wait 的 worker。
// 为什么需要：skynet_mq_push 只把队列挂进 global queue、**不唤醒任何 worker**，而 worker
// 只在 thread_timer 每 2.5ms 的 wakeup() 时被叫醒 ⇒ 空闲服务收到**跨线程 push**（sngo 的
// skynet_context_push：池响应/redis 订阅/etcd watch/MQ 叫醒）的消息要等下一个 tick。
// 实测代价：闭合环每调用固定 +2.5ms（conc=1/2 时 p50 全钉在 2.57ms，见 simple-server
// docs/plans/perf-evaluation.md §十.5 T5）。skynet_context_push 内调用本函数即消除该延迟。
void skynet_wakeup_worker(void);
void skynet_context_send(struct skynet_context * context, void * msg, size_t sz, uint32_t source, int type, int session);
int skynet_context_newsession(struct skynet_context *);
struct message_queue * skynet_context_message_dispatch(struct skynet_monitor *, struct message_queue *, int weight);	// return next queue
int skynet_context_total();
void skynet_context_dispatchall(struct skynet_context * context);	// for skynet_error output before exit

void skynet_context_endless(uint32_t handle);	// for monitor

void skynet_globalinit(void);
void skynet_globalexit(void);
void skynet_initthread(int m);

void skynet_profile_enable(int enable);

#endif
