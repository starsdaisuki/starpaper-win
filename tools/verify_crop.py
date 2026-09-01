import sys
from PIL import Image

# 用法: verify.py <jpg> <sl> <st> <sr> <sb> [dl dt dr db]
#   src 千分比（程序状态里 src=... 那一串），dst 像素矩形（缺省=整屏）
path = sys.argv[1]
sl, st, sr, sb = [float(v)/1000 for v in sys.argv[2:6]]
im = Image.open(path).convert("RGB"); W, H = im.size
# 任务栏那一条是半透明毛玻璃：既不是纯黑也不保色，落在里面的采样一律不作数
import os
WORK_BOTTOM = int(os.environ.get('WORK_BOTTOM', H))   # 任务栏顶边；不给就当整屏可用
if len(sys.argv) >= 10:
    dl, dt, dr, db = [int(v) for v in sys.argv[6:10]]
else:
    dl, dt, dr, db = 0, 0, W, H

VW, VH = 1920, 1080
sx0, sx1, sy0, sy1 = sl*VW, sr*VW, st*VH, sb*VH
# 源坐标 → 屏幕坐标：先映射到 dst 矩形内，再加上 dst 的偏移
fx = lambda x: dl + (x - sx0)/(sx1 - sx0)*(dr - dl)
fy = lambda y: dt + (y - sy0)/(sy1 - sy0)*(db - dt)

is_cyan   = lambda c: c[0] < 120 and c[1] > 140 and c[2] > 160
is_yellow = lambda c: c[0] > 160 and c[1] > 140 and c[2] < 110
is_redish = lambda c: c[0] > c[1]*1.55 and c[0] > c[2]*1.55 and c[0] > 40
is_black  = lambda c: max(c) < 26

# 采样线选在画面里，避开任务栏和窗口
Y = int(dt + (db-dt)*0.79)
X = int(dl + (dr-dl)*0.79)

rows = []
def find(pred, axis, test, span=34):
    lim = W if axis == 'x' else H
    for d in range(-span, span+1):
        v = int(round(pred)) + d
        if not (0 <= v < lim): continue
        if axis == 'y' and v >= WORK_BOTTOM: continue
        if axis == 'x' and Y >= WORK_BOTTOM: continue
        c = im.getpixel((v, Y)) if axis == 'x' else im.getpixel((X, v))
        if test(c): return v
    return None

def add(name, pred, axis, test):
    lim = W if axis == 'x' else H
    if pred < -20 or pred > lim + 20:
        rows.append((name, pred, None, "off-screen (expected)"))
        return
    if axis == 'y' and pred >= WORK_BOTTOM - 10:
        rows.append((name, pred, None, "under taskbar (not checkable)"))
        return
    rows.append((name, pred, find(pred, axis, test), None))

add("x=5% line",  fx(96),   'x', is_cyan)
add("x=95% line", fx(1824), 'x', is_cyan)
add("y=5% line",  fy(54),   'y', is_yellow)
add("y=95% line", fy(1026), 'y', is_yellow)
add("top red",    fy(5),    'y', is_redish)
add("bottom red", fy(1075), 'y', is_redish)
# 网格线每 10% 一条，放大到看不见边框时靠它定位
is_grid = lambda c: 52 < max(c) < 100 and max(c) - min(c) < 22
add("grid x=30%", fx(576),  'x', is_grid)
add("grid y=30%", fy(324),  'y', is_grid)
add("grid x=70%", fx(1344), 'x', is_grid)
add("grid y=70%", fy(756),  'y', is_grid)

print(f"{path}   {W}x{H}   src={sl:.3f},{st:.3f}..{sr:.3f},{sb:.3f}   dst={dl},{dt}..{dr},{db}")
print(f"{'feature':<16}{'pred':>9}{'meas':>9}{'delta':>9}  ok")
ok = True
for n, p, g, note in rows:
    if note:
        print(f"{n:<16}{p:>9.1f}{'--':>9}{'':>9}  - {note}")
        continue
    if g is None:
        print(f"{n:<16}{p:>9.1f}{'MISS':>9}{'':>9}  X"); ok = False; continue
    d = g - p; good = abs(d) <= 12; ok &= good
    print(f"{n:<16}{p:>9.1f}{g:>9}{d:>+9.1f}  {'v' if good else 'X'}")

# letterbox：dst 之外必须是纯黑，不能漏出系统壁纸
if (dl, dt, dr, db) != (0, 0, W, H):
    bars = []
    if dt > 4:   bars.append(("top bar",    [im.getpixel((X, y)) for y in range(2, dt-2, 6)]))
    if db < WORK_BOTTOM-4:
        bars.append(("bottom bar", [im.getpixel((X, y)) for y in range(db+2, WORK_BOTTOM-2, 6)]))
    if dl > 4:   bars.append(("left bar",   [im.getpixel((x, Y)) for x in range(2, dl-2, 6)]))
    if dr < W-4: bars.append(("right bar",  [im.getpixel((x, Y)) for x in range(dr+2, W-2, 6)]))
    for name, px in bars:
        black = sum(1 for c in px if is_black(c))
        good = black >= len(px) * 0.9
        ok &= good
        print(f"{name:<16}{'':>9}{black}/{len(px)} black{'':>1}  {'v' if good else 'X'}")

print("VERDICT:", "PASS" if ok else "FAIL")
