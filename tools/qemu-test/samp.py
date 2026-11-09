import socket, time, re, collections
cnt=collections.Counter()
for i in range(15):
    s=socket.socket(socket.AF_UNIX); s.connect('/tmp/kt/monff.sock'); s.settimeout(2); time.sleep(0.2)
    try: s.recv(65536)
    except Exception: pass
    s.send(b"info registers -a\n"); time.sleep(1.0); out=b''
    try:
        while True:
            d=s.recv(65536)
            if not d: break
            out+=d
    except Exception: pass
    s.close()
    o=out.decode(errors='ignore')
    for blk in o.split('CPU#')[1:]:
        n=blk.split()[0]
        rip=re.search(r'RIP=([0-9a-f]+)',blk).group(1)
        hlt='HLT=1' in blk
        cs=re.search(r'CS =([0-9a-f]+)',blk).group(1)
        cnt[(n, 'halt' if hlt else rip, cs)]+=1
for k,v in sorted(cnt.items()): print(k,v)
