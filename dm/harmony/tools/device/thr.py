import re,sys
# 对比两次 /proc/<pid>/task/* 采样，按线程打印 CPU 占用。
# Per-thread CPU between two /proc task snapshots.
# Usage: thr.py <a.txt> <b.txt> [seconds-between]
a_path,b_path=sys.argv[1],sys.argv[2]
D=float(sys.argv[3]) if len(sys.argv)>3 else 5.05
def rd(p):
    d={}
    for l in open(p):
        m=re.match(r'(\d+) \((.*?)\) (.*)',l)
        if not m: continue
        f=m[3].split(); d[m[1]]=(m[2],int(f[11]),int(f[12]))
    return d
a=rd(a_path);b=rd(b_path)
r=sorted(((b[t][1]-a[t][1]+b[t][2]-a[t][2])/D,(b[t][1]-a[t][1])/D,b[t][0]) for t in a if t in b)[::-1]
for x in r[:12]: print('%3d%% user %3d%% %s'%x)
