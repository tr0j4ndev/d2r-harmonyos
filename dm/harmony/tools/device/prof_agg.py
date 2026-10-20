import sys, re, subprocess, collections, os, bisect, shutil
# cpu_sampler（app 内 files/cpu-sample 触发，输出 cpu-samples.txt）的聚合分析。
# Aggregation for the in-app CPU sampler (trigger: files/cpu-sample, output: cpu-samples.txt).
#
# Usage: prof_agg.py <samples.txt> <mode> [thread] [arg]
#   mode=summary [thread]        每线程 self/inclusive 汇总（缺省全部线程） | per-thread self/inclusive
#   mode=attr <thread>           归到第一个宿主/运行时调用方 | attribute to first host/runtime caller
#   mode=chain <thread> [depth]  叶函数 + 宿主内联链 | leaf + host inline chain
#   mode=proj <thread> [all|alloc|driver]  归到第一个项目源码帧 | first project-source frame
#
# Env:
#   XBL_PROF_DIR     含去符号 libxblhost.so / libxblr_execution.so / musl.syms 的目录 | dir with unstripped libs
#   LLVM_SYMBOLIZER  llvm-symbolizer 路径（缺省 PATH 或 $OHOS_SDK/native/llvm/bin） | symbolizer path
SYM = os.environ.get('LLVM_SYMBOLIZER') or shutil.which('llvm-symbolizer') or \
    os.path.join(os.environ.get('OHOS_SDK', ''), 'native/llvm/bin/llvm-symbolizer')
PROF = os.environ.get('XBL_PROF_DIR', 'prof')
LOCAL = {'libxblhost.so': os.path.join(PROF, 'libxblhost.so'),
         'libxblr_execution.so': os.path.join(PROF, 'libxblr_execution.so')}
MAIN_TEXT = (0x2ef000, 0x2ef000 + 28804768)   # 客体 main 映像在 libxblr_execution.so 内的范围 | guest main image inside libxblr_execution.so

path = sys.argv[1]
mode = sys.argv[2] if len(sys.argv) > 2 else 'summary'
focus = sys.argv[3] if len(sys.argv) > 3 else None
arg = sys.argv[4] if len(sys.argv) > 4 else None

names, samples, maps = {}, [], []
M = (1 << 48) - 1
for line in open(path, errors='replace'):
    if line.startswith('T '):
        _, tid, name = line.rstrip('\n').split(' ', 2); names[int(tid)] = name
    elif line.startswith('S '):
        p = line.split(); samples.append((int(p[1]), [int(x, 16) & M for x in p[2:]]))
    elif line.startswith('M '):
        m = re.match(r'M ([0-9a-f]+)-([0-9a-f]+) (\S+) ([0-9a-f]+) \S+ \d+\s*(.*)', line)
        if m: maps.append((int(m.group(1), 16), int(m.group(2), 16), int(m.group(4), 16), m.group(5).strip()))
maps.sort()
base = {}
for s, e, off, p in maps:
    if p and off == 0 and p not in base: base[p] = s
starts = [m[0] for m in maps]
def where(a):
    i = bisect.bisect_right(starts, a) - 1
    if i < 0 or a >= maps[i][1]: return ('?', a)
    p = maps[i][3]; b = base.get(p, maps[i][0] - maps[i][2])
    return (os.path.basename(p) or p or 'anon', a - b)
cache = {}
def sym(mod, off):
    if mod == 'libxblr_execution.so' and MAIN_TEXT[0] <= off < MAIN_TEXT[1]:
        return 'guest main+%#x' % ((off - MAIN_TEXT[0]) & ~0xff)
    if mod not in LOCAL: return mod
    return cache.get((mod, off), '%s+%#x' % (mod, off))
def symbolize(inlines=False):
    need = collections.defaultdict(set)
    for tid, st in samples:
        for a in st[:1] + st[1:]:
            mod, off = where(a)
            if mod in LOCAL: need[mod].add(off)
    for mod, offs in need.items():
        if not os.path.exists(LOCAL[mod]): continue
        args = [SYM, '--obj=' + LOCAL[mod], '--functions=short', '--demangle',
                '--inlines' if inlines else '--no-inlines']
        out = subprocess.run(args, input='\n'.join(hex(o) for o in sorted(offs)),
                             capture_output=True, text=True).stdout
        blocks = [b for b in out.split('\n\n') if b.strip()]
        for o, b in zip(sorted(offs), blocks):
            fn = b.strip().split('\n')[0]
            if fn != '??': cache[(mod, o)] = fn[:90]

def musl_syms():
    tab = []
    f = os.path.join(PROF, 'musl.syms')
    if os.path.exists(f):
        for l in open(f):
            p = l.split()
            if len(p) >= 2: tab.append((int(p[0], 16), p[-1]))
    tab.sort()
    addrs = [m[0] for m in tab]
    def find(off):
        i = bisect.bisect_right(addrs, off) - 1
        return tab[i][1] if i >= 0 else '?'
    return find

def tids():
    return [t for t, n in names.items() if n == focus or str(t) == focus]

if mode == 'summary':
    symbolize()
    for tid, n in collections.Counter(t for t, _ in samples).most_common():
        if focus and names.get(tid, '') != focus and str(tid) != focus: continue
        print(f'=== {names.get(tid, tid)} ({tid}) samples={n}')
        selfc, modc, incl = collections.Counter(), collections.Counter(), collections.Counter()
        for t, st in samples:
            if t != tid: continue
            mod, off = where(st[0]); modc[mod] += 1; selfc[sym(mod, off)] += 1
            seen = set()
            for a in [st[0]] + st[2:]:
                m2, o2 = where(a); f = sym(m2, o2)
                if m2 in LOCAL and f not in seen: seen.add(f); incl[f] += 1
        print('  modules:', ', '.join(f'{m} {c*100/n:.0f}%' for m, c in modc.most_common(6)))
        print('  self:'); [print(f'    {c*100/n:5.1f}% {f}') for f, c in selfc.most_common(int(os.environ.get('TOP', 18)))]
        print('  inclusive (host/runtime frames):'); [print(f'    {c*100/n:5.1f}% {f}') for f, c in incl.most_common(int(os.environ.get('TOPI', 22)))]

elif mode == 'attr':
    symbolize()
    ts = set(tids()); c = collections.Counter(); n = 0
    for t, st in samples:
        if t not in ts: continue
        n += 1
        mod, off = where(st[0])
        if mod in LOCAL: continue
        for a in st[1:]:
            m2, o2 = where(a)
            if m2 in LOCAL: c[mod + ' <- ' + sym(m2, o2)] += 1; break
        else: c[mod + ' <- ?'] += 1
    for k, v in c.most_common(25): print('%5.1f%% %s' % (v * 100 / n, k))

elif mode == 'chain':
    depth = int(arg) if arg else 3
    msym = musl_syms()
    ts = set(tids()); rows = []; need = set()
    for t, st in samples:
        if t not in ts: continue
        leaf = where(st[0]); host = None
        for a in st:
            m, o = where(a)
            if m == 'libxblhost.so': host = o if a == st[0] else o - 4; break
        rows.append((leaf, host))
        if host is not None: need.add(host)
    chain = {}
    if need and os.path.exists(LOCAL['libxblhost.so']):
        offs = sorted(need)
        out = subprocess.run([SYM, '--obj=' + LOCAL['libxblhost.so'], '--functions=short', '--inlines', '--demangle'],
                             input='\n'.join(hex(o) for o in offs), capture_output=True, text=True).stdout
        blocks = [b for b in out.split('\n\n') if b.strip()]
        for o, b in zip(offs, blocks):
            ls = b.strip().split('\n'); fns = [ls[i][:50] for i in range(0, len(ls), 2)]
            lines = [ls[i].rsplit('/', 1)[-1] for i in range(1, len(ls), 2)]
            chain[o] = ' < '.join('%s@%s' % (f, l) for f, l in list(zip(fns, lines))[:depth])
    n = len(rows); c1, c2 = collections.Counter(), collections.Counter()
    for (lm, lo), h in rows:
        leafname = lm if lm != 'ld-musl-aarch64.so.1' else 'musl:' + msym(lo)
        if lm == 'libxblhost.so': leafname = 'host'
        c1[leafname] += 1
        c2[(leafname if leafname != 'host' else '') + ' | ' + (chain.get(h, '?') if h is not None else '-')] += 1
    print('samples', n)
    for k, v in c1.most_common(25): print('%5.1f%% %s' % (v * 100 / n, k))
    print('---')
    for k, v in c2.most_common(60): print('%5.1f%% %s' % (v * 100 / n, k))

elif mode == 'proj':
    sub = arg or 'all'
    ts = set(tids()); need = set(); rows = []
    for t, st in samples:
        if t not in ts: continue
        hs = []
        for k, a in enumerate(st):
            m, o = where(a)
            if m == 'libxblhost.so': hs.append(o if k == 0 else o - 4)
        rows.append((where(st[0]), hs)); need.update(hs)
    first = {}; isalloc = {}
    if need and os.path.exists(LOCAL['libxblhost.so']):
        offs = sorted(need)
        out = subprocess.run([SYM, '--obj=' + LOCAL['libxblhost.so'], '--functions=short', '--inlines', '--demangle'],
                             input='\n'.join(hex(o) for o in offs), capture_output=True, text=True).stdout
        blocks = [b for b in out.split('\n\n') if b.strip()]
        proj = re.compile(r'(xbg_[a-z_]+\.(cpp|h)|game_graphics\.cpp):(\d+)')
        for o, b in zip(offs, blocks):
            ls = b.strip().split('\n')
            pairs = [(ls[i], ls[i + 1] if i + 1 < len(ls) else '') for i in range(0, len(ls), 2)]
            isalloc[o] = any(('operator_new' in f or 'operator_delete' in f or 'allocate' in f) for f, _ in pairs)
            for f, l in pairs:
                m = proj.search(l)
                if m: first[o] = '%s %s:%s' % (f[:40], m.group(1), m.group(3)); break
    c = collections.Counter(); n = 0
    for (lm, lo), hs in rows:
        if sub == 'alloc' and not (lm == 'ld-musl-aarch64.so.1' and any(isalloc.get(h) for h in hs[:2])): continue
        if sub == 'driver' and lm != 'libmaleoon_v300.so': continue
        n += 1
        c[next((first[h] for h in hs if h in first), '?')] += 1
    tot = len(rows)
    print('samples', n, 'of', tot)
    for k, v in c.most_common(45): print('%5.1f%% %s' % (v * 100 / tot, k))

else:
    sys.exit(__doc__)
