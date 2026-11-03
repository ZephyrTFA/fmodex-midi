# Run inside `nix develop` ($CC = i686-w64-mingw32-gcc, `cc` = native host compiler).

CC      ?= i686-w64-mingw32-gcc
HOSTCC  ?= cc
PYTHON  ?= python3
B       := build

CFLAGS  := -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Isrc -I$(B)
LDFLAGS := -static-libgcc -Wl,--exclude-all-symbols -Wl,--enable-stdcall-fixup

EXPORTS := references/fmodex-4.32.1/exports.txt
PROXY_SRC := src/hooks.c src/smf.c src/player.c src/engine.c src/backend.c src/log.c src/config.c

.PHONY: all proxy midiplay check test dist release clean
all: proxy midiplay

proxy: $(B)/fmodex.dll

$(B)/fmodex.def $(B)/hooks_gen.h: tools/gen_def.py src/hooks.txt $(EXPORTS) | $(B)
	$(PYTHON) tools/gen_def.py $(EXPORTS) src/hooks.txt $(B)/fmodex.def $(B)/hooks_gen.h

$(B)/fmodex.dll: $(PROXY_SRC) src/*.h $(B)/fmodex.def $(B)/hooks_gen.h
	$(CC) -shared $(CFLAGS) $(LDFLAGS) -o $@ $(PROXY_SRC) $(B)/fmodex.def -lwinmm

NM ?= i686-w64-mingw32-nm

# The proxy must export exactly the real DLL's names and import nothing beyond
# kernel32/msvcrt/winmm (+ fmodex_real via forwarders); every hook's stdcall
# byte count must match its mangled signature.
check: $(B)/fmodex.dll
	$(PYTHON) tools/check_exports.py $(B)/fmodex.dll $(EXPORTS) src/hooks.txt
	$(CC) -c $(CFLAGS) -o $(B)/hooks.o src/hooks.c
	$(NM) $(B)/hooks.o > $(B)/hooks.nm
	$(PYTHON) tools/check_stdcall.py $(B)/hooks.nm src/hooks.txt

$(B):
	mkdir -p $@

clean:
	rm -rf $(B)

CORE_SRC := src/smf.c src/player.c src/engine.c src/backend.c src/log.c src/config.c

midiplay: $(B)/midiplay.exe

$(B)/midiplay.exe: src/midiplay.c $(CORE_SRC) src/*.h | $(B)
	$(CC) -municode $(CFLAGS) -static-libgcc -o $@ src/midiplay.c $(CORE_SRC) -lwinmm

# Self-contained installer: one .ps1 with the proxy embedded.
VERSION ?= $(shell git describe --always --dirty 2>/dev/null || echo dev)
# Where the installer tells users to report problems; CI passes it explicitly.
ISSUES_URL ?= $(shell git remote get-url origin 2>/dev/null | sed -E 's#^(git@|https://)github.com[:/]#https://github.com/#; s#\.git$$##; s#$$#/issues#')
dist: $(B)/fmodex-midi.ps1

$(B)/fmodex-midi.ps1: tools/installer.ps1.in tools/make_installer.py $(B)/fmodex.dll fmodex-midi.ini.example
	$(PYTHON) tools/make_installer.py tools/installer.ps1.in $(B)/fmodex.dll fmodex-midi.ini.example $(VERSION) "$(ISSUES_URL)" $@

# Release assets: source zip, the bare DLL (manual install), the one-click
# installer, checksums and release notes. Run on a clean checkout (CI does).
R := $(B)/release
release: $(B)/fmodex.dll tools/installer.ps1.in tools/make_installer.py
	rm -rf $(R) && mkdir -p $(R)
	git archive --format=zip --prefix=fmodex-midi-$(VERSION)/ -o $(R)/fmodex-midi-$(VERSION)-src.zip HEAD
	cp $(B)/fmodex.dll $(R)/
	$(PYTHON) tools/make_installer.py tools/installer.ps1.in $(B)/fmodex.dll fmodex-midi.ini.example $(VERSION) "$(ISSUES_URL)" $(R)/fmodex-midi.ps1
	cd $(R) && sha256sum fmodex-midi.ps1 fmodex.dll fmodex-midi-$(VERSION)-src.zip > SHA256SUMS
	sed -e 's|@VERSION@|$(VERSION)|g' -e 's|@ISSUES_URL@|$(ISSUES_URL)|g' tools/release-notes.md.in > $(R)/NOTES.md
	@ls -l $(R)

# Native host tests: parser vs mido, player unit tests, fuzz smoke.
test:
	test/run_tests.sh
