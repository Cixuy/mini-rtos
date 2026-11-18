#include "rtos.h"
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#endif

// 输出目录不存在就建一层（不让用户先 mkdir）
static void ensure_dir(const char* d) {
#ifdef _WIN32
    _mkdir(d);
#else
    mkdir(d, 0755);
#endif
}

// =====================================================================
// 场景：三个任务，复现"优先级反转"
//
//   LOW  (prio 6) : 拿到互斥量 M 后进临界区干 60 个 tick（模拟擦 Flash）
//   MED  (prio 4) : 第 5 个 tick 才到达，纯计算 150 个 tick，不碰任何锁
//   HIGH (prio 1) : 第 8 个 tick 才到达，要 M，拿不到就阻塞
//
// 不继承时：LOW 被 MED 抢占后干不了活，HIGH 明明优先级最高，
//          却要等 MED 跑完 + LOW 剩下的临界区 —— 这就是优先级反转。
// 继承时  ：HIGH 一阻塞，LOW 立刻被抬到 prio 1，MED 抢不动它，
//          HIGH 的等待时间退化成"LOW 剩余临界区"。
// =====================================================================

static sem_t_ M;
static int    g_med_id  = -1;
static int    g_low_id  = -1;
static int    g_high_id = -1;

static uint64_t g_high_block_t = 0;
static uint64_t g_high_got_t   = 0;
static uint64_t g_high_blocked = 0;
static uint64_t g_low_cs       = 0;

static void low_task(void* a) {
    (void)a;
    int me = self_id();
    sem_wait_(&M, me);
    uint64_t t0 = (uint64_t)rtos_tick_total();
    busy(60);                                   // 临界区：擦 4KB Flash + 写配置
    g_low_cs = (uint64_t)rtos_tick_total() - t0;
    sem_post_(&M, me);
}

static void med_task(void* a) {
    (void)a;
    sleep_ticks(5);                             // 第 5 个 tick 才"到达"
    busy(150);                                  // 纯计算，跟锁无关
}

static void high_task(void* a) {
    (void)a;
    int me = self_id();
    sleep_ticks(8);                             // 第 8 个 tick 才"到达"
    g_high_block_t = (uint64_t)rtos_tick_total();
    sem_wait_(&M, me);
    g_high_got_t   = (uint64_t)rtos_tick_total();
    g_high_blocked = g_high_got_t - g_high_block_t;
    busy(5);                                    // 拿到锁后干一小会儿
    sem_post_(&M, me);
}

typedef struct {
    const char* name;
    int         inherit;
    uint64_t    high_blocked;
    uint64_t    low_cs;
    uint64_t    high_got;
    uint64_t    high_block_t;
    uint64_t    med_during_block;
    uint64_t    total;
    int         switches;
    int         own_n;                  // 该场景的 owner 序列快照
    int         own[MAX_TICKS];
} result_t;

static result_t run_scenario(const char* name, int inherit, const char* trace_path) {
    result_t r;
    memset(&r, 0, sizeof(r));
    r.name = name;
    r.inherit = inherit;

    rtos_init(1);
    g_high_block_t = g_high_got_t = g_high_blocked = g_low_cs = 0;

    sem_init_(&M, 1, /*is_mutex=*/1, /*inherit=*/inherit);

    g_high_id = task_create("HIGH", 1, high_task, NULL);
    g_med_id  = task_create("MED",  4, med_task,  NULL);
    g_low_id  = task_create("LOW",  6, low_task,  NULL);

    rtos_start(600);

    r.high_blocked = g_high_blocked;
    r.low_cs       = g_low_cs;
    r.high_got     = g_high_got_t;
    r.high_block_t = g_high_block_t;
    r.total        = (uint64_t)rtos_tick_total();

    // 统计 HIGH 被阻塞期间，MED 白占了多少 tick（插队代价）+ 快照 owner 序列
    {
        int n = 0, t;
        const int* owner = rtos_owner_series(&n);
        for (t = (int)g_high_block_t; t < (int)g_high_got_t && t < n; ++t)
            if (owner[t] == g_med_id) r.med_during_block++;
        r.own_n = (n < MAX_TICKS) ? n : MAX_TICKS;
        memcpy(r.own, owner, (size_t)r.own_n * sizeof(int));
    }

    trace_dump(trace_path);
    return r;
}

// ---------------- 终端时间线 ----------------
static void draw_timeline(const result_t* r, int task_id, char ch) {
    const int* owner = r->own;
    int n = r->own_n;
    const int W = 96;
    int i, t;
    for (i = 0; i < W; ++i) {
        int a = (int)((long)i * n / W);
        int b = (int)((long)(i + 1) * n / W);
        int hit = 0;
        for (t = a; t < b && t < n; ++t) if (owner[t] == task_id) { hit = 1; break; }
        putchar(hit ? ch : '.');
    }
    putchar('\n');
}

static void print_scene(const result_t* r) {
    printf("\n=== %s ===\n", r->name);
    {   // 让总 tick 数正好落在时间线末端（8 + 96 列）的右边缘
        int i;
        printf("  tick 0");
        for (i = 8; i < 8 + 96 - 3; ++i) putchar(' ');
        printf("%3d\n", (int)r->total);
    }
    printf("  LOW   "); draw_timeline(r, g_low_id,  '#');
    printf("  MED   "); draw_timeline(r, g_med_id,  '+');
    printf("  HIGH  "); draw_timeline(r, g_high_id, '*');
    printf("        (#=LOW  +=MED  *=HIGH  .=空闲)\n");
}

// ---------------- 表格：按显示列宽对齐（中文占两列） ----------------
static int disp_w(const char* s) {
    int w = 0;
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        if (*p < 0x80)                { w += 1; p += 1; }
        else if ((*p & 0xE0) == 0xC0) { w += 1; p += 2; }
        else if ((*p & 0xF0) == 0xE0) { w += 2; p += 3; }
        else                          { w += 2; p += 4; }
    }
    return w;
}

static void pad_l(const char* s, int w) {
    int d = disp_w(s), i;
    printf("%s", s);
    for (i = d; i < w; ++i) putchar(' ');
}

static void pad_r(const char* s, int w) {
    int d = disp_w(s), i;
    for (i = d; i < w; ++i) putchar(' ');
    printf("%s", s);
}

static void pad_num(unsigned long long v, int w) {
    char buf[32];
    int d, i;
    snprintf(buf, sizeof(buf), "%llu", v);
    d = (int)strlen(buf);
    for (i = d; i < w; ++i) putchar(' ');
    printf("%s", buf);
}

static void print_table(const result_t* a, const result_t* b) {
    double ratio = b->high_blocked ? (double)a->high_blocked / (double)b->high_blocked : 0.0;
    printf("\n  ");
    pad_l("指标", 34); pad_r("不继承", 9); printf("   ");
    pad_r("继承", 9); printf("   说明\n");
    printf("  ------------------------------------------------------------------------\n");

    printf("  "); pad_l("HIGH 被阻塞的 tick 数", 34);
    pad_num(a->high_blocked, 9); printf("   ");
    pad_num(b->high_blocked, 9); printf("   %.1fx\n", ratio);

    printf("  "); pad_l("  其中被 MED 插队", 34);
    pad_num(a->med_during_block, 9); printf("   ");
    pad_num(b->med_during_block, 9); printf("   优先级反转的直接代价\n");

    printf("  "); pad_l("HIGH 拿到锁的时刻 (tick)", 34);
    pad_num(a->high_got, 9); printf("   ");
    pad_num(b->high_got, 9); printf("\n");

    printf("  "); pad_l("LOW 临界区实际耗时 (tick)", 34);
    pad_num(a->low_cs, 9); printf("   ");
    pad_num(b->low_cs, 9); printf("   代码里只写了 busy(60)\n");

    printf("  "); pad_l("整场运行总 tick", 34);
    {
        char t1[32], t2[32];
        snprintf(t1, sizeof(t1), "~%llu", (unsigned long long)a->total);
        snprintf(t2, sizeof(t2), "~%llu", (unsigned long long)b->total);
        pad_r(t1, 9); printf("   "); pad_r(t2, 9);
        printf("   收尾有 ±1 抖动\n");
    }

    printf("  ------------------------------------------------------------------------\n");
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    const char* outdir = "out";
    int json = 0, repeat = 1, i;
    char pa[512], pb[512], ps[512];
    result_t a, b;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--json") == 0) json = 1;
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) outdir = argv[++i];
        else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) repeat = atoi(argv[++i]);
    }

    ensure_dir(outdir);

    snprintf(pa, sizeof(pa), "%s/trace_no_inherit.json", outdir);
    snprintf(pb, sizeof(pb), "%s/trace_inherit.json", outdir);
    snprintf(ps, sizeof(ps), "%s/summary.json", outdir);

    if (!json) {
        printf("mini-rtos - 单核抢占式调度 / 优先级反转复现\n");
        printf("tick=1ms  LOW(prio6) 临界区 60 tick  MED(prio4) 150 tick  HIGH(prio1) 第 8 tick 到达\n");
    }

    a = run_scenario("场景 A：不开启优先级继承", 0, pa);
    b = run_scenario("场景 B：开启优先级继承",   1, pb);

    if (json) {
        printf("{\"no_inherit\":{\"highBlocked\":%llu,\"lowCs\":%llu,\"medDuringBlock\":%llu,"
               "\"highGot\":%llu,\"total\":%llu},"
               "\"inherit\":{\"highBlocked\":%llu,\"lowCs\":%llu,\"medDuringBlock\":%llu,"
               "\"highGot\":%llu,\"total\":%llu}}\n",
               (unsigned long long)a.high_blocked, (unsigned long long)a.low_cs,
               (unsigned long long)a.med_during_block, (unsigned long long)a.high_got,
               (unsigned long long)a.total,
               (unsigned long long)b.high_blocked, (unsigned long long)b.low_cs,
               (unsigned long long)b.med_during_block, (unsigned long long)b.high_got,
               (unsigned long long)b.total);
        return 0;
    }

    int soak_bad = 0;
    // ---- 自检：同一份配置重复跑 N 次，关键指标必须完全一致 ----
    // 这条检查把 README 里"连跑 5 次指标一致"的结论变成可执行断言，
    // 一旦 tick 驱动哪天被改坏（比如又退回 nanosleep），CI 会以非 0 退出。
    if (repeat > 1) {
        int bad = 0, k;
        for (k = 1; k < repeat; ++k) {
            result_t a2 = run_scenario("场景 A", 0, pa);
            result_t b2 = run_scenario("场景 B", 1, pb);
            int ok = (a2.high_blocked == a.high_blocked) &&
                     (a2.low_cs       == a.low_cs)       &&
                     (a2.med_during_block == a.med_during_block) &&
                     (b2.high_blocked == b.high_blocked) &&
                     (b2.low_cs       == b.low_cs);
            if (!ok) {
                ++bad;
                printf("  第 %d 次不一致：A highBlocked %llu->%llu  lowCs %llu->%llu\n", k + 1,
                       (unsigned long long)a.high_blocked, (unsigned long long)a2.high_blocked,
                       (unsigned long long)a.low_cs, (unsigned long long)a2.low_cs);
            }
        }
        printf("\n[自检] 重复 %d 次，%s（A: 阻塞 %llu tick / 临界区 %llu tick）\n",
               repeat, bad ? "存在不一致，tick 驱动可能被改坏" : "全部一致",
               (unsigned long long)a.high_blocked, (unsigned long long)a.low_cs);
        soak_bad = bad;
    }

    print_scene(&a);
    print_scene(&b);
    print_table(&a, &b);
    printf("  trace 已写入 %s / trace_inherit.json / summary.json\n", outdir);

    FILE* f = fopen(ps, "w");
    if (f) {
        fprintf(f,
            "{\"tickMs\":1,\"a\":{\"name\":\"no-inherit\",\"highBlocked\":%llu,\"medDuringBlock\":%llu,"
            "\"highGot\":%llu,\"lowCs\":%llu,\"total\":%llu},\n"
            " \"b\":{\"name\":\"inherit\",\"highBlocked\":%llu,\"medDuringBlock\":%llu,"
            "\"highGot\":%llu,\"lowCs\":%llu,\"total\":%llu}}\n",
            (unsigned long long)a.high_blocked, (unsigned long long)a.med_during_block,
            (unsigned long long)a.high_got, (unsigned long long)a.low_cs, (unsigned long long)a.total,
            (unsigned long long)b.high_blocked, (unsigned long long)b.med_during_block,
            (unsigned long long)b.high_got, (unsigned long long)b.low_cs, (unsigned long long)b.total);
        fclose(f);
    }
    return soak_bad ? 1 : 0;   // 自检不一致时让 make / CI 失败
}
