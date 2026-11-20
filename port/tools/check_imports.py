# Dev helper: verify every undefined import of the APK's libraries is provided.
import re, subprocess, sys
RE = sys.argv[1]
src = open('source/imports.c').read()
have = set(re.findall(r'\bD\((\w+)\)', src)) | set(re.findall(r'\bW\((\w+)\)', src)) | set(re.findall(r'R\("([^"]+)"', src))
def syms(lib, defined):
    out = subprocess.run([RE, '--dyn-syms', '-W', lib], capture_output=True, text=True).stdout
    res = set()
    for l in out.splitlines():
        p = l.split()
        if len(p) < 8: continue
        if (p[6] == 'UND') != (not defined): continue
        if p[6] == 'UND' and defined: continue
        res.add(p[7].split('@')[0])
    return res
cocos = sys.argv[2]; fmod = sys.argv[3]
fmod_def = syms(fmod, True)
for lib in (cocos, fmod):
    und = syms(lib, False) - {''}
    weak = set()
    out = subprocess.run([RE, '--dyn-syms', '-W', lib], capture_output=True, text=True).stdout
    for l in out.splitlines():
        p = l.split()
        if len(p) >= 8 and p[6] == 'UND' and p[4] == 'WEAK': weak.add(p[7].split('@')[0])
    missing = sorted(s for s in und if s not in have and s not in fmod_def and s not in weak)
    print(lib.split('/')[-1], 'undefined:', len(und), 'missing:', missing)
