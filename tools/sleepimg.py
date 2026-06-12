import re, sys
from PIL import Image

W, H = 240, 320

def parse_array(path):
    txt = open(path).read()
    # nur den Block zwischen { und } nehmen
    blk = txt[txt.index('{')+1: txt.rindex('}')]
    vals = re.findall(r'0x[0-9a-fA-F]+', blk)
    return [int(v,16) for v in vals]

def to_image(bytes_):
    img = Image.new('1', (W,H), 1)  # 1=weiß
    px = img.load()
    stride = (W+7)//8
    for y in range(H):
        for x in range(W):
            byte = bytes_[y*stride + (x>>3)]
            bit = (byte >> (7-(x&7))) & 1  # MSB-first
            px[x,y] = 0 if bit else 1      # gesetztes Bit = schwarz
    return img

def from_image(img):
    img = img.convert('1')
    px = img.load()
    stride=(W+7)//8
    out=[0]*(stride*H)
    for y in range(H):
        for x in range(W):
            black = (px[x,y]==0)
            if black:
                out[y*stride+(x>>3)] |= (1<<(7-(x&7)))
    return out

def write_header(bytes_, path):
    stride=(W+7)//8
    lines=["// sleep_img.h — Standby-Bild: schlafender Fennek (1-Bit, Floyd-Steinberg).",
           "// 240x320, MSB-first, gesetztes Bit = schwarzes Pixel.",
           "#pragma once","#include <stdint.h>","#include <pgmspace.h>","",
           "constexpr int kSleepImgW = 240;","constexpr int kSleepImgH = 320;",
           f"const uint8_t kSleepImg[{stride*H}] PROGMEM = {{"]
    for i in range(0,len(bytes_),16):
        chunk=bytes_[i:i+16]
        lines.append("  "+", ".join(f"0x{b:02x}" for b in chunk)+",")
    lines.append("};")
    open(path,"w").write("\n".join(lines)+"\n")

if __name__=="__main__":
    cmd=sys.argv[1]
    if cmd=="export":
        b=parse_array("src/core/sleep_img.h")
        to_image(b).save(sys.argv[2])
        print("geschrieben:", sys.argv[2], "bytes:", len(b))
    elif cmd=="import":
        img=Image.open(sys.argv[2])
        write_header(from_image(img), "src/core/sleep_img.h")
        print("sleep_img.h aktualisiert aus", sys.argv[2])
