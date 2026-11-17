#ifndef MINI_RTOS_H
#define MINI_RTOS_H

// mini-rtos —— 单核抢占式调度内核的最小实现
//
// 为什么用线程来"模拟"单核：
//   任务栈由操作系统线程承载，但调度决策（优先级、tick、抢占点、阻塞队列）
//   全部由本内核自己做，全局锁 g_lock 保证任一时刻只有一个任务真正在跑。
//   这在语义上等价于单核 MCU：不会有两个任务同时执行临界区。
//
// 这样做的好处：不需要开发板，PC 上就能复现优先级反转、优先级继承这类
// 只有"真机 + 长临界区"才会暴露的问题，而且每次跑出来的 tick 数完全可复现。

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#define MAX_TASKS      8
#define MAX_TICKS      4096     // trace 缓冲：最多记录多少 tick 的 CPU 归属
#define MAX_EVENTS     8192
#define NUM_PRIO       8        // 0 最高，7 最低（与大多数 RTOS 相反，这里按"数字小=优先级高"）

typedef enum { T_READY, T_RUNNING, T_BLOCKED, T_DONE } task_state;

typedef struct tcb {
    int         id;
    char        name[16];
    int         prio;           // 当前有效优先级（可能被继承临时抬高）
    int         base_prio;      // 原始优先级，解锁时用来还原
    task_state  state;
    void      (*fn)(void*);
    void*       arg;
    pthread_t   th;
    uint64_t    run_ticks;      // 累计占用 CPU 的 tick 数
    uint64_t    consumed;       // 该任务已消费到第几个 tick（busy 用）
    uint64_t    block_since;    // 进入阻塞时的 tick
    uint64_t    wake_at;        // 定时唤醒的 tick（sleep_ticks 用）
    int         timer;          // 1 = 该任务是定时阻塞，不看信号量
    int         waiting_sem;    // 正在等的信号量下标，-1 表示没等
    int         waited_for;     // 等的是哪个任务持有的资源（用于统计）
} tcb_t;

typedef struct {
    int count;                  // 可用计数
    int holder;                 // 互斥量当前持有者 task id；信号量为 -1
    int is_mutex;
    int inherit;                // 是否开启优先级继承
} sem_t_;

// ---- 内核 ----
void rtos_init(int tick_ms);
int  task_create(const char* name, int prio, void (*fn)(void*), void* arg);
void rtos_start(int max_ticks);
int  rtos_tick_total(void);
const int* rtos_owner_series(int* n);   // 每个 tick 占着 CPU 的任务 id

// ---- 同步原语 ----
void sem_init_(sem_t_* s, int count, int is_mutex, int inherit);
void sem_wait_(sem_t_* s, int self_id);
void sem_post_(sem_t_* s, int self_id);

// ---- 任务侧 API ----
int  self_id(void);
void busy(unsigned ticks);          // 占用 CPU n 个 tick，其间可被抢占
void sleep_ticks(unsigned ticks);   // 主动阻塞 n 个 tick（定时阻塞）

// ---- 观测 ----
void trace_dump(const char* path);

#endif
