import socket, time, re, subprocess, collections
def mon(c, wait=1.0):
    s=socket.socket(socket.AF_UNIX); s.connect('/tmp/kt/monff.sock'); s.settimeout(2); time.sleep(0.2)
    try: s.recv(65536)
    except Exception: pass
    s.send((c+"\n").encode()); time.sleep(wait); out=b''
    try:
        while True:
            d=s.recv(65536)
            if not d: break
            out+=d
    except Exception: pass
    s.close(); return out.decode(errors='ignore')
cnt=collections.Counter()
for k in range(6):
    r=mon("info registers")
    rsp=re.search(r'RSP=([0-9a-f]+)',r).group(1); rip=re.search(r'RIP=([0-9a-f]+)',r).group(1)
    cs=re.search(r'CS =([0-9a-f]+)',r).group(1)
    if cs!='0008': cnt[('user',)]+=1; continue
    o=mon(f"x/96gx 0x{rsp}",1.5)
    vals=[int(v,16) for v in re.findall(r'0x([0-9a-f]{16})',o)]
    code=[hex(v) for v in vals if 0x100000<=v<0x800000]
    syms=subprocess.run(['addr2line','-f','-C','-e','/tmp/kt/isox/kurono_base.elf',rip]+code[:25],capture_output=True,text=True).stdout.split('\n')[0::2]
    cnt[tuple(syms[:10])]+=1
for k,v in cnt.items(): print(v, ' <- '.join(k)); print()
