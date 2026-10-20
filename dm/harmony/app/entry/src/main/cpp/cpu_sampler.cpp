// In-process sampling profiler. The device kernel masks user-space addresses in hiperf samples,
// so busy threads are interrupted with SIGPROF and their pc / lr / frame-pointer chain recorded.
// Trigger: create files/cpu-sample (optional content: "<seconds> [g]", default 6; g also runs the gate profile). Output: files/cpu-samples.txt
// with "S tid pc lr fp0 fp1 ..." lines, thread names, and a copy of /proc/self/maps for offline symbolization.
#include <signal.h>
#include <ucontext.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <dlfcn.h>
#include <sched.h>
#include <time.h>
#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

namespace {
constexpr int kDepth=12;
struct Sample{int32_t tid;uint64_t pc,lr,frames[kDepth];};
constexpr size_t kCap=1<<17;
Sample *ring=nullptr;
std::atomic<size_t> used{0};
std::atomic<bool> running{false};

void on_prof(int,siginfo_t *,void *ctx){
    size_t i=used.fetch_add(1,std::memory_order_relaxed);if(i>=kCap)return;
    auto *uc=static_cast<ucontext_t*>(ctx);Sample &s=ring[i];
    s.tid=int32_t(syscall(SYS_gettid));s.pc=uc->uc_mcontext.pc;s.lr=uc->uc_mcontext.regs[30];
    uint64_t sp=uc->uc_mcontext.sp,fp=uc->uc_mcontext.regs[29];
    for(int k=0;k<kDepth;k++){
        // Only follow frame records that lie on this thread's stack just above sp; guest code may not keep a chain.
        if(fp<sp || fp-sp>(8u<<20) || (fp&15)){s.frames[k]=0;continue;}
        uint64_t rec[2];struct iovec local{rec,sizeof rec},remote{reinterpret_cast<void*>(fp),sizeof rec};
        if(syscall(SYS_process_vm_readv,getpid(),&local,1,&remote,1,0)!=long(sizeof rec)){for(int j=k;j<kDepth;j++)s.frames[j]=0;break;}
        s.frames[k]=rec[1];
        uint64_t next=rec[0];if(next<=fp){for(int j=k+1;j<kDepth;j++)s.frames[j]=0;break;}fp=next;
    }
}

struct Busy{int tid;std::string name;};
std::vector<int> task_ids(){std::vector<int> out;DIR *d=opendir("/proc/self/task");if(!d)return out;while(dirent *e=readdir(d)){int t=atoi(e->d_name);if(t>0)out.push_back(t);}closedir(d);return out;}
uint64_t task_ticks(int tid,std::string *name){
    char path[64];snprintf(path,sizeof path,"/proc/self/task/%d/stat",tid);FILE *f=fopen(path,"rb");if(!f)return 0;
    char buf[1024];size_t n=fread(buf,1,sizeof buf-1,f);fclose(f);buf[n]=0;
    char *l=strchr(buf,'('),*r=strrchr(buf,')');if(!l||!r)return 0;if(name)name->assign(l+1,r);
    unsigned long long ut=0,st=0;char *p=r+2;for(int field=3;field<14 && p;field++){p=strchr(p,' ');if(p)p++;}
    if(!p||sscanf(p,"%llu %llu",&ut,&st)!=2)return 0;return ut+st;
}

void run(std::string dir,int seconds,bool gates){
    if(gates)if(void *h=dlopen("libxblr_execution.so",RTLD_NOW|RTLD_NOLOAD)){if(auto fn=reinterpret_cast<void(*)(int)>(dlsym(h,"xblr_gate_profile")))fn(seconds);dlclose(h);}
    std::vector<int> all=task_ids();std::vector<uint64_t> t0(all.size());
    for(size_t i=0;i<all.size();i++)t0[i]=task_ticks(all[i],nullptr);
    usleep(500000);
    std::vector<Busy> busy;
    for(size_t i=0;i<all.size();i++){std::string name;uint64_t t=task_ticks(all[i],&name);if(t>=t0[i]+5)busy.push_back({all[i],name});}   // >=10% of a core over 0.5 s
    if(!ring)ring=static_cast<Sample*>(calloc(kCap,sizeof(Sample)));
    used=0;struct sigaction sa{},old{};sa.sa_sigaction=on_prof;sa.sa_flags=SA_SIGINFO|SA_RESTART;sigemptyset(&sa.sa_mask);sigaction(SIGPROF,&sa,&old);
    const pid_t pid=getpid();const int ticks=seconds*1000;
    for(int k=0;k<ticks && used.load()<kCap;k++){for(const Busy &b:busy)syscall(SYS_tgkill,pid,b.tid,SIGPROF);usleep(1000);}
    usleep(20000);signal(SIGPROF,SIG_IGN);
    const size_t n=std::min(used.load(),kCap);
    FILE *f=fopen((dir+"/cpu-samples.txt").c_str(),"wb");
    if(f){
        fprintf(f,"# seconds=%d samples=%zu\n",seconds,n);
        for(const Busy &b:busy)fprintf(f,"T %d %s\n",b.tid,b.name.c_str());
        for(size_t i=0;i<n;i++){const Sample &s=ring[i];fprintf(f,"S %d %llx %llx",s.tid,(unsigned long long)s.pc,(unsigned long long)s.lr);for(int k=0;k<kDepth && s.frames[k];k++)fprintf(f," %llx",(unsigned long long)s.frames[k]);fputc('\n',f);}
        FILE *m=fopen("/proc/self/maps","rb");if(m){char line[512];while(fgets(line,sizeof line,m))fprintf(f,"M %s",line);fclose(m);}
        fclose(f);
    }
    running=false;
}
}

// files/cpu-bench: time a fixed dependent-ALU loop pinned to each CPU (best of 15) -> files/cpu-bench.txt.
void bench(std::string dir){
    FILE *f=fopen((dir+"/cpu-bench.txt").c_str(),"wb");if(!f){running=false;return;}
    for(int cpu=0;cpu<CPU_SETSIZE && cpu<32;cpu++){
        cpu_set_t set;CPU_ZERO(&set);CPU_SET(cpu,&set);if(sched_setaffinity(0,sizeof set,&set))continue;
        uint64_t best=~0ull;volatile uint64_t sink=0;
        for(int rep=0;rep<15;rep++){timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);uint64_t x=rep+1;for(int i=0;i<2000000;i++)x=x*6364136223846793005ull+1442695040888963407ull;sink=x;clock_gettime(CLOCK_MONOTONIC,&b);
            uint64_t ns=uint64_t(b.tv_sec-a.tv_sec)*1000000000ull+uint64_t(b.tv_nsec-a.tv_nsec);if(ns<best)best=ns;usleep(2000);}
        fprintf(f,"cpu %d best_us=%llu\n",cpu,(unsigned long long)(best/1000));(void)sink;
    }
    fclose(f);running=false;
}
// files/cpu-pin: lines "<thread-name-prefix> <hex cpu mask>"; applied once to every matching thread -> files/cpu-pin.txt.
void pin(std::string dir){
    FILE *in=fopen((dir+"/cpu-pin").c_str(),"rb");if(!in)return;
    char prefix[64];unsigned long long mask;std::string report;
    while(fscanf(in,"%63s %llx",prefix,&mask)==2){
        cpu_set_t set;CPU_ZERO(&set);for(int c=0;c<64 && c<CPU_SETSIZE;c++)if(mask>>c&1)CPU_SET(c,&set);
        for(int tid:task_ids()){std::string name;task_ticks(tid,&name);if(name.compare(0,strlen(prefix),prefix))continue;
            int rc=sched_setaffinity(tid,sizeof set,&set);char line[160];snprintf(line,sizeof line,"%s tid=%d mask=%llx rc=%d\n",name.c_str(),tid,mask,rc);report+=line;}
    }
    fclose(in);unlink((dir+"/cpu-pin").c_str());
    if(FILE *o=fopen((dir+"/cpu-pin.txt").c_str(),"wb")){fputs(report.c_str(),o);fclose(o);}
}
extern "C" void XblCpuSamplerPoll(const char *files_dir){
    if(running.load())return;std::string dir(files_dir),marker=dir+"/cpu-sample";
    pin(dir);
    if(access((dir+"/cpu-bench").c_str(),F_OK)==0){unlink((dir+"/cpu-bench").c_str());running=true;std::thread(bench,dir).detach();return;}
    FILE *f=fopen(marker.c_str(),"rb");if(!f)return;int seconds=6;char g=0;if(fscanf(f,"%d %c",&seconds,&g)<1)seconds=6;fclose(f);unlink(marker.c_str());
    if(seconds<1||seconds>30)seconds=6;running=true;std::thread(run,dir,seconds,g=='g').detach();
}
