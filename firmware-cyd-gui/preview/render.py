#!/usr/bin/env python3
"""Convert out/*.rgb565 (240x320) to PNG, upscaled 2x, plus a contact sheet."""
import sys, glob, os
import numpy as np
from PIL import Image
os.chdir(os.path.dirname(os.path.abspath(__file__)))
names = sys.argv[1:] or sorted(os.path.basename(p)[:-7] for p in glob.glob('out/*.rgb565'))
ims = []
for n in names:
    a = np.fromfile(f'out/{n}.rgb565', dtype='<u2').reshape(320, 240).astype(np.uint32)
    r = ((a >> 11) & 31) * 255 // 31; g = ((a >> 5) & 63) * 255 // 63; b = (a & 31) * 255 // 31
    im = Image.fromarray(np.dstack([r, g, b]).astype(np.uint8))
    im.resize((480, 640), Image.NEAREST).save(f'out/{n}.png'); ims.append(im)
if len(ims) > 1:
    sheet = Image.new('RGB', (244 * len(ims), 320), (30, 30, 30))
    for i, im in enumerate(ims): sheet.paste(im, (i * 244, 0))
    sheet.save('out/sheet.png')
print('ok', names)
