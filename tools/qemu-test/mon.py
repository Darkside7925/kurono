import socket, time, sys
for c in sys.argv[1:]:
    s=socket.socket(socket.AF_UNIX); s.connect('/tmp/kt/monff.sock'); s.settimeout(1); time.sleep(0.15)
    try: s.recv(65536)
    except Exception: pass
    s.send((c+"\n").encode()); time.sleep(0.4); s.close()
