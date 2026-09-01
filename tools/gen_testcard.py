from PIL import Image, ImageDraw, ImageFont
W, H = 1920, 1080
img = Image.new("RGB", (W, H), (28, 28, 32))
d = ImageDraw.Draw(img)

def font(sz):
    for p in ("/System/Library/Fonts/Supplemental/Arial Bold.ttf",
              "/System/Library/Fonts/Supplemental/Arial.ttf",
              "/System/Library/Fonts/Helvetica.ttc"):
        try: return ImageFont.truetype(p, sz)
        except Exception: pass
    return ImageFont.load_default()

# 10% 网格
for i in range(1, 10):
    d.line([(W*i//10, 0), (W*i//10, H)], fill=(70, 70, 78), width=2)
    d.line([(0, H*i//10), (W, H*i//10)], fill=(70, 70, 78), width=2)

# ⭐ 关键判据线：屏幕 1.6:1 / 视频 1.7778:1，居中填满时左右应各裁掉正好 5%
d.rectangle([93, 0, 99, H], fill=(0, 230, 255))          # x=5%
d.rectangle([W-99, 0, W-93, H], fill=(0, 230, 255))      # x=95%
d.rectangle([0, 51, W, 57], fill=(255, 220, 0))          # y=5%
d.rectangle([0, H-57, W, H-51], fill=(255, 220, 0))      # y=95%

# 最外圈红框：看得见 = 这条边没被裁掉
d.rectangle([0, 0, W-1, H-1], outline=(255, 40, 40), width=10)

# 四角色块
for (x, y, c) in [(10,10,(0,255,80)), (W-140,10,(0,140,255)),
                  (10,H-140,(255,0,220)), (W-140,H-140,(255,255,255))]:
    d.rectangle([x, y, x+130, y+130], fill=c)

# 边缘每 2.5% 一个刻度，被裁多少可以直接数
for i in range(41):
    x = W*i//40
    long = (i % 4 == 0)
    d.rectangle([x-2, 0, x+2, 40 if long else 22], fill=(255,255,255) if long else (150,150,150))
    d.rectangle([x-2, H-(40 if long else 22), x+2, H], fill=(255,255,255) if long else (150,150,150))
for i in range(41):
    y = H*i//40
    long = (i % 4 == 0)
    d.rectangle([0, y-2, 40 if long else 22, y+2], fill=(255,255,255) if long else (150,150,150))
    d.rectangle([W-(40 if long else 22), y-2, W, y+2], fill=(255,255,255) if long else (150,150,150))

f52, f44, f64, f36 = font(52), font(44), font(64), font(36)
def txt(x, y, s, fnt, c, anchor="mm"):
    d.text((x, y), s, font=fnt, fill=c, anchor=anchor)

txt(W//2, 30,     "TOP EDGE  y=0%",    f44, (255,255,255))
txt(W//2, H-30,   "BOTTOM EDGE  y=100%", f44, (255,255,255))
txt(170, 300,     "x=5%",  f52, (0,230,255))
txt(W-170, 300,   "x=95%", f52, (0,230,255))
txt(480, 100,     "y=5%",  f44, (255,220,0))
txt(480, H-100,   "y=95%", f44, (255,220,0))
txt(W//2, H//2-90, "CENTER", f64, (255,255,255))
txt(W//2, H//2-20, "source 1920x1080  (16:9)", f36, (200,200,200))
txt(W//2, H//2+40, "fill+center on 1.6:1 screen", f36, (200,200,200))
txt(W//2, H//2+90, "=> cut exactly 5% left & right", f36, (255,180,80))
# 每 10% 标数字，方便直接读出边缘落在哪
for i in range(1, 10):
    txt(W*i//10, 78, f"{i*10}%", f36, (190,190,190))
    txt(72, H*i//10, f"{i*10}%", f36, (190,190,190))

img.save("testcard.png")
print("testcard.png saved")
