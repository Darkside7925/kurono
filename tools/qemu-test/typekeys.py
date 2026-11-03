import socket, time, sys
def mon(c):
    s=socket.socket(socket.AF_UNIX); s.connect('/tmp/kt/monff.sock'); s.settimeout(1); time.sleep(0.15)
    try: s.recv(65536)
    except Exception: pass
    s.send((c+"\n").encode()); time.sleep(0.25); s.close()
m={' ':'spc','.':'dot','/':'slash',':':'shift-semicolon','-':'minus'}
for a in sys.argv[1:]:
    if a.startswith('@'): mon("sendkey "+a[1:]); time.sleep(0.6); continue
    for ch in a:
        mon("sendkey "+m.get(ch, ch)); time.sleep(0.35)
