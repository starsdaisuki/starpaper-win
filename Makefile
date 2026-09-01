# 在 macOS / Linux 上交叉编译出 Windows exe。
#
#   make          → x64    需要 brew install mingw-w64
#   make arm64    → ARM64  需要 llvm-mingw（见下），产物是原生 ARM64，不走 Prism 模拟
#
# llvm-mingw 不在 brew 里，去 release 页下 macos-universal 那个包解压即可：
#   https://github.com/mstorsjo/llvm-mingw/releases
# 默认找 ~/.local/llvm-mingw；装在别处用 make arm64 LLVM_MINGW=/your/path
#
# Windows 上原生编译用 build.bat（x64 / ARM64 各自开对应的 Native Tools 命令行）。

# 版本号唯一事实源：改这里，x64 / ARM64 的 exe 版本资源一起跟着变。
VERSION  ?= 1.0.3
COMMA    := ,
RCFLAGS  := -DSP_VER_STR=$(VERSION) -DSP_VER_NUM=$(subst .,$(COMMA),$(VERSION)),0

CXX      := x86_64-w64-mingw32-g++
TARGET   := StarPaper.exe
SRC      := src/desktop.cpp src/player.cpp src/pipeline.cpp src/theme.cpp src/widgets.cpp src/thumbs.cpp src/settings.cpp src/startup.cpp src/main.cpp
RES      := build-res.o
# 发布构建会用 make -B 全量重编；这里仍把所有头文件列成普通 make 的依赖，
# 避免日常直接 make 时改了 theme/widgets/thumbs 之类的头却拿到旧 exe。
HDRS     := $(wildcard src/*.h)

CXXFLAGS := -std=c++17 -O2 -Wall -municode -mwindows
# -static* 三件套：不带 libstdc++-6.dll / libgcc_s_seh-1.dll，产物是单文件
LDFLAGS  := -static -static-libgcc -static-libstdc++
LIBS     := -lcomctl32 -lgdi32 -ld3d11 -ldxgi -lmfplat -lmfuuid -lole32 -loleaut32 -lshell32 -lcomdlg32 -lwtsapi32 -luuid -lruntimeobject

# ---- ARM64 交叉编译 ----
# 源码里没有任何架构相关的东西（无 intrinsics / 无内联汇编），换个 target 三元组就够。
# CXXFLAGS / LDFLAGS / LIBS 和 x64 完全共用。
LLVM_MINGW  ?= $(HOME)/.local/llvm-mingw
ARM_CXX     := $(LLVM_MINGW)/bin/aarch64-w64-mingw32-clang++
ARM_WINDRES := $(LLVM_MINGW)/bin/aarch64-w64-mingw32-windres
ARM_OBJDUMP := $(LLVM_MINGW)/bin/aarch64-w64-mingw32-objdump
ARM_TARGET  := StarPaper-arm64.exe
ARM_RES     := build-res-arm64.o

.PHONY: all clean deps arm64 deps-arm64

all: $(TARGET)
arm64: $(ARM_TARGET)

$(RES): res/StarPaper.rc res/StarPaper.ico res/StarPaper.manifest
	x86_64-w64-mingw32-windres $(RCFLAGS) -I res $< -O coff -o $@

$(TARGET): $(SRC) $(RES) $(HDRS)
	$(CXX) $(CXXFLAGS) $(SRC) $(RES) -o $@ $(LDFLAGS) $(LIBS)
	@echo "→ $@ ($$(du -h $@ | cut -f1))"

$(ARM_RES): res/StarPaper.rc res/StarPaper.ico res/StarPaper.manifest
	@test -x $(ARM_CXX) || { \
	  echo "找不到 $(ARM_CXX)"; \
	  echo "装 llvm-mingw：https://github.com/mstorsjo/llvm-mingw/releases"; \
	  echo "（下 macos-universal 那个包，解压成 ~/.local/llvm-mingw）"; \
	  exit 1; }
	$(ARM_WINDRES) $(RCFLAGS) -I res $< -O coff -o $@

$(ARM_TARGET): $(SRC) $(ARM_RES) $(HDRS)
	$(ARM_CXX) $(CXXFLAGS) $(SRC) $(ARM_RES) -o $@ $(LDFLAGS) $(LIBS)
	@echo "→ $@ ($$(du -h $@ | cut -f1))"

# 核对产物只依赖系统 DLL —— 这是本项目唯一的硬指标，改完构建参数必查
deps: $(TARGET)
	@x86_64-w64-mingw32-objdump -p $(TARGET) | grep "DLL Name"

deps-arm64: $(ARM_TARGET)
	@$(ARM_OBJDUMP) -p $(ARM_TARGET) | grep "DLL Name"

clean:
	@test -x /usr/bin/trash || { echo "找不到 /usr/bin/trash；为避免永久删除，clean 已中止"; exit 1; }
	@for f in $(TARGET) $(RES) $(ARM_TARGET) $(ARM_RES); do \
	  [ ! -e "$$f" ] || /usr/bin/trash "$$f"; \
	done
