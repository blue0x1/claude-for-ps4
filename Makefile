# Claude for PS4: chat with Claude on a jailbroken PS4.
# Build: make            (needs the OpenOrbis toolchain, clang and ld.lld)
#        make OO_PS4_TOOLCHAIN=/path/to/PS4Toolchain

TITLE       := Claude for PS4
VERSION     := 01.21
TITLE_ID    := CLDE00001
CONTENT_ID  := IV0000-CLDE00001_00-CLAUDEFORPS40000

OO_PS4_TOOLCHAIN ?= /opt/oo/OpenOrbis/PS4Toolchain
TOOLCHAIN   := $(OO_PS4_TOOLCHAIN)
export OO_PS4_TOOLCHAIN

LIBS        := -lc -lkernel -lc++ -lSceUserService -lSceVideoOut -lSceAudioOut -lScePad \
               -lSceSysmodule -lSceFreeType -lSceNet -lSceSsl -lSceHttp \
               -lSceCommonDialog -lSceImeDialog -lSceNetCtl -lSceSystemService -lSDL2

ASSETS      := $(wildcard assets/fonts/*)
LIBMODULES  := $(wildcard sce_module/*)

BUILDDIR    := build
CFILES      := $(wildcard src/*.c)
OBJS        := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(CFILES))
HEADERS     := $(wildcard src/*.h)

CFLAGS      := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -Wall -Wno-unused-function -D__ORBIS__ -DAPP_VERSION=\"$(VERSION)\" \
               -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include -isystem $(TOOLCHAIN)/include/stb -c
LDFLAGS     := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr -L$(TOOLCHAIN)/lib $(LIBS) $(TOOLCHAIN)/lib/crt1.o

UNAME_S     := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
CC          := /usr/local/opt/llvm/bin/clang
LD          := /usr/local/opt/llvm/bin/ld.lld
CDIR        := macos
else
CC          := clang
LD          := ld.lld
CDIR        := linux
endif
BIN         := $(TOOLCHAIN)/bin/$(CDIR)
# PkgTool.Core is a .NET app; this avoids needing libicu on the build machine.
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT := 1

.PHONY: all clean
all: $(CONTENT_ID).pkg

$(CONTENT_ID).pkg: pkg.gp4
	$(BIN)/PkgTool.Core pkg_build $< .

pkg.gp4: eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png sce_sys/pic1.png $(LIBMODULES) $(ASSETS)
	$(BIN)/create-gp4 -out $@ --content-id=$(CONTENT_ID) --files "$^"

sce_sys/param.sfo: Makefile
	$(BIN)/PkgTool.Core sfo_new $@
	$(BIN)/PkgTool.Core sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(BIN)/PkgTool.Core sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(VERSION)'
	$(BIN)/PkgTool.Core sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0
	$(BIN)/PkgTool.Core sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(BIN)/PkgTool.Core sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(BIN)/PkgTool.Core sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(BIN)/PkgTool.Core sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(BIN)/PkgTool.Core sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(BIN)/PkgTool.Core sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(BIN)/PkgTool.Core sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(VERSION)'

eboot.bin: $(OBJS)
	$(LD) $(OBJS) -o $(BUILDDIR)/claude.elf $(LDFLAGS)
	$(BIN)/create-fself -in=$(BUILDDIR)/claude.elf -out=$(BUILDDIR)/claude.oelf --eboot "eboot.bin" --paid 0x3800000000000011

$(BUILDDIR)/%.o: src/%.c $(HEADERS)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -rf $(BUILDDIR) $(CONTENT_ID).pkg pkg.gp4 sce_sys/param.sfo eboot.bin
