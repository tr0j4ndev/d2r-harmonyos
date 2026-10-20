#include "execution_internal.h"
#include "hle_ohos_compat.h"
#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/syscall.h>
_Static_assert(sizeof(xblr_cpu_frame)==832,"assembly frame size");
_Static_assert(offsetof(xblr_cpu_frame,q)==256 && offsetof(xblr_cpu_frame,nzcv)==768 &&
    offsetof(xblr_cpu_frame,site)==800,"assembly offsets");
_Static_assert(sizeof(xblr_instruction_site)==56,"offline metadata ABI");
extern const xblr_image_desc xblr_execution_images[2];
/* 模块基址：分析崩溃栈/门站点时按它把运行时地址换算回模块偏移 | module bases: used to map runtime
 * addresses back to module offsets when analysing crash stacks and gate sites */
static void log_module_bases(void) {
    for(unsigned i=0;i<2;i++)
        fprintf(stderr,"[xblr-base] %s base=%p size=%llu text=%llu\n",xblr_execution_images[i].name,
            xblr_execution_images[i].base,(unsigned long long)xblr_execution_images[i].size,
            (unsigned long long)xblr_execution_images[i].text_bytes);
}
extern const xblr_hle_binding __start_xblr_hle[],__stop_xblr_hle[];
static pthread_key_t thread_key;
static pthread_once_t thread_once=PTHREAD_ONCE_INIT;
static int key_error;
static atomic_int execution_started;
static atomic_uint execution_failure,current_stage;
static atomic_uint completed_stage,execution_finished,terminal_status;
static atomic_uint_fast64_t thread_gate_limit;
static pthread_mutex_t failure_lock=PTHREAD_MUTEX_INITIALIZER;
static xblr_execution_report first_failure;
static xblr_module *modules[2];
static xblr_module_info module_info[2];
extern void xblr_threads_wake_failure(void);
extern void xblr_thread_finish_current(void);
extern void xblr_ui_observe(uint64_t message);
extern void xblr_world_tick_observe(uint64_t game);
extern void xblr_svc_abort_dispatch(void);
int xblr_execution_failed(void) {return (int)atomic_load(&execution_failure);}
static void make_key(void) { key_error=pthread_key_create(&thread_key,free); }
/* Every gate looks the thread up; the pthread key stays only for its free() destructor. */
static _Thread_local xblr_guest_thread *current_thread;
/* Generated fast gate stubs read the thread context as ((void**)*(tp-272))[key]
 * and touch virtual_x18 / tlr at these fixed offsets (rewrite_harmony.py). */
_Static_assert(offsetof(xblr_guest_thread,virtual_x18)==0,"fast gate virtual_x18 offset");
_Static_assert(offsetof(xblr_guest_thread,tlr)==0x2010,"fast gate TLR_OFFSET");
/* Byte offset of thread_key in the musl TSD array; bit 63 set keeps every fast stub
 * on the full gate until the layout has been verified on this device. */
__attribute__((visibility("hidden"))) uint64_t xblr_fast_tsd=UINT64_C(1)<<63;
static void verify_fast_tsd(xblr_guest_thread *t) {
    static atomic_int done;
    if(atomic_exchange(&done,1))return;
    uintptr_t tp;__asm__ volatile("mrs %0, tpidr_el0":"=r"(tp));
    void **tsd=*(void ***)(tp-272);
    int ok=tsd&&pthread_getspecific(thread_key)==t&&tsd[thread_key]==t;
    if(ok)__atomic_store_n(&xblr_fast_tsd,(uint64_t)thread_key*8,__ATOMIC_RELEASE);
    fprintf(stderr,"[xblr-exec] fast gates %s (key=%u)\n",ok?"enabled":"disabled: TSD layout mismatch",(unsigned)thread_key);
}
static int set_current_thread(xblr_guest_thread *t) {
    int e=pthread_setspecific(thread_key,t);if(!e)current_thread=t;
    if(!e&&t)verify_fast_tsd(t);
    return e;
}
xblr_guest_thread *xblr_current_thread(void) { return current_thread; }
_Noreturn void xblr_execution_stop(uint32_t status) {
    xblr_guest_thread *t=xblr_current_thread();
    if(!t||!t->report||!t->escape_armed)abort();
    t->report->status=status;
    fprintf(stderr,"[xblr-exec] STOP status=%u stage=%u pc=%#llx svc=%#llx gates=%llu\n",
        status,t->report->stage_entered,(unsigned long long)t->report->original_pc,
        (unsigned long long)t->report->svc_number,(unsigned long long)t->report->gates);
    pthread_mutex_lock(&failure_lock);
    if(!atomic_load(&execution_failure)) {first_failure=*t->report;atomic_store(&execution_failure,status);}
    pthread_mutex_unlock(&failure_lock);
    xblr_svc_abort_dispatch();
    xblr_threads_wake_failure();
    longjmp(t->escape,1);
}
static xblr_guest_thread *new_thread_context(xblr_execution_report *report) {
    xblr_guest_thread *t=calloc(1,sizeof *t);if(!t)return NULL;
    t->tls_pointer=(uintptr_t)t->tls;t->report=report?report:&t->local_report;
    if(!report)t->local_report=(xblr_execution_report){.abi_version=1,.struct_bytes=sizeof t->local_report,
        .max_gates=atomic_load(&thread_gate_limit),.stage_entered=atomic_load(&current_stage)};
    uintptr_t object=(uintptr_t)t->thread_object;memcpy(t->tlr+0x1f8,&object,8);
    if(set_current_thread(t)){free(t);return NULL;}return t;
}
void xblr_run_guest_thread(uint64_t entry,uint64_t arg,uint64_t sp) {
    xblr_guest_thread *t=new_thread_context(NULL);if(!t)abort();
    t->report->symbol_address=entry;
    snprintf(t->report->symbol,sizeof t->report->symbol,"guest thread entry %#llx",(unsigned long long)entry);
    t->escape_armed=1;
    if(!setjmp(t->escape)) {sw_thread_enter(entry,arg,sp);xblr_execution_stop(XBLR_EXEC_HOST_FAILURE);}
    t->escape_armed=0;set_current_thread(NULL);free(t);
}
void xblr_exit_guest_thread(void) {
    xblr_guest_thread *t=xblr_current_thread();if(!t||!t->escape_armed)abort();
    longjmp(t->escape,2);
}
uint64_t xblr_invoke_guest(void *fn,const uint64_t *a,uint32_t n) {
    if(!fn||n>8||(n&&!a)||!xblr_current_thread())abort();uint64_t v[8]={0};
    if(n)memcpy(v,a,n*sizeof *a);
    return ((uint64_t(*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))fn)(v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7]);
}
uint64_t xblr_invoke_guest_callback(void *user,void *fn,const uint64_t *a,uint32_t n) {
    (void)user;return xblr_invoke_guest(fn,a,n);
}
void xblr_graphics_fatal(void *user,const char *message) {
    (void)user;xblr_guest_thread *t=xblr_current_thread();
    if(t&&t->report)snprintf(t->report->symbol,sizeof t->report->symbol,"graphics: %.116s",message?message:"unknown");
    fprintf(stderr,"[xblr-exec] GRAPHICS_FAILURE %s\n",message?message:"unknown");
    xblr_execution_stop(XBLR_EXEC_HOST_FAILURE);
}
void *xblr_module_named_symbol(const char *module,const char *name) {
    uint64_t address=0;
    for(unsigned i=0;i<2;++i)if(!strcmp(g_mod[i].name,module)&&!xblr_module_symbol(modules[i],name,&address))return (void*)(uintptr_t)address;
    return NULL;
}
int xblr_execution_has_started(void) {return atomic_load(&execution_started);}
int xblr_get_execution_progress(xblr_execution_progress *p,size_t bytes) {
    if(!p||bytes!=sizeof *p)return XBLR_EXEC_ARGUMENT;
    *p=(xblr_execution_progress){.abi_version=1,.struct_bytes=sizeof *p};
    p->started=(uint32_t)atomic_load(&execution_started);
    p->stage_completed=atomic_load(&completed_stage);
    p->stage_entered=atomic_load(&current_stage);
    p->status=atomic_load(&execution_failure);
    if(!p->status)p->status=atomic_load(&terminal_status);
    p->active=p->started&&!atomic_load(&execution_finished)&&!p->status;
    return 0;
}
int xblr_query_module_memory(uint64_t address,uint64_t *base,uint64_t *size,uint64_t *perm) {
    for(unsigned i=0;i<2;++i) {
        const xblr_image_desc *d=&xblr_execution_images[i];uint64_t b=(uintptr_t)d->base;
        if(address<b||address-b>=d->size)continue;
        uint64_t off=address-b;
        if(off<d->ro_offset){*base=b;*size=d->ro_offset;*perm=5;}
        else if(off<d->data_offset){*base=b+d->ro_offset;*size=d->data_offset-d->ro_offset;*perm=1;}
        else {*base=b+d->data_offset;*size=d->size-d->data_offset;*perm=3;}
        return 1;
    }
    return 0;
}
void *xblr_execution_symbol(const char *name) {
    xblr_guest_thread *t=xblr_current_thread();
    if(!t||!t->report)return NULL;
    uint64_t address=0;
    for(unsigned i=0;i<2;++i)if(!xblr_module_symbol(modules[i],name,&address))return (void*)(uintptr_t)address;
    snprintf(t->report->symbol,sizeof t->report->symbol,"%s",name);
    xblr_execution_stop(XBLR_EXEC_MISSING_SYMBOL);
}
static uint64_t counter(void) {
    uint64_t ns=guest_ns();
    return (ns/1000000000u)*19200000u+(ns%1000000000u)*12u/625u;
}
/* HLE 调用追踪环：记下最近被门的操作（站点 PC + 操作函数指针），在致命信号里 dump。
 * 为什么需要：客体代码在初始化中途空指针崩了（NSO text 0x17417a4），宿主日志里没有任何错误，
 * 只有「崩之前最后几次 HLE 调用分别是谁」能指出是哪个服务给了游戏意外的东西；
 * op 指针能用未 strip 的 .so 符号化成服务名（llvm-addr2line）。
 * HLE call trace ring: site PC + operation function pointer of the last gated operations, dumped from a
 * fatal-signal handler. The host log shows no error at all, so only "which HLE calls ran last" can point at
 * the service that handed the game something unexpected. */
#define XBLR_TRACE_N 65536

typedef struct { uint64_t pc,op,tid,site,meta[3]; } xblr_trace_entry;
static xblr_trace_entry trace_ring[XBLR_TRACE_N];
static atomic_uint trace_written;
static atomic_int trace_dumped;
/* 调试期：每个门进入时记下帧指针，崩溃处理器据此 dump 完整客体寄存器。
 * Debug aid: the crashing signal handler dumps full guest registers from the last gated frame. */
static _Thread_local xblr_cpu_frame *crash_frame;
static const int trace_signals[3]={SIGSEGV,SIGBUS,SIGABRT};
static struct sigaction trace_previous[3];
#define XBLR_HLE_RING 512
typedef struct { const char *name; int tid; unsigned long x19,x20,x21; } hle_ent;
static hle_ent hle_ring[XBLR_HLE_RING];
static atomic_uint hle_written;
/* 明细环：少数关键服务的参数/返回值（ReadDirectory 的数量与名字、文件大小、读到的字节、时钟）。
 * 只记最后 64 条，崩时 dump——用来定位「哪个返回值被游戏当成了容器键」。
 * Detail ring: arguments/results of a few key services, last 64 kept, dumped on a fatal signal. */
#define XBLR_NOTE_N 256
static char note_ring[XBLR_NOTE_N][208];
static atomic_uint note_written;
void xblr_hle_note(const char *fmt, ...) {
    unsigned i=atomic_fetch_add(&note_written,1)%XBLR_NOTE_N;
    int pfx=snprintf(note_ring[i],sizeof note_ring[i],"t=%d ",gettid());
    va_list ap; va_start(ap,fmt); vsnprintf(note_ring[i]+(pfx>0?pfx:0),sizeof note_ring[i]-(pfx>0?pfx:0),fmt,ap); va_end(ap);
}
/* HLE 服务入口（由 hle_ohos_compat.h 的跳板调用）：只记名字，开销是一次原子加。
 * Called from the trampolines in hle_ohos_compat.h: records only the name. */
/* 把宿主刚用过的栈区清零：端口自己的 C 函数跑在客体线程的栈上，它们的临时数据会覆盖
 * 客体写在同一片栈上的内容（真机上根本没有这些宿主代码）。实测：游戏读自己未初始化的
 * 栈字段时读到了链接器留下的 ELF 头字节，把 0x464C457F 当成纹理宽高 → 端口算出 27 TB
 * → 游戏查表失败 → 空指针崩。清零后客体看到的就只是 0（等同真机全新栈），它自己的
 * 「为 0 就用默认值」分支会正常生效。
 * Zero the stack the host just used: the port's own C functions run on the guest thread's stack and
 * their temporaries overwrite what the guest had written in the same stack memory (on real hardware
 * none of that host code exists). Measured: reading its own uninitialised stack field, the game got
 * ELF-header bytes the dynamic linker had left behind, took 0x464C457F as the texture size, the port
 * computed 27 TB, the game's table lookup failed and it dereferenced NULL. After scrubbing the guest
 * sees plain zeros (what a fresh stack gives on real hardware) and its own "if zero use the default"
 * branch works. */
int xblr_stack_range(uint64_t addr, uint64_t *base, uint64_t *size);
static _Thread_local uintptr_t scrub_lo, scrub_hi;
void xblr_hle_scrub(void *top, size_t bytes) {
    uintptr_t t=(uintptr_t)top;
    if(!top || bytes < 16 || bytes > (1u << 20))return;
    if(t<=scrub_lo || t>scrub_hi) {
        uint64_t b,s;
        if(!xblr_stack_range(t-1,&b,&s)) {scrub_lo=scrub_hi=0;return;}
        scrub_lo=(uintptr_t)b;scrub_hi=(uintptr_t)(b+s);
    }
    if(t-scrub_lo<bytes)bytes=t-scrub_lo;
    memset((char *)t - bytes, 0, bytes);
}

void xblr_hle_trace(const char *name) {
    unsigned i=atomic_fetch_add(&hle_written,1);
    hle_ring[i%XBLR_HLE_RING].name=name;
    hle_ring[i%XBLR_HLE_RING].tid=gettid();
    /* 客体调用本服务时，callee-saved 寄存器仍是客体的（C 函数会保留它们）：
     * 记下 x19/x20/x21，就能看见游戏当时在操作哪个对象（例如纹理描述符）。
     * When the guest calls a service, the callee-saved registers still hold the guest\`s values
     * (C functions preserve them): recording x19/x20/x21 shows which object it was working on. */
    { unsigned long a,b2,c; __asm__ volatile("mov %0, x19" : "=r"(a));
      __asm__ volatile("mov %0, x20" : "=r"(b2)); __asm__ volatile("mov %0, x21" : "=r"(c));
      hle_ring[i%XBLR_HLE_RING].x19=a; hle_ring[i%XBLR_HLE_RING].x20=b2; hle_ring[i%XBLR_HLE_RING].x21=c; }
}

static void trace_dump(int sig, siginfo_t *si, void *uc) {
    (void)si;(void)uc;
    if(atomic_exchange(&trace_dumped,1)) return;
    /* 先 dump 我们的环再链给应用记录器：应用处理器会 re-raise，进程随即死亡，
     * 顺序反过来我们的环永远没机会输出。crash-last.txt 仍由应用处理器在链入时写出。
     * Dump our rings BEFORE chaining: the app recorder re-raises and the process dies,
     * so dumping after chaining never ran. crash-last.txt is still written by the app
     * handler when we chain below. */
    xblr_cpu_frame *cf=crash_frame;
    if(cf) {
        dprintf(2,"[xblr-crash] frame site=%#llx continuation=%#llx nzcv=%llx\n",
            (unsigned long long)cf->site,(unsigned long long)cf->continuation,(unsigned long long)cf->nzcv);
        for(unsigned i=0;i<31;i+=4)
            dprintf(2,"[xblr-crash] x%u=%#llx x%u=%#llx x%u=%#llx x%u=%#llx\n",
                i,(unsigned long long)cf->x[i],i+1,(unsigned long long)cf->x[i+1],i+2,(unsigned long long)cf->x[i+2],i+3,(unsigned long long)cf->x[i+3]);
        dprintf(2,"[xblr-crash] sp=%#llx\n",(unsigned long long)cf->sp);
    }
    xblr_guest_thread *ct=xblr_current_thread();
    if(ct)dprintf(2,"[xblr-crash] virtual_x18=%#llx tls=%#llx\n",(unsigned long long)ct->virtual_x18,(unsigned long long)ct->tls_pointer);
    unsigned hw=atomic_load(&hle_written),hfirst=hw>XBLR_HLE_RING?hw-XBLR_HLE_RING:0;
    unsigned hkept=hw<XBLR_HLE_RING?hw:XBLR_HLE_RING;
    dprintf(2,"[xblr-crash] signal=%d thread=%d\n",sig,gettid());
    dprintf(2,"[xblr-hle] signal=%d last HLE calls oldest-to-newest kept=%u total=%u\n",sig,hkept,hw);
    for(unsigned i=hfirst;i<hw;i++) dprintf(2,"[xblr-hle] t=%d x19=%#lx x20=%#lx x21=%#lx %s\n",hle_ring[i%XBLR_HLE_RING].tid,hle_ring[i%XBLR_HLE_RING].x19,hle_ring[i%XBLR_HLE_RING].x20,hle_ring[i%XBLR_HLE_RING].x21,hle_ring[i%XBLR_HLE_RING].name?hle_ring[i%XBLR_HLE_RING].name:"?");
    unsigned nw=atomic_load(&note_written),nf=nw>XBLR_NOTE_N?nw-XBLR_NOTE_N:0;
    dprintf(2,"[xblr-note] detail oldest-to-newest kept=%u\n",nw<XBLR_NOTE_N?nw:XBLR_NOTE_N);
    for(unsigned i=nf;i<nw;i++) dprintf(2,"[xblr-note] %s\n",note_ring[i%XBLR_NOTE_N]);
    unsigned w=atomic_load(&trace_written),first=w>XBLR_TRACE_N?w-XBLR_TRACE_N:0;
    unsigned kept=w<XBLR_TRACE_N?w:XBLR_TRACE_N;
    dprintf(2,"[xblr-trace] signal=%d gated calls oldest-to-newest kept=%u total=%u\n",sig,kept,w);
    for(unsigned i=first;i<w;i++) {
        const xblr_trace_entry *e=&trace_ring[i%XBLR_TRACE_N];
        dprintf(2,"[xblr-trace] pc=%#llx op=%#llx tid=%llu site=%#llx resume=%#llx target=%#llx ka=%#llx\n",
            (unsigned long long)e->pc,(unsigned long long)e->op,(unsigned long long)e->tid,
            (unsigned long long)e->site,(unsigned long long)e->meta[0],(unsigned long long)e->meta[1],(unsigned long long)e->meta[2]);
    }
    for(unsigned i=0;i<3;i++) {
        if(trace_signals[i]!=sig) continue;
        struct sigaction *p=&trace_previous[i];
        if(p->sa_flags&SA_SIGINFO) { if(p->sa_sigaction) p->sa_sigaction(sig,NULL,NULL); }
        else if(p->sa_handler&&p->sa_handler!=SIG_IGN&&p->sa_handler!=SIG_DFL) p->sa_handler(sig);
        break;
    }
    /* 没链上就恢复默认，让系统照常处理 | restore the default so the system still reports it */
    signal(sig,SIG_DFL);
    raise(sig);
}

static void trace_install(void) {
    for(unsigned i=0;i<3;i++) {
        struct sigaction a;memset(&a,0,sizeof a);
        a.sa_sigaction=trace_dump;a.sa_flags=SA_NODEFER|SA_SIGINFO;
        sigemptyset(&a.sa_mask);
        sigaction(trace_signals[i],&a,&trace_previous[i]);
    }
}

static void trace_named_port(xblr_guest_thread *t,const xblr_cpu_frame *f) {
    uint64_t b,n,p;char name[13]={0};uint64_t address=f->x[1];
    if(xblr_query_module_memory(address,&b,&n,&p)&&(p&1)&&n-(address-b)>=12)memcpy(name,(void*)(uintptr_t)address,12);
    LOG("ConnectToNamedPort name=%s lr=%#llx fp=%#llx",name[0]?name:"<not in module>",(unsigned long long)f->x[30],(unsigned long long)f->x[29]);
    /* Main guest runs on the dedicated pthread stack. Never dereference arbitrary
     * frame pointers or worker stacks merely to produce diagnostics. */
    if(!t->is_main)return;
    pthread_attr_t attr;void *base;size_t size;
    if(pthread_getattr_np(pthread_self(),&attr))return;
    int e=pthread_attr_getstack(&attr,&base,&size);pthread_attr_destroy(&attr);if(e||size<16)return;
    uint64_t fp=f->x[29],lo=(uintptr_t)base,hi=lo+size;
    for(unsigned i=0;i<12&&!(fp&15)&&fp>=lo&&fp>=f->sp&&fp<=hi-16;++i) {
        uint64_t pair[2];memcpy(pair,(void*)(uintptr_t)fp,sizeof pair);
        module_t *m=module_of((void*)(uintptr_t)pair[1]);
        LOG("guest frame %u %s+%#llx",i,m?m->name:"host",(unsigned long long)(m?pair[1]-(uintptr_t)m->base:pair[1]));
        if(pair[0]<=fp)break;fp=pair[0];
    }
}
/* Gate profile: per-site hit counts for a time window, printed to stderr, to find guest hot loops that
 * pay the full gate (complete register save + dispatch) on every iteration. */
#define GP_N 65536u
typedef struct { _Atomic uint64_t site; _Atomic uint64_t n; } gate_prof_slot;
static gate_prof_slot gate_prof[GP_N];
static atomic_int gate_prof_on;
static _Atomic uint64_t gate_prof_total,gate_prof_dropped;
static void gate_prof_hit(const xblr_instruction_site *s) {
    uint64_t key=(uintptr_t)s;atomic_fetch_add_explicit(&gate_prof_total,1,memory_order_relaxed);
    for(unsigned i=0,h=(unsigned)((key>>4)*0x9E3779B1u)%GP_N;i<16;i++,h=(h+1)%GP_N) {
        uint64_t cur=atomic_load_explicit(&gate_prof[h].site,memory_order_relaxed);
        if(!cur){uint64_t z=0;if(atomic_compare_exchange_strong(&gate_prof[h].site,&z,key)||z==key){atomic_fetch_add_explicit(&gate_prof[h].n,1,memory_order_relaxed);return;}continue;}
        if(cur==key){atomic_fetch_add_explicit(&gate_prof[h].n,1,memory_order_relaxed);return;}
    }
    atomic_fetch_add_explicit(&gate_prof_dropped,1,memory_order_relaxed);
}
static int gate_prof_cmp(const void *a,const void *b){uint64_t x=((const gate_prof_slot*)a)->n,y=((const gate_prof_slot*)b)->n;return x<y?1:x>y?-1:0;}
static void *gate_prof_run(void *arg) {
    unsigned seconds=(unsigned)(uintptr_t)arg;sleep(seconds);atomic_store(&gate_prof_on,0);usleep(50000);
    static gate_prof_slot copy[GP_N];memcpy(copy,gate_prof,sizeof copy);qsort(copy,GP_N,sizeof copy[0],gate_prof_cmp);
    uint64_t kinds[12]={0};for(unsigned i=0;i<GP_N;i++)if(copy[i].site){const xblr_instruction_site *s=(const void*)(uintptr_t)copy[i].site;kinds[s->kind<12?s->kind:11]+=copy[i].n;}
    uint64_t total=atomic_load(&gate_prof_total);
    fprintf(stderr,"[xblr-gateprof] seconds=%u gates=%llu per_s=%.0f dropped=%llu kinds 0=%llu 1=%llu 2=%llu 3=%llu 4=%llu 5=%llu 6=%llu 7=%llu 8=%llu 9=%llu 10=%llu 11+=%llu\n",seconds,(unsigned long long)total,(double)total/seconds,(unsigned long long)atomic_load(&gate_prof_dropped),
        (unsigned long long)kinds[0],(unsigned long long)kinds[1],(unsigned long long)kinds[2],(unsigned long long)kinds[3],(unsigned long long)kinds[4],(unsigned long long)kinds[5],(unsigned long long)kinds[6],(unsigned long long)kinds[7],(unsigned long long)kinds[8],(unsigned long long)kinds[9],(unsigned long long)kinds[10],(unsigned long long)kinds[11]);
    for(unsigned i=0;i<80&&copy[i].site;i++){const xblr_instruction_site *s=(const void*)(uintptr_t)copy[i].site;
        fprintf(stderr,"[xblr-gateprof] #%u n=%llu per_s=%.0f kind=%u arg=%#x extra=%u pc=%#llx op=%p\n",i,(unsigned long long)copy[i].n,(double)copy[i].n/seconds,s->kind,s->arg,s->extra,(unsigned long long)s->original_pc,(void*)s->operation);}
    return NULL;
}
__attribute__((visibility("default"))) void xblr_gate_profile(int seconds) {
    if(seconds<1||seconds>30||atomic_load(&gate_prof_on))return;
    memset(gate_prof,0,sizeof gate_prof);atomic_store(&gate_prof_total,0);atomic_store(&gate_prof_dropped,0);atomic_store(&gate_prof_on,1);
    pthread_t th;if(pthread_create(&th,NULL,gate_prof_run,(void*)(uintptr_t)seconds)==0)pthread_detach(th);else atomic_store(&gate_prof_on,0);
}
void xblr_instruction_dispatch(xblr_cpu_frame *f) {
    if(__builtin_expect(atomic_load_explicit(&gate_prof_on,memory_order_relaxed),0))gate_prof_hit((const void*)(uintptr_t)f->site);
    crash_frame=f;
    xblr_guest_thread *t=xblr_current_thread();
    if(!t)abort();
    xblr_execution_report *r=t->report;
    if(atomic_load(&execution_failure)) {
        pthread_mutex_lock(&failure_lock);*r=first_failure;pthread_mutex_unlock(&failure_lock);
        longjmp(t->escape,1);
    }
    const xblr_instruction_site *s=(const void*)(uintptr_t)f->site;
    if(!t->is_main)r->stage_entered=atomic_load(&current_stage);
    r->original_pc=s->original_pc;r->guest_sp=f->sp;
    ++r->gates;if(r->max_gates&&r->gates>r->max_gates)xblr_execution_stop(XBLR_EXEC_GATE_LIMIT);
    f->x[18]=t->virtual_x18;f->continuation=s->resume;
    switch(s->kind) {
    case 0: if(__builtin_expect(g_flag.dbg,0)) {
        /* Off by default: a shared atomic per gate is measurable at millions of gates
         * per second. tid is the thread context pointer (no gettid syscall); meta[2]
         * is guest x19, which located the op that clobbered callee-saved registers. */
        unsigned ti=atomic_fetch_add_explicit(&trace_written,1,memory_order_relaxed);
        xblr_trace_entry *te=&trace_ring[ti%XBLR_TRACE_N];
        te->pc=s->original_pc;te->op=(uintptr_t)s->operation;te->tid=(uint64_t)(uintptr_t)t;
        te->site=f->site;te->meta[0]=s->resume;te->meta[1]=s->target;te->meta[2]=f->x[19];
    }
        if(s->arg==0x5549)xblr_ui_observe(f->x[1]);else if(s->arg==0x5754)xblr_world_tick_observe(f->x[0]);s->operation(f);break;
    case 1: if(s->arg<31)f->x[s->arg]=t->tls_pointer;break;
    case 2: t->tls_pointer=s->arg<31?f->x[s->arg]:0;break;
    case 3: if(s->arg<31)f->x[s->arg]=(uintptr_t)t->tlr;break;
    case 4: if(s->arg<31)f->x[s->arg]=counter();break;
    case 5:
        r->svc_number=s->arg;memcpy(r->svc_args,f->x,sizeof r->svc_args);
        if(s->arg==0x1f)trace_named_port(t,f);
        svc_dispatch((int)s->arg,f->x,s->original_pc);
        break;
    case 6: f->x[18]=s->target;break;
    case 7: case 8: {
        uint64_t value=s->arg==32?(uint32_t)f->x[18]:f->x[18];
        if((value==0)==(s->kind==7))f->continuation=s->taken;
        break;
    }
    case 9: case 10:
        if((((f->x[18]>>s->extra)&1)==0)==(s->kind==9))f->continuation=s->taken;
        break;
    case 11: {
        uint64_t delta=f->x[18]-s->target;
        if(f->x[18]<s->target||delta%4||delta/4>=s->extra)xblr_execution_stop(XBLR_EXEC_BAD_BRANCH);
        f->continuation=s->taken+(delta/4)*8;break;
    }
    default:xblr_execution_stop(XBLR_EXEC_BAD_BRANCH);
    }
    t->virtual_x18=f->x[18];
}
static void invoke(uint32_t stage,const char *name,uint64_t a,uint64_t b) {
    xblr_guest_thread *t=xblr_current_thread();xblr_execution_report *r=t->report;
    r->stage_entered=stage;snprintf(r->symbol,sizeof r->symbol,"%s",name);
    atomic_store(&current_stage,stage);
    r->symbol_address=(uintptr_t)xblr_execution_symbol(name);
    fprintf(stderr,"[xblr-exec] ENTER stage=%u symbol=%s address=%#llx\n",stage,name,(unsigned long long)r->symbol_address);
    r->result_x0=((uint64_t(*)(uint64_t,uint64_t))(uintptr_t)r->symbol_address)(a,b);
    r->stage_completed=stage;
    atomic_store(&completed_stage,stage);
    fprintf(stderr,"[xblr-exec] RETURN stage=%u symbol=%s gates=%llu\n",stage,name,(unsigned long long)r->gates);
}
typedef struct { xblr_guest_thread *t; uint32_t max_stage; xblr_execution_report *r; } xblr_boot;

/* 客体程序的 main 线程在真机上跑的是**全新栈（内核给的零页）**。端口原先直接跑在应用的
 * 调用线程上——那根栈已经被应用自己和动态链接器用过（链接器读 ELF 头会留下 7f 45 4c 46 …），
 * 游戏读到这些残留，把 ELF 魔数当成纹理宽高（实测 0x464C457F），端口就忠实地算出 27 TB 的
 * 尺寸，游戏查自己的分配表失败后解引用了 NULL（NSO text 0x17417a4）。
 * 所以这里给客体开一根**自己 mmap 的栈**（匿名映射＝零页，懒提交，不花实际内存），
 * 在它上面跑完整段启动序列，恢复真机语义。
 * On real hardware the guest program's main thread runs on a fresh stack (zero pages from the kernel).
 * The port used to run it directly on the app's calling thread, whose stack had already been used by the
 * app and the dynamic linker (reading an ELF header leaves 7f 45 4c 46 ... behind); the game read those
 * leftovers, took the ELF magic as a texture size (measured 0x464C457F), the port faithfully computed a
 * 27 TB size, the game's own allocation-table lookup failed and it dereferenced NULL (NSO text 0x17417a4).
 * So give the guest its own mmap'd stack (anonymous = zero pages, lazily committed) and run the whole
 * startup sequence on it, restoring the real-hardware semantics. */
static void *xblr_boot_thread(void *arg) {
    xblr_boot *b=arg;xblr_guest_thread *t=b->t;xblr_execution_report *r=b->r;uint32_t max_stage=b->max_stage;
    t->handle=svc_thread_main_init();
    int escaped=setjmp(t->escape);
    if(escaped) {
        /* Explicit main ExitThread also publishes only on the restored native stack. */
        if(escaped==2)xblr_thread_finish_current();
        goto done;
    }
    invoke(1,"_init_libc0",0,0);if(max_stage==1)goto done;
    invoke(2,"nnosInitialize",t->handle,0);if(max_stage==2)goto done;
    invoke(3,"_init_libc1",0,0);if(max_stage==3)goto done;
    invoke(4,"nninitInitializeSdkModule",0,0);if(max_stage==4)goto done;
    invoke(5,"nninitStartup",0,0);if(max_stage==5)goto done;
    invoke(6,"_init_libc2",0,0);if(max_stage==6)goto done;
    r->stage_entered=7;
    atomic_store(&current_stage,7);
    for(unsigned i=0;i<2;++i) {
        uint64_t p=(uintptr_t)xblr_execution_images[i].base+module_info[i].init_offset;
        if(module_info[i].init_offset)((void(*)(void))(uintptr_t)p)();
    }
    r->stage_completed=7;atomic_store(&completed_stage,7);if(max_stage==7)goto done;
    static char *argv[]={"D2R",NULL}; /* preserve original host/main.c argument contract */
    invoke(8,"_ZN2nn2os11SetHostArgcEi",1,0);
    invoke(8,"_ZN2nn2os11SetHostArgvEPPc",(uintptr_t)argv,0);if(max_stage==8)goto done;
    {extern int xblr_padmode_boot(void);int pe=xblr_padmode_boot();if(pe)xblr_execution_stop((uint32_t)pe);}
    invoke(9,"nnMain",0,0);
done:
    t->escape_armed=0;t->report=NULL;set_current_thread(NULL);
    return NULL;
}

int xblr_execute_startup(uint32_t max_stage,uint64_t max_gates,xblr_execution_report *r,size_t bytes) {
    if(!r||bytes!=sizeof *r||max_stage<1||max_stage>9||(!max_gates&&max_stage!=9))return XBLR_EXEC_ARGUMENT;
    trace_install();
    log_module_bases();
    *r=(xblr_execution_report){.abi_version=1,.struct_bytes=sizeof *r,.max_gates=max_gates,.page_bytes=(uint32_t)sysconf(_SC_PAGESIZE)};
    if(r->page_bytes!=4096)return r->status=XBLR_EXEC_PAGE_SIZE;
    if(atomic_exchange(&execution_started,1))return r->status=XBLR_EXEC_BUSY;
    pthread_once(&thread_once,make_key);
    if(key_error) {atomic_store(&terminal_status,XBLR_EXEC_TLS);atomic_store(&execution_finished,1);return r->status=XBLR_EXEC_TLS;}
    atomic_store(&thread_gate_limit,max_gates);
    xblr_guest_thread *t=new_thread_context(r);
    if(!t) {atomic_store(&terminal_status,XBLR_LOAD_MEMORY);atomic_store(&execution_finished,1);return r->status=XBLR_LOAD_MEMORY;}
    t->is_main=1;t->escape_armed=1;
    for(unsigned i=0;i<2;++i) {
        int e=xblr_bind_image(&xblr_execution_images[i],&modules[i],&module_info[i]);
        if(e)xblr_execution_stop((uint32_t)e);
        memcpy(g_mod[i].name,xblr_execution_images[i].name,16);g_mod[i].base=(uint8_t*)xblr_execution_images[i].base;g_mod[i].size=xblr_execution_images[i].size;
    }
    g_nmod=2;g_t0_ns=now_ns();
    g_flag.thr=!!getenv("SW_THR");g_flag.svc=!!getenv("SW_SVC");g_flag.fs=!!getenv("SW_FS");
    g_flag.movie=!!getenv("SW_MOVIE");g_flag.dbg=!!getenv("SW_DBG");g_flag.imports=!!getenv("SW_IMPORTS");g_flag.bootprof=!!getenv("SW_BOOTPROF");
    xblr_link_report link;
    int e=xblr_link_modules(modules,2,__start_xblr_hle,(size_t)(__stop_xblr_hle-__start_xblr_hle),1,NULL,NULL,&link);
    if(e)xblr_execution_stop((uint32_t)e);
    r->relocations_applied=link.writes_applied;
    /* 客体启动序列跑在**自己 mmap 的零页栈**上（见 xblr_boot_thread 的注释）。
     * The guest startup sequence runs on its own mmap'd zero-page stack (see xblr_boot_thread). */
    {
        pthread_attr_t ba;pthread_attr_init(&ba);
        size_t ss=32u<<20;
        void *stk=mmap(NULL,ss,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(stk!=MAP_FAILED)pthread_attr_setstack(&ba,stk,ss);
        else fprintf(stderr,"[xblr] 零页栈 mmap 失败，退回应用线程栈（可能重现残留问题）\n");
        xblr_boot b={t,max_stage,r};
        /* 新线程会让客体缺端口 TLS 状态（实测启动即崩：一条 stage 都没跑到），所以仍在原线程上跑。
'
         * A new thread leaves the guest without the port\`s TLS state (measured: it crashed during startup
'
         * with not a single stage reached), so keep running on the calling thread. */
        (void)ba;
        xblr_boot_thread(&b);
    }
    atomic_store(&terminal_status,r->status);atomic_store(&execution_finished,1);
    return (int)r->status;
}
