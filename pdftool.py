"""
Converts PDF documents to bitmaps and generates a C header file
that can be rendered on Waveshare E-paper displays.
Usage: python -m pdftool [document] [first page] [last page] [crop page]
       document: path to PDF document
       first page: start conversion at this page
       last page: stop conversion at this page (inclusive)
       crop page: an optional page number to use when determining area to crop. If not specified
                  each page is croppped to minimize white borders.
"""

import os
import re
import sys
import subprocess

pdf = sys.argv[1]
p1 = int(sys.argv[2])
pn = int(sys.argv[3])

# if 4th argument is supplied, crop based on that page. Otherwise maximize per page.
p0 = 0
crop_area = None
if len(sys.argv) == 5:
    p0 = int(sys.argv[4])
    p0_name = "test.jpg"
    subprocess.run(["pdftoppm", "-jpeg", "-r", "300", "-thinlinemode", "solid", "-f", str(p0), "-singlefile", pdf, "test"])
    rv = subprocess.run(
        ["magick", p0_name, "-trim", "-format", "%[fx:w]x%[fx:h]+%[fx:page.x]+%[fx:page.y]", "info:"],
        capture_output=True,
        text=True
    )
    crop_area = rv.stdout
    os.remove(p0_name)

root = "doc"
jpg = root + ".jpg"
h = root + ".h"
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
        if crop_area is not None:
            subprocess.run(["magick", jpg, "-crop", crop_area, jpg])
        else:
            subprocess.run(["mogrify", "-trim", jpg]) 
        subprocess.run(["convert", jpg, "-resize", "480x800!", jpg]) 
        subprocess.run(["convert", jpg, "-threshold", "80%", jpg]) 
        subprocess.run(["mogrify", "-rotate", "-90", jpg])
        subprocess.run(["convert", jpg, "-depth", "1", "-format", "'txt'", txt])

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
os.remove(jpg)
