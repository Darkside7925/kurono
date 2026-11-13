# Turn a wallpaper PNG into Kurono's pre-decoded RGBA header (src/ui/wallpaper*.h).
# usage: python3 make_header.py in.png out.h <symbol prefix: wallpaper | wallpaper2> <name>
import sys
from PIL import Image
src, out, sym, name = sys.argv[1:5]
im = Image.open(src).convert('RGBA')
w, h = im.size
data = im.tobytes()
with open(out, 'w', newline='\n') as f:
    f.write(f"// auto-generated raw rgba wallpaper \"{name}\": original, rendered by tools/wallpapers/gen.py\n")
    f.write("// pre-decoded on host to bypass the freestanding image decoder. (satoru)\n")
    f.write("#pragma once\n#include <stdint.h>\n\n")
    f.write(f"extern const unsigned int {sym}_rgba_w;\nextern const unsigned int {sym}_rgba_h;\n")
    f.write("// one definition for the whole kernel; the headers only declare it, so\n")
    f.write("// including this from several files no longer makes a 2 mb copy each. (satoru)\n")
    f.write(f"extern const unsigned char {sym}_rgba_data[];\n#if defined(KURONO_WP_IMPL)\n")
    f.write(f"const unsigned int {sym}_rgba_w = {w};\nconst unsigned int {sym}_rgba_h = {h};\n")
    f.write(f"const unsigned char {sym}_rgba_data[] = {{\n")
    for i in range(0, len(data), 20):
        f.write("    " + ",".join(str(b) for b in data[i:i+20]) + ",\n")
    f.write("};\n#else\n")
    f.write(f"extern const unsigned char {sym}_rgba_data[];\n#endif\n")
print(out, w, h)
