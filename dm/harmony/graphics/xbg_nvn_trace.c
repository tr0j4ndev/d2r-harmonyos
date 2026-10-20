// NVN 入口追踪：给 xbg_game 加 -finstrument-functions，每个函数进出都会回调到这里。
// 目的：崩溃发生在图形初始化之后（客体在 NSO text 0x17417a4 拿到 NULL），宿主这边却没有任何错误。
// 记下「谁被调用 + 是客体哪段地址调的」，崩时 dump；再把我这份未 strip 的 .so 符号化，
// 就能看出崩前最后调用的是哪个 NVN 入口、游戏从哪调它——那个入口的返回值就是嫌疑。
// NVN entry trace: -finstrument-functions on the xbg_game target calls back here on every function
// entry/exit. The crash happens after graphics init, in guest code, with nothing reported on the host
// side; recording (callee, guest caller) and dumping on a fatal signal shows which NVN entry ran last
// and where the game called it from — that entry's return value is the suspect.
#include <dlfcn.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define XBG_TRACE_N (1u << 17)   // 131072 条；图形初始化的调用量很大，环要够长 | big enough for gfx init

typedef struct { uintptr_t fn, caller; int tid; } xbg_trace_ent;
static xbg_trace_ent ring[XBG_TRACE_N];
static atomic_uint written;
static atomic_int dumped;
static const int signals[3] = {SIGSEGV, SIGBUS, SIGABRT};
static struct sigaction previous[3];

void xbg_trace_install(void);

__attribute__((no_instrument_function)) static int tid_of(void) { return (int)syscall(SYS_gettid); }

__attribute__((no_instrument_function)) static void dump(int sig, siginfo_t *si, void *uc) {
    if (atomic_exchange(&dumped, 1)) return;
    /* 先链回上一个处理器（应用记录器写 crash-last.txt），再 dump 我们的环 | chain first, then dump */
    for (unsigned i = 0; i < 3; ++i) {
        if (signals[i] != sig) continue;
        struct sigaction *p = &previous[i];
        if (p->sa_flags & SA_SIGINFO) { if (p->sa_sigaction) { p->sa_sigaction(sig, si, uc); return; } }
        else if (p->sa_handler && p->sa_handler != SIG_IGN && p->sa_handler != SIG_DFL) { p->sa_handler(sig); return; }
        break;
    }
    unsigned w = atomic_load(&written), first = w > XBG_TRACE_N ? w - XBG_TRACE_N : 0;
    dprintf(2, "[xbg-trace] signal=%d thread=%d calls oldest-to-newest kept=%u total=%u\n",
            sig, tid_of(), w < XBG_TRACE_N ? w : XBG_TRACE_N, w);
    for (unsigned i = first; i < w; ++i)
        dprintf(2, "[xbg-trace] fn=%#lx caller=%#lx t=%d\n",
                (unsigned long)ring[i % XBG_TRACE_N].fn,
                (unsigned long)ring[i % XBG_TRACE_N].caller,
                ring[i % XBG_TRACE_N].tid);
    for (unsigned i = 0; i < 3; ++i) {
        if (signals[i] != sig) continue;
        struct sigaction *p = &previous[i];
        if (p->sa_flags & SA_SIGINFO) { if (p->sa_sigaction) p->sa_sigaction(sig, NULL, NULL); }
        else if (p->sa_handler && p->sa_handler != SIG_IGN && p->sa_handler != SIG_DFL) p->sa_handler(sig);
        break;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

/* 静态库成员若无人引用不会链进来，所以除了构造函数，xbg_game_nvn.c 里还会显式调一次。
 * A static-library member is only linked when referenced, so xbg_game_nvn.c calls this explicitly too. */
__attribute__((constructor, no_instrument_function)) static void xbg_trace_init(void) { xbg_trace_install(); }

__attribute__((no_instrument_function)) void xbg_trace_install(void) {
    static int once;
    if (once++) return;
    {   /* xblhost 基址：把 fn 地址换算回 .so 偏移才能符号化 | xblhost base for symbolizing fn */
        Dl_info info; extern int dladdr(const void *, Dl_info *);
        if (dladdr((void *)&xbg_trace_install, &info) && info.dli_fbase)
            dprintf(2, "[xbg-trace] module=%s base=%p\n", info.dli_fname ? info.dli_fname : "?", info.dli_fbase);
    }
    for (unsigned i = 0; i < 3; ++i) {
        struct sigaction a; memset(&a, 0, sizeof a);
        a.sa_sigaction = dump; a.sa_flags = SA_NODEFER | SA_SIGINFO; sigemptyset(&a.sa_mask);
        sigaction(signals[i], &a, &previous[i]);
    }
}

__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void *fn, void *caller) {
    unsigned i = atomic_fetch_add(&written, 1);
    xbg_trace_ent *e = &ring[i % XBG_TRACE_N];
    e->fn = (uintptr_t)fn; e->caller = (uintptr_t)caller; e->tid = tid_of();
}

// 退出也记（fn 相同、caller 为 0 表示返回）：能区分「最后一个调用有没有返回」。
// Exits are recorded too (caller 0): tells whether the last call returned.
__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void *fn, void *caller) {
    (void)caller;
    unsigned i = atomic_fetch_add(&written, 1);
    xbg_trace_ent *e = &ring[i % XBG_TRACE_N];
    e->fn = (uintptr_t)fn; e->caller = 0; e->tid = tid_of();
}
