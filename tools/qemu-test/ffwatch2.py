import socket, time, os, re
log, sock, outdir = '/tmp/kt/ff.log', '/tmp/kt/monff.sock', '/tmp/kt/ff'
t0 = time.time(); i = int(os.environ.get("START", "0"))
def dump(path):
    try:
        s=socket.socket(socket.AF_UNIX); s.connect(sock); s.settimeout(3); time.sleep(0.3)
        try: s.recv(65536)
        except: pass
        s.send(f"screendump {path}\n".encode()); time.sleep(2); s.close()
    except Exception as e: print("dump fail", e, flush=True)
while time.time() - t0 < 7000:
    time.sleep(300)
    dump(f"{outdir}/f{i:03d}.ppm"); i += 1
    try: txt = open(log, errors='ignore').read()
    except: txt = ''
    ff = [l for l in txt.splitlines() if re.search(r'firefox|kpkg|\[ff|gecko|xul|wayland|wl_|Fault|PANIC|#PF|#GP', l, re.I)]
    print(f"[{int(time.time()-t0)}s] shot {i-1} lines={len(txt.splitlines())} last: {ff[-1][:140] if ff else '-'}", flush=True)
