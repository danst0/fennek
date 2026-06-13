import sys, math
from PIL import Image, ImageDraw, ImageFont, ImageFilter
sys.path.insert(0, "tools")
from sleepimg import parse_array, to_image, from_image, write_header, W, H

FONT = "/usr/share/fonts/urw-base35/Z003-MediumItalic.otf"
TEXT = "by Dr. Daniel Dumke"

def render(xc, y0, k, size, preview_path, ss=4, write=False):
    fox = to_image(parse_array("src/core/sleep_img.h")).convert("L")
    Wp, Hp = W*ss, H*ss
    mask = Image.new("L", (Wp, Hp), 0)
    md = ImageDraw.Draw(mask)
    font = ImageFont.truetype(FONT, size*ss)
    widths = [md.textlength(ch, font=font) for ch in TEXT]
    total = sum(widths)
    x = xc*ss - total/2
    for ch, wch in zip(TEXT, widths):
        xcen = x + wch/2
        dx = (xcen - xc*ss)
        y = y0*ss + k*dx*dx/ss        # Kurve (Parabel); k skaliert
        slope = 2*k*dx/ss             # dy/dx
        ang = math.degrees(math.atan(slope))
        box = size*ss*2
        gi = Image.new("L", (box, box), 0)
        gd = ImageDraw.Draw(gi)
        gd.text((box/2, box/2), ch, fill=255, font=font, anchor="mm")
        gi = gi.rotate(-ang, resample=Image.BICUBIC, center=(box/2, box/2))
        mask.paste(gi, (int(xcen-box/2), int(y-box/2)), gi)
        x += wch

    halo = mask.filter(ImageFilter.MaxFilter(3*ss+1))
    mask_s = mask.resize((W, H), Image.LANCZOS)
    halo_s = halo.resize((W, H), Image.LANCZOS)
    out = fox.copy(); op = out.load(); hp = halo_s.load(); mp = mask_s.load()
    for y_ in range(H):
        for x_ in range(W):
            if hp[x_,y_] > 35: op[x_,y_] = 255
    for y_ in range(H):
        for x_ in range(W):
            if mp[x_,y_] > 95: op[x_,y_] = 0
    out = out.point(lambda v: 255 if v>=128 else 0).convert("1").convert("L")
    out.resize((W*2,H*2), Image.NEAREST).save(preview_path)
    if write:
        write_header(from_image(out), "src/core/sleep_img.h")
        print("sleep_img.h geschrieben")
    return out

if __name__ == "__main__":
    # xc y0 k size [write]
    a = sys.argv[1:]
    xc=float(a[0]); y0=float(a[1]); k=float(a[2]); size=int(a[3])
    write = (len(a)>4 and a[4]=="write")
    render(xc,y0,k,size,"/tmp/fennek_credit_2x.png", write=write)
    print("preview /tmp/fennek_credit_2x.png", xc,y0,k,size,"write=",write)
