"""
Converts PDFs to bitmaps for e-paper displays using Poppler and ImageMagick.

Usage: python -m doctoebm -o [path] [document]
       o: output directory
       document: path to PDF
"""
import io
import os
import re
import sys
import getopt
import subprocess
from pathlib import Path

dpi = "300"
screen_size = "480x800!"

doc = sys.argv[-1]
argv = sys.argv[1:-1]
opts, args = getopt.getopt(argv, "o:")

output_path = "."
for o, a in opts:
    if o == "-o":
        output_path = a.rstrip("/")
        if not os.path.exists(output_path):
            os.makedirs(output_path)

root = output_path + "/doc"
print("Converting PDF to images...")
subprocess.run(["pdftoppm", "-png", "-progress", "-r", dpi, "-thinlinemode", "solid", doc, root])
print("Finished converting PDF to images.")

paths = sorted(list(Path(output_path).glob('*.png')))

print("Determining page size...")
w = h = 0

for p in paths:
    rv = subprocess.run(
            ["magick", p, "-trim", "-format", "%[fx:w] %[fx:h] %[fx:page.x] %[fx:page.y]", "info:"],
            capture_output=True,
            text=True
         )
    info = [int(x) for x in rv.stdout.split()]
    if w * h < info[0] * info[1]:
        w = info[0]
        h = info[1]
        dx = info[2]
        dy = info[3]

crop = "{}x{}+{}+{}".format(w, h, dx, dy)
print("Crop: {}".format(crop))

ebm = "a.ebm"

with open(ebm, "ab") as dst:
    for i, p in enumerate(paths):
        print("Processing page {}/{}...".format(i+1, len(paths)))
        png = str(p)
        txt = re.sub("-0+", "-", png, count=1).replace(".png", ".txt")
    
        subprocess.run(["magick", png, "-crop", crop, png])
        subprocess.run(["convert", png, "-resize", screen_size, png]) 
        subprocess.run(["convert", png, "-threshold", "80%", png]) 
        subprocess.run(["mogrify", "-rotate", "-90", png])
        subprocess.run(["convert", png, "-depth", "1", "-format", "'txt'", txt])
    
        with open(txt, "r") as src:
            total = 0
            n = 7
            x = 0
            src.readline()
            for line in src:
                px = re.search("\([^\)]+\)", line).group()
                if px == "(0)":                        
                    x |= (1 << n)
                n -= 1
                if n < 0:
                    dst.write(x.to_bytes(1))
                    total += 1
                    n = 7
                    x = 0
        os.remove(txt)
        os.remove(png)

