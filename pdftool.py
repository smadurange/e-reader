import os
import re
import sys
import subprocess

pdf = sys.argv[1]
p1 = int(sys.argv[2])
pn = int(sys.argv[3])

root = pdf.replace("." + pdf.split('.')[-1], "")
jpg = root + ".jpg"
h = root + ".h"
mon = "mon.jpg"
res = "res.jpg"
txt = root + ".txt"
macro = root.upper() + "_H"
data_len = pn - p1 + 1

with open(h, "w") as f:
    f.write("#ifndef {}\n".format(macro))
    f.write("#define {}\n\n".format(macro))
    f.write("const size_t data_len = {};\n\n".format(data_len))
    f.write("const unsigned char data[{0}][48000] = {{\n".format(data_len))
    for p in range(p1, pn + 1):
        print("Processing page {}/{}...".format(p, pn))
        subprocess.run(["pdftoppm", "-jpeg", "-r", "300", "-thinlinemode", "solid", "-f", str(p), "-singlefile", pdf, root])
        subprocess.run(["mogrify", "-trim", "-rotate", "-90", jpg])
        subprocess.run(["convert", jpg, "-monochrome", mon]) 
        subprocess.run(["convert", mon, "-resize", "800x480!", res]) 
        subprocess.run(["convert", res, "-depth", "1", "-format", "'txt'", txt])

        f.write("\t{\n\t\t")
        total = 0
        with open(txt, "r") as fd:
            n = 7
            x = 0xFF
            count = 0
            fd.readline()
            for line in fd:
                px = re.search("\([^\)]+\)", line).group()
                if px == "(0)":                        
                    x &= ~(1 << n)
                n -= 1
                if n < 0:
                    f.write("0x{:02X}, ".format(x))
                    count += 1
                    total += 1
                    if count >= 12:
                        f.write("\n\t\t")
                        count = 0
                    x = 0xFF
                    n = 7
        f.write("\n\t},\n")
    f.write("};\n\n")    
    f.write("#endif /* {} */".format(macro))    

size = os.path.getsize(h)
if size < 1024:
    print("Done! Wrote {:0.1f}B to {}".format(size, h))
elif size < pow(1024,2):
    print("Done! Wrote {:0.1f}KB to {}".format(round(size / 1024, 2), h))
elif size < pow(1024,3):
    print("Done! Wrote {:0.1f}MB to {}".format(round(size / (pow(1024, 2)), 2), h))
elif size < pow(1024,4):
    print("Done! Wrote {:0.1f}GB to {}".format(round(size / (pow(1024, 3)), 2), h))

os.remove(txt)
os.remove(res)
os.remove(mon)
os.remove(jpg)
