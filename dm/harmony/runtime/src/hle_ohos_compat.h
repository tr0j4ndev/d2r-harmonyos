// Existing HLE source's platform registration adapter; game function bodies are unchanged.
#ifndef XBLR_HLE_OHOS_COMPAT_H
#define XBLR_HLE_OHOS_COMPAT_H
#include "xblr_loader.h"
#include "xblr_execution.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <stdint.h>
#include <errno.h>
#define LOG(...) do { fprintf(stderr,"[xblr] " __VA_ARGS__);fputc('\n',stderr); } while(0)
#define die(...) do { LOG("FATAL " __VA_ARGS__);abort(); } while(0)
struct xblr_hle_flags { int thr,svc,fs,movie,dbg,imports,bootprof; };
extern struct xblr_hle_flags g_flag;
extern uint64_t g_t0_ns;
uint64_t now_ns(void);
void *guest_sym(const char *name);
typedef uint32_t Result;
#define RES_OK 0u
#define RES_FAIL 0x2A2u
#define TARGET_OS_IPHONE 0
enum {PROF_FS_READ,PROF_WAIT_LOCK,PROF_WAIT_CV,PROF_WAIT_ADDR,PROF_WAIT_SYNC,PROF_SLEEP,PROF_MAIN_WAIT,PROF_GFX_PRESENT,PROF_GFX_SHADER,PROF_TACT,PROF_N};
void sw_prof_add(int id,uint64_t ns,uint64_t bytes);
void sw_prof_mark(const char *name);
uint64_t guest_ns(void);
int sw_detclock_enabled(void);
void sw_guest_clock_advanced(void);
uint64_t sw_guest_frame(void);
void sw_guest_frame_tick(void);
uint64_t sw_entropy64(void);
void guest_clock_freeze(int on);
void guest_clock_freeze_for(int reason,int on);
void *objmap_get(const void *guest);
void objmap_put(const void *guest,void *host);
void *objmap_del(const void *guest);
void hle_bind_system_event(void *event,int autoclear);
void hle_signal_system_event(void *event);
void sw_guest_write_prepare(void *addr,size_t bytes);
void sw_guest_write_prep_register(void (*fn)(void *,size_t));
void crash_watch_frame(void);
void xblr_pump_ui_touch(void);
void xblr_world_note_stick(int32_t lx,int32_t ly);
uint64_t xblr_pump_ui_frontend(void);
void uimsg_subscribe(void (*fn)(const char*,const char*,const char*));
int xblr_clone_tree(const char *from,const char *to,int flags);
int xblr_exchange_save(const char *from,const char *to,int flags);
void xblr_run_guest_thread(uint64_t entry,uint64_t arg,uint64_t sp);
void xblr_exit_guest_thread(void);
int xblr_execution_failed(void);
void xblr_guest_thread_init(void);
void sw_thread_enter(uint64_t entry,uint64_t arg,uint64_t sp);
uint32_t svc_thread_main_init(void);
int svc_thread(int num,uint64_t *regs);
typedef struct {char name[16];uint8_t *base;size_t size;} module_t;
#define MAX_MOD 8
extern module_t g_mod[MAX_MOD];
extern int g_nmod;
module_t *module_of(const void *address);
void *module_sym(module_t *module,const char *name);
void sw_backtrace(uint64_t fp);
void hlecount_svc(int number);
uint64_t svc_host_call(int number,uint64_t *regs);
uint64_t svc_dispatch(int number,uint64_t *regs,uint64_t original_pc);
int sw_opmode(void);
void sw_focus_set(int focus);
void sw_touch_set(const xblr_touch *touches,int count);
int sw_audio_suspend(int suspended);
uint64_t sw_audio_render_calls(void);
int front_text_input(const char *title,const char *subtitle,const char *guide,const char *initial,int maximum_chars,char *output,int output_bytes);
const char *xblr_romfs_rest(const char *mount_root,const char *guest_path);
const char *xblr_romfs_name(const char *parent,const char *physical_name);
int xblr_romfs_is_root(const char *hostpath,const char *guestpath);
int xblr_fs_guest_name_matches(const char *hostpath,const char *guestpath);
int xblr_graphics_configured(void);
/* HLE 调用跳板：把绑定地址从服务函数换成尾调用跳板——记下服务名（存进环形缓冲，崩时 dump）
 * 再 b 回真函数。为什么要生成汇编：服务签名各不相同（含 void 返回、浮点参数），
 * 用 C 包装器无法通吃；跳板保存所有参数寄存器（含 q0–q7）再尾跳，任何签名都成立。
 * HLE call trampoline: the binding points at a trampoline that records the service name into a ring
 * buffer and tail-calls the real function. It has to be assembly: the services have different
 * signatures (void returns, float args), which no single C wrapper can forward; saving all argument
 * registers (including q0-q7) and tail-jumping works for every signature. */
#define HLE_TRAMPOLINE(cname,mangled) \
    static const char cname##_tracename[] __attribute__((used)) = mangled; \
    __attribute__((naked,used)) static void cname##_tramp(void) { \
        __asm__ volatile( \
            "sub sp, sp, #0xE0\n\t" \
            "stp x0, x1, [sp, #0x00]\n\t" \
            "stp x2, x3, [sp, #0x10]\n\t" \
            "stp x4, x5, [sp, #0x20]\n\t" \
            "stp x6, x7, [sp, #0x30]\n\t" \
            "stp x8, x30, [sp, #0x40]\n\t" \
            "stp q0, q1, [sp, #0x50]\n\t" \
            "stp q2, q3, [sp, #0x70]\n\t" \
            "stp q4, q5, [sp, #0x90]\n\t" \
            "stp q6, q7, [sp, #0xB0]\n\t" \
            "adrp x0, " #cname "_tracename\n\t" \
            "add x0, x0, :lo12:" #cname "_tracename\n\t" \
            "bl xblr_hle_trace\n\t" \
            "mov x0, sp\n\t" \
            "sub x0, x0, #0x100\n\t" \
            "mov w1, #0x10000\n\t" \
            "bl xblr_hle_scrub\n\t" \
            "ldp q6, q7, [sp, #0xB0]\n\t" \
            "ldp q4, q5, [sp, #0x90]\n\t" \
            "ldp q2, q3, [sp, #0x70]\n\t" \
            "ldp q0, q1, [sp, #0x50]\n\t" \
            "ldp x8, x30, [sp, #0x40]\n\t" \
            "ldp x6, x7, [sp, #0x30]\n\t" \
            "ldp x4, x5, [sp, #0x20]\n\t" \
            "ldp x2, x3, [sp, #0x10]\n\t" \
            "ldp x0, x1, [sp, #0x00]\n\t" \
            "add sp, sp, #0xE0\n\t" \
            "b " #cname "\n\t"); \
    }
void xblr_hle_trace(const char *name);
void xblr_hle_note(const char *fmt, ...) __attribute__((format(printf,1,2)));
#define HLE(mangled,ret,cname,...) \
    static ret cname(__VA_ARGS__) __attribute__((used)); \
    HLE_TRAMPOLINE(cname,mangled) \
    __attribute__((used,section("xblr_hle"),aligned(8))) \
    static const xblr_hle_binding xblr_hle_##cname={mangled,(uintptr_t)cname##_tramp,0}; \
    static ret cname(__VA_ARGS__)
#define HLE_FORCE(mangled,why,ret,cname,...) \
    static ret cname(__VA_ARGS__) __attribute__((used)); \
    HLE_TRAMPOLINE(cname,mangled) \
    __attribute__((used,section("xblr_hle"),aligned(8))) \
    static const xblr_hle_binding xblr_hle_##cname={mangled,(uintptr_t)cname##_tramp,1}; \
    static ret cname(__VA_ARGS__)
#endif
