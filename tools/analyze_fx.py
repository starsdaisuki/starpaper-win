"""调色验收：把 20 张截图变成数字，逐条判定。

⚠️ 壁纸是**动态视频**，每张截图是不同的一帧，所以任何绝对差值里都混着「内容自己变了」。
   00-off 和 19-restore 是同一套（无调色）参数下的两帧 —— 它们之间的差就是纯内容漂移，
   用它当噪声底。任何效果必须显著大于这个底才算数。
"""
import sys, os, numpy as np
from PIL import Image

d = sys.argv[1]
def load(tag):
    im = Image.open(os.path.join(d, f"fx-{tag}.jpg")).convert("RGB")
    return np.asarray(im, dtype=np.float32) / 255.0

def metrics(a):
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    y = 0.2126 * r + 0.7152 * g + 0.0722 * b
    mx, mn = a.max(-1), a.min(-1)
    sat = np.where(mx > 1e-4, (mx - mn) / np.maximum(mx, 1e-4), 0.0)

    # 锐度：灰度的拉普拉斯方差。模糊会砸掉它，锐化会抬高它。
    yy = y[::2, ::2]
    lap = (yy[1:-1, 1:-1] * 4 - yy[:-2, 1:-1] - yy[2:, 1:-1] - yy[1:-1, :-2] - yy[1:-1, 2:])
    sharp = float(lap.var())
    # ⚠️ 拉普拉斯方差对「已经很糊」的图会触底：blur=40 时它已经掉到基准的 15%，
    #    再糊下去几乎不动了。2026-08-23 就是被它误判成「最大半径没生效」。
    #    一阶梯度能量在糊掉之后还有分辨力，判模糊强弱一律看它。
    hf = float(np.abs(np.diff(yy, axis=1)).mean())

    h, w = y.shape
    ch, cw = h // 6, w // 6
    center = float(y[h//2-ch:h//2+ch, w//2-cw:w//2+cw].mean())
    corners = float(np.mean([y[:ch, :cw].mean(), y[:ch, -cw:].mean(),
                             y[-ch:, :cw].mean(), y[-ch:, -cw:].mean()]))
    flat = np.sort(y.ravel())
    n = flat.size
    return dict(R=float(r.mean()), G=float(g.mean()), B=float(b.mean()),
                Y=float(y.mean()), S=float(sat.mean()), sharp=sharp,
                hf=hf, center=center, corner=corners, vig=corners / max(center, 1e-4),
                hi=float(flat[int(n*0.90):].mean()), lo=float(flat[:int(n*0.10)].mean()))

tags = ["00-off","01-mono","02-sat200","03-dim60","04-bright","05-contrast",
        "06-warm","07-cool","08-tint","09-expo","10-gamma","11-blur","12-blur-max",
        "13-vignette","14-sharpen","15-highlight","16-shadows","17-vibrance",
        "18-combo","19-restore"]
M = {t: metrics(load(t)) for t in tags}
base, rest = M["00-off"], M["19-restore"]

# 噪声底：无调色的两帧之间，各指标差了多少
noise = {k: abs(rest[k] - base[k]) for k in base}
print("=== 噪声底（00-off vs 19-restore，两者都没调色，差值＝纯内容漂移）===")
for k in ("Y","S","sharp","hf","vig","hi","lo","R","B"):
    rel = noise[k] / max(abs(base[k]), 1e-6) * 100
    print(f"  {k:6s} {noise[k]:+.4f}  ({rel:.1f}%)")

print("\n=== 各场景 ===")
print(f"{'场景':<14}{'亮度Y':>8}{'饱和S':>8}{'锐度':>10}{'高频':>10}{'角/中':>8}{'R-B':>8}   判定")

def fmt(t):
    m = M[t]
    return f"{m['Y']:8.3f}{m['S']:8.3f}{m['sharp']:10.5f}{m['hf']:10.6f}{m['vig']:8.3f}{m['R']-m['B']:8.3f}"

def chk(cond, msg_ok, msg_bad):
    return ("PASS " + msg_ok) if cond else ("**FAIL** " + msg_bad)

verdict = {}
b = base
verdict["00-off"]      = "基准"
verdict["01-mono"]     = chk(M["01-mono"]["S"] < 0.02, f"S={M['01-mono']['S']:.4f} 已归零", f"S={M['01-mono']['S']:.4f} 不是黑白")
verdict["02-sat200"]   = chk(M["02-sat200"]["S"] > b["S"] * 1.3, "饱和度明显上升", "饱和度没涨够")
verdict["03-dim60"]    = chk(M["03-dim60"]["Y"] < b["Y"] * 0.55, f"亮度降到 {M['03-dim60']['Y']/b['Y']*100:.0f}%（期望≈40%）", "没变暗")
verdict["04-bright"]   = chk(M["04-bright"]["Y"] > b["Y"] + 0.15, "整体抬亮", "没变亮")
verdict["05-contrast"] = chk(M["05-contrast"]["hi"] > b["hi"] and M["05-contrast"]["lo"] < b["lo"],
                             f"亮部 {b['hi']:.3f}→{M['05-contrast']['hi']:.3f}，暗部 {b['lo']:.3f}→{M['05-contrast']['lo']:.3f}", "两头没拉开")
verdict["06-warm"]     = chk(M["06-warm"]["R"]-M["06-warm"]["B"] > (b["R"]-b["B"]) + 0.03, "红多于蓝，偏暖", "没变暖")
verdict["07-cool"]     = chk(M["07-cool"]["R"]-M["07-cool"]["B"] < (b["R"]-b["B"]) - 0.03, "蓝多于红，偏冷", "没变冷")
verdict["08-tint"]     = chk(M["08-tint"]["G"] < b["G"] and (M["08-tint"]["R"]+M["08-tint"]["B"]) > (b["R"]+b["B"]),
                             "绿被压、红蓝被提＝偏品红", "没往品红走")
verdict["09-expo"]     = chk(M["09-expo"]["Y"] > b["Y"] * 1.4, f"亮度 ×{M['09-expo']['Y']/b['Y']:.2f}", "曝光没提上去")
verdict["10-gamma"]    = chk(M["10-gamma"]["lo"] > b["lo"] * 1.3, f"暗部 {b['lo']:.3f}→{M['10-gamma']['lo']:.3f}", "暗部没提亮")
verdict["11-blur"]     = chk(M["11-blur"]["hf"] < b["hf"] * 0.3, f"高频掉到 {M['11-blur']['hf']/b['hf']*100:.1f}%", "没糊")
verdict["12-blur-max"] = chk(M["12-blur-max"]["hf"] < M["11-blur"]["hf"] * 0.97,
                             f"高频 {M['11-blur']['hf']:.6f}→{M['12-blur-max']['hf']:.6f}，比 blur40 更糊",
                             f"高频 {M['11-blur']['hf']:.6f}→{M['12-blur-max']['hf']:.6f}，最大半径没更糊")
verdict["13-vignette"] = chk(M["13-vignette"]["vig"] < b["vig"] * 0.8,
                             f"角/中 {b['vig']:.3f}→{M['13-vignette']['vig']:.3f}", "四角没被压暗")
verdict["14-sharpen"]  = chk(M["14-sharpen"]["sharp"] > b["sharp"] * 1.15, f"锐度 ×{M['14-sharpen']['sharp']/b['sharp']:.2f}", "没变锐")
verdict["15-highlight"]= chk(M["15-highlight"]["hi"] < b["hi"] - 0.02 and abs(M["15-highlight"]["lo"]-b["lo"]) < 0.03,
                             f"亮部 {b['hi']:.3f}→{M['15-highlight']['hi']:.3f}，暗部基本没动", "没有只压亮部")
verdict["16-shadows"]  = chk(M["16-shadows"]["lo"] > b["lo"] + 0.01, f"暗部 {b['lo']:.3f}→{M['16-shadows']['lo']:.3f}", "暗部没提")
verdict["17-vibrance"] = chk(M["17-vibrance"]["S"] > b["S"] * 1.1, "饱和度上升", "没生效")
verdict["18-combo"]    = chk(M["18-combo"]["sharp"] < b["sharp"] * 0.8 and M["18-combo"]["S"] > b["S"] * 1.15,
                             "既糊了（模糊生效）又更艳（调色生效）＝三趟都跑到了", "多趟叠加有一环没生效")
verdict["19-restore"]  = chk(abs(rest["Y"]-b["Y"]) < 0.05 and abs(rest["S"]-b["S"]) < 0.05,
                             "回到基准（差值在内容漂移范围内）", "清空后没回到默认")

for t in tags:
    print(f"{t:<14}{fmt(t)}   {verdict[t]}")

fails = [t for t in tags if "FAIL" in verdict[t]]
print(f"\n{'='*70}")
print(f"通过 {len(tags)-1-len(fails)}/{len(tags)-1}" + ("" if not fails else f"，失败：{', '.join(fails)}"))
