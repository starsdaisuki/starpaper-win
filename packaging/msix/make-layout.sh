#!/usr/bin/env bash
# 组一个 MSIX 布局目录（exe + AppxManifest.xml + Assets）。
# 只做「摆文件」，不打包、不签名 —— 打包用 makeappx（Windows SDK）或 makemsix。
#
#   ./packaging/msix/make-layout.sh arm64 [输出目录]
#   ./packaging/msix/make-layout.sh x64
#
# 图标使用仓里已经审过的 res/icon.png，再用 sips 生成各尺寸。

set -euo pipefail
[ -x /usr/bin/trash ] || { echo "找不到 /usr/bin/trash；拒绝用永久删除替代。" >&2; exit 1; }

ARCH="${1:-}"
case "$ARCH" in
  arm64) EXE="StarPaper-arm64.exe" ;;
  x64)   EXE="StarPaper.exe" ;;
  *) echo "用法: $0 {x64|arm64} [输出目录]" >&2; exit 2 ;;
esac

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_ROOT="$ROOT/build"
OUT="${2:-$BUILD_ROOT/msix-$ARCH}"

# 输出只允许落在本项目 build/ 下面。先解析成绝对路径，挡住 build/../ 之类
# 看似在 build 内、实际越界的目标；绝不对调用者给的任意目录做递归清空。
OUT="$(python3 - "$BUILD_ROOT" "$OUT" <<'PY'
import os, sys
base = os.path.realpath(sys.argv[1])
out = os.path.abspath(sys.argv[2])
if os.path.islink(out):
    raise SystemExit(f"输出目录不能是 symlink：{out}")
resolved = os.path.realpath(out)
if resolved == base or os.path.commonpath((base, resolved)) != base:
    raise SystemExit(f"输出目录必须真实位于 {base} 内，收到：{out} -> {resolved}")
print(out)
PY
)"
mkdir -p "$BUILD_ROOT"

STAGE="$(mktemp -d "$BUILD_ROOT/.msix-$ARCH.XXXXXX")"
cleanup() {
  if [ -n "${STAGE:-}" ] && [ -e "$STAGE" ]; then
    /usr/bin/trash "$STAGE"
  fi
}
trap cleanup EXIT

# ⚠️ 打包前**强制重编全部依赖**，别信 make 的时间戳判断。
# 2026-08-27 的事故：`git stash pop` 写回源码和上一次编译落在同一秒里，
# make 认为「不比目标新」直接跳过，于是拿旧 exe 打了包发上了商店。
# 只删 exe 仍不够：VERSION 是 Make 变量，变了不会让旧 build-res*.o 自动失效。
# -B 同时重编 C++ 和资源对象，不再靠永久删除构建产物触发。
echo "→ 强制重编 $ARCH"
if [ "$ARCH" = arm64 ]; then make -B -C "$ROOT" arm64 >/dev/null; else make -B -C "$ROOT" >/dev/null; fi
[ -f "$ROOT/$EXE" ] || { echo "重编之后仍然没有 $ROOT/$EXE" >&2; exit 1; }

mkdir -p "$STAGE/Assets"
cp "$ROOT/$EXE" "$STAGE/StarPaper.exe"

# ⚠️ ProcessorArchitecture 必须和 exe 的真实架构一致，否则装到别的架构上会直接崩。
sed "s/ProcessorArchitecture=\"[^\"]*\"/ProcessorArchitecture=\"$ARCH\"/" \
    "$ROOT/packaging/msix/AppxManifest.xml" > "$STAGE/AppxManifest.xml"

# Partner Center 的身份值不入库（见 identity.env.example）。没有 identity.env 时
# 占位符原样保留 —— 本地松散注册照样能跑，只是不能提交到商店。
IDENT="$ROOT/packaging/msix/identity.env"
if [ -f "$IDENT" ]; then
  # shellcheck disable=SC1090
  . "$IDENT"
  for v in IDENTITY_NAME IDENTITY_PUBLISHER PUBLISHER_DISPLAY_NAME; do
    val="${!v:-}"
    [ -n "$val" ] || { echo "identity.env 里缺 $v" >&2; exit 1; }
  done
  # 不用 sed 拼 replacement：显示名里若有 & / | / 反斜杠会被 sed 当语法。
  python3 - "$STAGE/AppxManifest.xml" \
    "$IDENTITY_NAME" "$IDENTITY_PUBLISHER" "$PUBLISHER_DISPLAY_NAME" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text(encoding="utf-8")
for token, value in zip(
    ("__IDENTITY_NAME__", "__IDENTITY_PUBLISHER__", "__PUBLISHER_DISPLAY_NAME__"),
    sys.argv[2:],
):
    s = s.replace(token, value)
p.write_text(s, encoding="utf-8")
PY
  echo "  identity: $IDENTITY_NAME"
else
  echo "⚠️  没有 packaging/msix/identity.env，manifest 保留占位符。" >&2
  echo "    要提交商店的话：cp identity.env.example identity.env 并填入 Partner Center 的值。" >&2
fi

SRC="$ROOT/res/icon.png"
[ -f "$SRC" ] || { echo "找不到图标源：$SRC" >&2; exit 1; }
gen() { sips -z "$2" "$2" "$SRC" --out "$STAGE/Assets/$1" >/dev/null; }
gen StoreLogo.png            50
gen Square44x44Logo.png      44
gen Square71x71Logo.png      71
gen Square150x150Logo.png    150
for s in 16 24 32 48 256; do gen "Square44x44Logo.targetsize-$s.png" "$s"; done

# 旧布局整个进废纸篓，再把已经完整生成的 staging 原子换过去。
# trash 移动目录本身，不递归遍历其中的 symlink。
if [ -e "$OUT" ]; then
  echo "→ 旧布局移入废纸篓：$OUT"
  /usr/bin/trash "$OUT"
fi
mkdir -p "$(dirname "$OUT")"
mv "$STAGE" "$OUT"
STAGE=""

echo "→ $OUT"
find "$OUT" -type f | sed "s|$OUT|  .|" | sort
