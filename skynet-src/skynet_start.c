#include "skynet.h"
#include "skynet_server.h"
#include "skynet_imp.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "skynet_module.h"
#include "skynet_timer.h"
#include "skynet_monitor.h"
#include "skynet_socket.h"
#include "skynet_daemon.h"
#include "skynet_harbor.h"

#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

struct monitor {
	int count;
	struct skynet_monitor ** m;
	pthread_cond_t cond;
	pthread_mutex_t mutex;
	int sleep;
	int quit;
};

struct worker_parm {
	struct monitor *m;
	int id;
	int weight;
};

static volatile int SIG = 0;
static volatile int F_SHUTDOWN = 0;

static void
handle_hup(int signal) {
	if (signal == SIGHUP) {
		SIG = 1;
	}
}

static void
handle_shutdown_signal(int signal) {
	if (signal == SIGINT || signal == SIGTERM) {
		F_SHUTDOWN = 1;
	}
}

#define CHECK_ABORT if (skynet_context_total()==0) break;

static void
create_thread(pthread_t *thread, void *(*start_routine) (void *), void *arg) {
	if (pthread_create(thread,NULL, start_routine, arg)) {
		fprintf(stderr, "Create thread failed");
		exit(1);
	}
}

static void
wakeup(struct monitor *m, int busy) {
	if (m->sleep >= m->count - busy) {
		// signal sleep worker, "spurious wakeup" is harmless
		pthread_cond_signal(&m->cond);
	}
}

// 引擎扩展（simple-engine，2026-09-16 perf T5）：外部线程投递消息后立即唤醒睡眠 worker。
//
// 问题：worker 无消息时睡在 pthread_cond_wait(&m->cond) 上；全仓只有两处 signal——
// thread_timer 每 2.5ms 的 wakeup(m, m->count-1) 与 thread_socket 的 socket 事件。
// 而 skynet_mq_push（跨线程 push 的唯一入口）**只把队列挂进 global queue、不 signal**
// ⇒ 空闲服务收到外部 push 的消息要等下一个 2.5ms tick 才被取出。实测：sngo 池调用闭合环
// 每调用固定 +2.5ms（conc=1/2 全部场景 p50=2.57ms；conc=8 时队列常非空、tick 被摊薄）。
//
// 实现要点（为什么这样写）：
//   - 用 G_MONITOR（start() 里登记）而不是 S/全局单例：struct monitor 是本文件私有的；
//   - 先读 m->sleep 再决定是否上锁：sleep==0 表示没有睡眠 worker（高负载常态）→ 直接返回，
//     不引入消息热路径的额外锁开销；读到 0 但恰好有 worker 正在入睡时可能"丢失一次信号"，
//     此时退化为原行为（下一个 2.5ms tick 兜底）——不引入新故障模式；
//   - 真正 signal 时**持 m->mutex**：worker 是持锁 ++sleep 后 cond_wait 的，持锁 signal
//     才能杜绝经典的 lost-wakeup（signal 落在"已判定无消息、尚未 cond_wait"的窗口里）；
//   - 多次 signal 无害（spurious wakeup 由 dispatch 循环自然处理）。
static struct monitor * G_MONITOR = NULL;

void
skynet_wakeup_worker(void) {
	struct monitor * m = G_MONITOR;
	if (m == NULL || m->sleep == 0) {
		return;
	}
	pthread_mutex_lock(&m->mutex);
	pthread_cond_signal(&m->cond);
	pthread_mutex_unlock(&m->mutex);
}

static void *
thread_socket(void *p) {
	struct monitor * m = p;
	skynet_initthread(THREAD_SOCKET);
	skynet_handle_register_thread();
	for (;;) {
		int r = skynet_socket_poll();
		if (r==0)
			break;
		if (r<0) {
			CHECK_ABORT
			continue;
		}
		wakeup(m,0);
	}
	return NULL;
}

static void
free_monitor(struct monitor *m) {
	int i;
	int n = m->count;
	for (i=0;i<n;i++) {
		skynet_monitor_delete(m->m[i]);
	}
	pthread_mutex_destroy(&m->mutex);
	pthread_cond_destroy(&m->cond);
	skynet_free(m->m);
	skynet_free(m);
}

static void *
thread_monitor(void *p) {
	struct monitor * m = p;
	int i;
	int n = m->count;
	skynet_initthread(THREAD_MONITOR);
	skynet_handle_register_thread();
	for (;;) {
		CHECK_ABORT
		for (i=0;i<n;i++) {
			skynet_monitor_check(m->m[i]);
		}
		for (i=0;i<5;i++) {
			CHECK_ABORT
			sleep(1);
		}
	}

	return NULL;
}

static void
signal_hup() {
	// make log file reopen

	struct skynet_message smsg;
	smsg.source = 0;
	smsg.session = 0;
	smsg.data = NULL;
	smsg.sz = (size_t)PTYPE_SYSTEM << MESSAGE_TYPE_SHIFT;
	uint32_t logger = skynet_handle_findname("logger");
	if (logger) {
		skynet_context_push(logger, &smsg);
	}
}

static void
signal_shutdown() {
	// send TEXT "shutdown" to .shutdown_srv, triggering graceful shutdown coordinator
	uint32_t handle = skynet_handle_findname(".shutdown_srv");
	if (handle == 0) {
		// fallback: try "shutdown_srv" without dot (some skynet versions strip dot)
		handle = skynet_handle_findname("shutdown_srv");
	}
	if (handle) {
		struct skynet_message smsg;
		smsg.source = 0;
		smsg.session = 0;
		// "shutdown" as TEXT message data, sz includes type bits
		smsg.data = skynet_malloc(9); // "shutdown\0"
		memcpy(smsg.data, "shutdown", 9);
		smsg.sz = 9 | ((size_t)PTYPE_TEXT << MESSAGE_TYPE_SHIFT);
		skynet_context_push(handle, &smsg);
	} else {
		// no shutdown_srv registered, just log to stderr
		fprintf(stderr, "SIGTERM/SIGINT received but no .shutdown_srv found, exiting\n");
		exit(0);
	}
}

static void *
thread_timer(void *p) {
	struct monitor * m = p;
	skynet_initthread(THREAD_TIMER);
	skynet_handle_register_thread();
	for (;;) {
		skynet_updatetime();
		skynet_socket_updatetime();
		CHECK_ABORT
		wakeup(m,m->count-1);
		usleep(2500);
		if (SIG) {
			signal_hup();
			SIG = 0;
		}
		if (F_SHUTDOWN) {
			signal_shutdown();
			F_SHUTDOWN = 0;
		}
	}
	// wakeup socket thread
	skynet_socket_exit();
	// wakeup all worker thread
	pthread_mutex_lock(&m->mutex);
	m->quit = 1;
	pthread_cond_broadcast(&m->cond);
	pthread_mutex_unlock(&m->mutex);
	return NULL;
}

static void *
thread_worker(void *p) {
	struct worker_parm *wp = p;
	int id = wp->id;
	int weight = wp->weight;
	struct monitor *m = wp->m;
	struct skynet_monitor *sm = m->m[id];
	skynet_initthread(THREAD_WORKER);
	skynet_handle_register_thread();
	struct message_queue * q = NULL;
	while (!m->quit) {
		q = skynet_context_message_dispatch(sm, q, weight);
		if (q == NULL) {
			if (pthread_mutex_lock(&m->mutex) == 0) {
				++ m->sleep;
				// "spurious wakeup" is harmless,
				// because skynet_context_message_dispatch() can be call at any time.
				if (!m->quit)
					pthread_cond_wait(&m->cond, &m->mutex);
				-- m->sleep;
				if (pthread_mutex_unlock(&m->mutex)) {
					fprintf(stderr, "unlock mutex error");
					exit(1);
				}
			}
		}
	}
	return NULL;
}

static void
start(int thread) {
	pthread_t pid[thread+3];

	struct monitor *m = skynet_malloc(sizeof(*m));
	memset(m, 0, sizeof(*m));
	m->count = thread;
	m->sleep = 0;

	m->m = skynet_malloc(thread * sizeof(struct skynet_monitor *));
	int i;
	for (i=0;i<thread;i++) {
		m->m[i] = skynet_monitor_new();
	}
	if (pthread_mutex_init(&m->mutex, NULL)) {
		fprintf(stderr, "Init mutex error");
		exit(1);
	}
	if (pthread_cond_init(&m->cond, NULL)) {
		fprintf(stderr, "Init cond error");
		exit(1);
	}

	G_MONITOR = m;	// 引擎扩展（simple-engine，2026-09-16 perf T5）：登记给 skynet_wakeup_worker

	create_thread(&pid[0], thread_monitor, m);
	create_thread(&pid[1], thread_timer, m);
	create_thread(&pid[2], thread_socket, m);

	static int weight[] = {
		-1, -1, -1, -1, 0, 0, 0, 0,
		1, 1, 1, 1, 1, 1, 1, 1,
		2, 2, 2, 2, 2, 2, 2, 2,
		3, 3, 3, 3, 3, 3, 3, 3, };
	struct worker_parm wp[thread];
	for (i=0;i<thread;i++) {
		wp[i].m = m;
		wp[i].id = i;
		if (i < sizeof(weight)/sizeof(weight[0])) {
			wp[i].weight= weight[i];
		} else {
			wp[i].weight = 0;
		}
		create_thread(&pid[i+3], thread_worker, &wp[i]);
	}

	for (i=0;i<thread+3;i++) {
		pthread_join(pid[i], NULL);
	}

	free_monitor(m);
}

static void
bootstrap(uint32_t logger_handle, const char * cmdline) {
	int sz = strlen(cmdline);
	char name[sz+1];
	char args[sz+1];
	int arg_pos;
	sscanf(cmdline, "%s", name);
	arg_pos = strlen(name);
	if (arg_pos < sz) {
		while(cmdline[arg_pos] == ' ') {
			arg_pos++;
		}
		strncpy(args, cmdline + arg_pos, sz);
	} else {
		args[0] = '\0';
	}
	const uint32_t handle = skynet_context_new(name, args);
	if (handle == 0) {
		struct skynet_context *logger = skynet_handle_grab(logger_handle);
		if (logger != NULL) {
			skynet_error(NULL, "Bootstrap error : %s\n", cmdline);
			skynet_context_dispatchall(logger);
			skynet_context_release(logger);
		}
		exit(1);
	}
}

void
skynet_start(struct skynet_config * config) {
	// register SIGHUP for log file reopen
	struct sigaction sa;
	sa.sa_handler = &handle_hup;
	sa.sa_flags = SA_RESTART;
	sigfillset(&sa.sa_mask);
	sigaction(SIGHUP, &sa, NULL);

	// register SIGINT/SIGTERM for graceful shutdown (send "shutdown" to coordinator)
	struct sigaction sa_shutdown;
	sa_shutdown.sa_handler = &handle_shutdown_signal;
	sa_shutdown.sa_flags = SA_RESTART;
	sigfillset(&sa_shutdown.sa_mask);
	sigaction(SIGINT, &sa_shutdown, NULL);
	sigaction(SIGTERM, &sa_shutdown, NULL);

	if (config->daemon) {
		if (daemon_init(config->daemon)) {
			exit(1);
		}
	}
	skynet_harbor_init(config->harbor);
	skynet_handle_init(config->harbor, config->thread);
	skynet_mq_init();
	skynet_module_init(config->module_path);
	skynet_timer_init();
	skynet_socket_init();
	skynet_profile_enable(config->profile);

	const uint32_t logger_handle = skynet_context_new(config->logservice, config->logger);
	if (logger_handle == 0) {
		fprintf(stderr, "Can't launch %s service\n", config->logservice);
		exit(1);
	}

	skynet_handle_namehandle(logger_handle, "logger");

	bootstrap(logger_handle, config->bootstrap);

	start(config->thread);

	// harbor_exit may call socket send, so it should exit before socket_free
	skynet_harbor_exit();
	skynet_socket_free();
	if (config->daemon) {
		daemon_exit(config->daemon);
	}
}
