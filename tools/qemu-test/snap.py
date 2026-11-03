import socket, time, sys
from PIL import Image
out = sys.argv[1]
s=socket.socket(socket.AF_UNIX); s.connect('/tmp/kt/monff.sock'); s.settimeout(3); time.sleep(0.3)
try: s.recv(65536)
except Exception: pass
s.send(b"screendump /tmp/kt/snap.ppm\n"); time.sleep(2.5); s.close()
Image.open('/tmp/kt/snap.ppm').convert('RGB').resize((960,540)).save(out)
