#include "rtos.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

// ============================ 内核全局状态 ============================
static tcb_t  g_tasks[MAX_TASKS];
static int    g_ntasks = 0;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;

static volatile int g_tick    = 0;    // 当前 tick
static volatile int g_running = 0;    // 1=内核在跑；0=正在停机，所有阻塞点立刻返回
static int          g_tick_ms = 1;
static int          g_current = -1;   // 当前占着 CPU 的任务
static int          g_switch  = 0;    // 上下文切换次数

// trace：每个 tick 是谁在跑（Gantt 图的数据）
static int g_owner[MAX_TICKS];
static int g_owner_n = 0;

// trace：事件流
typedef struct { int tick, task, prio; char what[24]; } ev_t;
static ev_t g_ev[MAX_EVENTS];
static int  g_nev = 0;

static sem_t_* g_sems[16];
static int     g_nsems = 0;

static void evp(const char* what, int task, int prio) {
    if (g_nev < MAX_EVENTS) {
        g_ev[g_nev].tick = g_tick;
        g_ev[g_nev].task = task;
        g_ev[g_nev].prio = prio;
        snprintf(g_ev[g_nev].what, sizeof(g_ev[g_nev].what), "%s", what);
        g_nev++;
    }
}

static void ev(const char* what, int task) { evp(what, task, -1); }

static int sem_index(sem_t_* s) {
    for (int i = 0; i < g_nsems; ++i) if (g_sems[i] == s) return i;
    return -1;
}

int self_id(void) {
    pthread_t me = pthread_self();
    for (int i = 0; i < g_ntasks; ++i)
        if (pthread_equal(g_tasks[i].th, me)) return i;
    return -1;
}

// ============================ 调度器 ============================
// 选优先级最高的就绪任务；同优先级按 id 先后（简易 round-robin 的近似）
static int highest_ready(void) {
    int best = -1;
    for (int i = 0; i < g_ntasks; ++i) {
        if (g_tasks[i].state == T_READY || g_tasks[i].state == T_RUNNING) {
            if (best < 0 || g_tasks[i].prio < g_tasks[best].prio) best = i;
        }
    }
    return best;
}

static void* sched_main(void* arg) {
    (void)arg;
    struct timespec ts = {0, 0};
    while (1) {
        ts.tv_sec  = 0;
        ts.tv_nsec = (long)g_tick_ms * 1000000L;
        nanosleep(&ts, NULL);

        pthread_mutex_lock(&g_lock);
        if (!g_running) { pthread_mutex_unlock(&g_lock); break; }
        g_tick++;

        // 1) 定时阻塞到点 -> 转就绪
        for (int i = 0; i < g_ntasks; ++i) {
            if (g_tasks[i].timer && g_tasks[i].state == T_BLOCKED &&
                (uint64_t)g_tick >= g_tasks[i].wake_at) {
                g_tasks[i].timer = 0;
                g_tasks[i].state = T_READY;
                ev("timer", i);
            }
        }

        // 2) 抢占 / 切换
        int best = highest_ready();
        if (best >= 0) {
            if (g_current != best) {
                if (g_current >= 0 && g_tasks[g_current].state == T_RUNNING) {
                    g_tasks[g_current].state = T_READY;
                    ev("preempt", g_current);
                }
                g_tasks[best].state = T_RUNNING;
                g_current = best;
                g_switch++;
                ev("switch", best);
            }
            g_tasks[g_current].run_ticks++;
        }

        // 3) 记 Gantt
        if (g_owner_n < MAX_TICKS) g_owner[g_owner_n++] = (best >= 0 ? g_current : -1);

        pthread_cond_broadcast(&g_cond);
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

static void* task_tramp(void* p) {
    int id = (int)(intptr_t)p;
    pthread_mutex_lock(&g_lock);
    while (g_running && g_tasks[id].state != T_RUNNING) pthread_cond_wait(&g_cond, &g_lock);
    pthread_mutex_unlock(&g_lock);

    if (g_running) g_tasks[id].fn(g_tasks[id].arg);

    pthread_mutex_lock(&g_lock);
    g_tasks[id].state = T_DONE;
    ev("done", id);
    if (g_current == id) g_current = -1;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

int task_create(const char* name, int prio, void (*fn)(void*), void* arg) {
    if (g_ntasks >= MAX_TASKS) return -1;
    int id = g_ntasks++;
    tcb_t* t = &g_tasks[id];
    memset(t, 0, sizeof(*t));
    t->id = id;
    snprintf(t->name, sizeof(t->name), "%s", name);
    t->prio = t->base_prio = prio;
    t->state = T_READY;
    t->fn = fn;
    t->arg = arg;
    t->waiting_sem = -1;
    t->waited_for = -1;
    return id;
}

void rtos_start(int max_ticks) {
    pthread_t st;
    pthread_create(&st, NULL, sched_main, NULL);
    for (int i = 0; i < g_ntasks; ++i)
        pthread_create(&g_tasks[i].th, NULL, task_tramp, (void*)(intptr_t)i);

    pthread_mutex_lock(&g_lock);
    g_running = 1;
    int next = highest_ready();
    if (next >= 0) {
        g_tasks[next].state = T_RUNNING;
        g_current = next;
        ev("switch", next);
    }
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);

    // 等到"全部任务结束"或"到 max_ticks"
    for (;;) {
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&g_lock);
        int all_done = 1;
        for (int i = 0; i < g_ntasks; ++i) if (g_tasks[i].state != T_DONE) all_done = 0;
        int t = g_tick;
        pthread_mutex_unlock(&g_lock);
        if (all_done || t >= max_ticks) break;
    }

    pthread_mutex_lock(&g_lock);
    g_running = 0;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);

    for (int i = 0; i < g_ntasks; ++i) pthread_join(g_tasks[i].th, NULL);
    pthread_join(st, NULL);
}

void rtos_init(int tick_ms) {
    g_tick_ms = tick_ms;
    g_tick = 0;
    g_current = -1;
    g_running = 0;
    g_nev = 0;
    g_owner_n = 0;
    g_ntasks = 0;
    g_nsems = 0;
    g_switch = 0;
    memset(g_tasks, 0, sizeof(g_tasks));
}

int rtos_tick_total(void) { return g_tick; }

const int* rtos_owner_series(int* n) { if (n) *n = g_owner_n; return g_owner; }

// ============================ 任务侧 API ============================
// 消耗 n 个 tick 的"CPU 时间"。
// 关键点：这里不自己 sleep，而是等调度器把 g_tick 推上去 —— 任务的工作严格
// 落在 tick 边界上，跟调度决策同源，所以同一份配置每次跑出来的 tick 数一致。
// 被更高优先级抢占时只是"消费不到 tick"，剩余工作量记在循环计数里，不会丢。
void busy(unsigned ticks) {
    int me = self_id();
    if (me < 0) return;
    for (unsigned i = 0; i < ticks; ++i) {
        pthread_mutex_lock(&g_lock);
        int stop = 0;
        for (;;) {
            if (!g_running) { stop = 1; break; }
            if (g_tasks[me].state == T_RUNNING && (uint64_t)g_tick > g_tasks[me].consumed) break;
            pthread_cond_wait(&g_cond, &g_lock);
        }
        if (!stop) g_tasks[me].consumed = (uint64_t)g_tick;
        pthread_mutex_unlock(&g_lock);
        if (stop) return;
    }
}

void sleep_ticks(unsigned ticks) {
    int me = self_id();
    if (me < 0) return;
    pthread_mutex_lock(&g_lock);
    if (!g_running) { pthread_mutex_unlock(&g_lock); return; }
    g_tasks[me].state   = T_BLOCKED;
    g_tasks[me].timer   = 1;
    g_tasks[me].wake_at = (uint64_t)g_tick + ticks;
    g_tasks[me].block_since = (uint64_t)g_tick;
    ev("sleep", me);
    if (g_current == me) g_current = -1;
    pthread_cond_broadcast(&g_cond);

    while (g_running && g_tasks[me].state != T_RUNNING) pthread_cond_wait(&g_cond, &g_lock);
    if (!g_running) { pthread_mutex_unlock(&g_lock); return; }
    if (g_tasks[me].state != T_RUNNING) pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

// ============================ 信号量 / 互斥量 ============================
void sem_init_(sem_t_* s, int count, int is_mutex, int inherit) {
    s->count   = count;
    s->holder  = -1;
    s->is_mutex = is_mutex;
    s->inherit  = inherit;
    if (g_nsems < 16) g_sems[g_nsems++] = s;
}

void sem_wait_(sem_t_* s, int me) {
    if (me < 0) return;
    pthread_mutex_lock(&g_lock);

    while (s->count <= 0) {
        if (!g_running) { pthread_mutex_unlock(&g_lock); return; }

        tcb_t* t = &g_tasks[me];
        t->state       = T_BLOCKED;
        t->block_since = (uint64_t)g_tick;
        t->waiting_sem = sem_index(s);
        t->waited_for  = s->holder;
        ev("block", me);

        // ★ 优先级继承：把持锁者抬到跟自己一样高，避免被中间优先级的任务插队
        if (s->is_mutex && s->inherit && s->holder >= 0) {
            int h = s->holder;
            if (g_tasks[h].prio > t->prio) {
                g_tasks[h].prio = t->prio;
                evp("inherit", h, t->prio);
            }
        }

        if (g_current == me) g_current = -1;
        pthread_cond_broadcast(&g_cond);

        while (g_running && t->state == T_BLOCKED) pthread_cond_wait(&g_cond, &g_lock);
        if (!g_running) { pthread_mutex_unlock(&g_lock); return; }

        // 单核语义：被唤醒只代表"进就绪队列"，必须真的被调度到才能往下走
        while (g_running && t->state != T_RUNNING) pthread_cond_wait(&g_cond, &g_lock);
        if (!g_running) { pthread_mutex_unlock(&g_lock); return; }
    }

    s->count--;
    if (s->is_mutex) s->holder = me;
    ev("lock", me);
    pthread_mutex_unlock(&g_lock);
}

void sem_post_(sem_t_* s, int me) {
    pthread_mutex_lock(&g_lock);

    if (s->is_mutex) {
        // 释放：把被继承抬高的优先级还原成 base_prio。
        // 注意不能拿"正在等这把锁的人"的优先级来算 —— 那个等待者马上就被唤醒，
        // 再把它算进来会导致优先级永远降不回去（LOW 会一直停在 prio 1）。
        // 真正该考虑的是"我还持有别的锁、别的任务还在等我"，本例只有一把互斥量。
        if (me >= 0) {
            int restore = g_tasks[me].base_prio;
            if (restore != g_tasks[me].prio) evp("restore", me, restore);
            g_tasks[me].prio = restore;
        }
        s->holder = -1;
        ev("unlock", me);
    }
    s->count++;

    // 唤醒优先级最高的那个等待者
    int pick = -1;
    for (int i = 0; i < g_ntasks; ++i) {
        if (g_tasks[i].state == T_BLOCKED && g_tasks[i].waiting_sem == sem_index(s)) {
            if (pick < 0 || g_tasks[i].prio < g_tasks[pick].prio) pick = i;
        }
    }
    if (pick >= 0) {
        g_tasks[pick].state       = T_READY;
        g_tasks[pick].waiting_sem = -1;
        g_tasks[pick].waited_for  = -1;
        ev("wake", pick);
    }

    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

// ============================ trace 导出 ============================
void trace_dump(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }

    fprintf(f, "{\"tickMs\":%d,\"ticks\":%d,\"switches\":%d,\n", g_tick_ms, g_tick, g_switch);

    fprintf(f, " \"tasks\":[");
    for (int i = 0; i < g_ntasks; ++i)
        fprintf(f, "%s{\"id\":%d,\"name\":\"%s\",\"prio\":%d,\"basePrio\":%d,\"runTicks\":%llu}",
                i ? "," : "", i, g_tasks[i].name, g_tasks[i].prio,
                g_tasks[i].base_prio, (unsigned long long)g_tasks[i].run_ticks);
    fprintf(f, "],\n");

    fprintf(f, " \"owner\":[");
    for (int i = 0; i < g_owner_n; ++i) fprintf(f, "%s%d", i ? "," : "", g_owner[i]);
    fprintf(f, "],\n");

    fprintf(f, " \"events\":[");
    for (int i = 0; i < g_nev; ++i)
        fprintf(f, "%s{\"t\":%d,\"task\":%d,\"e\":\"%s\",\"p\":%d}", i ? "," : "",
                g_ev[i].tick, g_ev[i].task, g_ev[i].what, g_ev[i].prio);
    fprintf(f, "]}\n");

    fclose(f);
}
