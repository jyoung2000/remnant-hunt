# Remnant x Monster Hunter: World mashup. Cross-compiles the Windows binaries with llvm-mingw.
#   make            build/win/{dinput8.dll, remnant_hunt.dll, r2read.exe}
#   make gen        regenerate src/core/gen/sheets.gen.h from the sheets (runs preflight first)
#   make package    out/remnant_hunt-<version>.zip, the release Melty installs into the game folder
#   make installer  out/RemnantHuntSetup-<version>.exe, the same files in a one-file Windows installer with a window
#   make test       Linux build of r2read cross-checked against the dev tools (needs Remnant II installed)
#   make test-logic turf wars, stamina, progression on fake game data (needs Wine)
TC      ?= $(HOME)/tc/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64/bin
CXX     := $(TC)/x86_64-w64-mingw32-clang++
CC      := $(TC)/x86_64-w64-mingw32-clang
WINDRES := $(TC)/x86_64-w64-mingw32-windres
VERSION ?= 0.4.3
B       := build/win
O       := build/obj

CXXFLAGS := -O2 -std=c++17 -Wall -Wno-unused-function -Wno-missing-braces -DNOMINMAX -DNDEBUG \
            -DIMGUI_USER_CONFIG='"rh_imconfig.h"' \
            -Ithird_party/minhook/include -Ithird_party/imgui -Ithird_party/imgui/backends -Isrc/core
CFLAGS   := -O2 -Ithird_party/minhook/include
LDFLAGS  := -shared -static -s -Wl,--no-insert-timestamp

IMGUI := third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp \
         third_party/imgui/imgui_widgets.cpp third_party/imgui/backends/imgui_impl_dx11.cpp \
         third_party/imgui/backends/imgui_impl_win32.cpp
MINHOOK := third_party/minhook/src/buffer.c third_party/minhook/src/hook.c third_party/minhook/src/trampoline.c \
           third_party/minhook/src/hde/hde64.c
CORE := $(wildcard src/core/*.cpp) src/common/log.cpp
OOZ  := $(addprefix third_party/ooz/,kraken_impl.cpp mermaid_impl.cpp leviathan_impl.cpp lzna_impl.cpp lzna.cpp \
        bitknit.cpp bitreader.cpp util.cpp)

obj = $(patsubst %,$(O)/%.o,$(1))

all: $(B)/dinput8.dll $(B)/remnant_hunt.dll $(B)/r2read.exe

src/core/gen/sheets.gen.h: $(wildcard sheets/*.json) tools/gen.py tools/preflight.py
	python3 tools/gen.py

gen: src/core/gen/sheets.gen.h

$(O)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(O)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(call obj,$(CORE)): src/core/gen/sheets.gen.h src/core/core.h src/core/state.h

$(B)/dinput8.dll: $(call obj,src/loader/loader.cpp src/common/log.cpp) src/loader/dinput8.def
	@mkdir -p $(B)
	$(CXX) $(LDFLAGS) -o $@ $(call obj,src/loader/loader.cpp src/common/log.cpp) src/loader/dinput8.def -luser32

$(B)/remnant_hunt.dll: $(call obj,$(CORE) $(IMGUI) $(MINHOOK))
	@mkdir -p $(B)
	$(CXX) $(LDFLAGS) -o $@ $^ -ld3d11 -ldxgi -ldinput8 -ldxguid -luuid -ld3dcompiler -luser32 -lgdi32 -ldwmapi -limm32 -ladvapi32

$(O)/ooz/%.o: third_party/ooz/%
	@mkdir -p $(dir $@)
	$(CXX) -O2 -std=c++17 -DNDEBUG -DOODLE_ENABLE_AVX2 -include immintrin.h -Wno-everything -Ithird_party/ooz -c $< -o $@

# Wwise Vorbis: ww2ogg (BSD-3) rebuilds Ogg Vorbis, stb_vorbis (public domain) decodes it.
WW2OGG := third_party/ww2ogg/wwriff.cpp third_party/ww2ogg/codebook.cpp
build/gen/codebooks.inc: third_party/ww2ogg/packed_codebooks_aoTuV_603.bin
	@mkdir -p build/gen
	python3 -c "import sys;d=open(sys.argv[1],'rb').read();open(sys.argv[2],'w').write('static const unsigned char kCodebooks[] = {'+','.join(str(x) for x in d)+'};\nstatic const size_t kCodebooksSize = %d;\n'%len(d))" $< $@

$(O)/r2read/%.o: src/r2read/% build/gen/codebooks.inc
	@mkdir -p $(dir $@)
	$(CXX) -O2 -std=c++17 -Wall -Wno-unused-function -Ithird_party/ww2ogg -Ibuild/gen -Ithird_party/acl/includes -Ithird_party/rtm/includes -c $< -o $@

$(O)/ww2ogg/%.o: third_party/ww2ogg/%
	@mkdir -p $(dir $@)
	$(CXX) -O2 -std=c++17 -Wno-everything -c $< -o $@

$(O)/ww2ogg/crc.c.o: third_party/ww2ogg/crc.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wno-everything -c $< -o $@

$(O)/stb/stb_vorbis.c.o: third_party/stb/stb_vorbis.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wno-everything -DSTB_VORBIS_NO_STDIO -c $< -o $@

R2READ_OBJ := $(O)/r2read/main.cpp.o $(O)/r2read/r2pak.cpp.o $(O)/r2read/wwise.cpp.o $(O)/r2read/r2mesh.cpp.o \
              $(patsubst third_party/ww2ogg/%,$(O)/ww2ogg/%.o,$(WW2OGG)) $(O)/ww2ogg/crc.c.o $(O)/stb/stb_vorbis.c.o

$(B)/r2read.exe: $(R2READ_OBJ) $(patsubst third_party/ooz/%,$(O)/ooz/%.o,$(OOZ))
	@mkdir -p $(B)
	$(CXX) -static -s -O2 -o $@ $^

$(O)/mhwread/%.o: src/mhwread/%
	@mkdir -p $(dir $@)
	$(CXX) -O2 -std=c++17 -Wall -c $< -o $@

# mhwread: reads Monster Hunter: World chunk archives in place (tools/mhwchunk_notes.md); not part of 'all'
mhwread: $(B)/mhwread.exe
$(B)/mhwread.exe: $(O)/mhwread/main.cpp.o $(patsubst third_party/ooz/%,$(O)/ooz/%.o,$(OOZ))
	@mkdir -p $(B)
	$(CXX) -static -s -O2 -o $@ $^

# r2read for Linux (dev tools: tools/test_mesh.py, tools/author_bosses.py, tools/attack_timing.py)
linux:
	@mkdir -p build/linux
	g++ -O2 -std=c++17 -DOODLE_ENABLE_AVX2 -include immintrin.h -Ithird_party/ooz -Ithird_party/ww2ogg -Ibuild/gen \
	    -Ithird_party/acl/includes -Ithird_party/rtm/includes \
	    -o build/linux/r2read src/r2read/main.cpp src/r2read/r2pak.cpp src/r2read/wwise.cpp src/r2read/r2mesh.cpp \
	    $(WW2OGG) -x c third_party/ww2ogg/crc.c third_party/stb/stb_vorbis.c -x none $(OOZ) -w

test: linux
	python3 tools/test_r2read.py build/linux/r2read "$(R2)"

package: all
	python3 tools/package.py $(VERSION)

# Host test of the turf wars and the stamina bar on fake game data (a Windows build, run under Wine).
test-logic: src/core/gen/sheets.gen.h
	@mkdir -p build
	$(CXX) -O1 -std=c++17 -Wall -Wno-unused-function -Wno-missing-braces -Isrc/core tests/test_turf_stamina.cpp \
	    src/core/turf.cpp src/core/stamina.cpp src/core/effects.cpp src/core/mem.cpp -static -o build/test_logic.exe
	$(CXX) -O1 -std=c++17 -Wall -Wno-unused-function -Wno-missing-braces -Isrc/core tests/test_progress.cpp \
	    src/core/progress.cpp src/core/effects.cpp src/core/mem.cpp -static -o build/test_progress.exe
	$(CXX) -O1 -std=c++17 -Wall -Wno-unused-function -Wno-missing-braces -Isrc/core tests/test_craft.cpp 	    src/core/inventory.cpp src/core/mem.cpp -static -o build/test_craft.exe
	$(CXX) -O1 -std=c++17 -Wall -Wno-unused-function -Wno-missing-braces -Isrc/core tests/test_hunter.cpp src/core/hunter_rig.cpp -static -o build/test_hunter.exe
	$(CXX) -O1 -std=c++17 -Wall -Isrc/core tests/test_key_edges.cpp -static -o build/test_key_edges.exe
	$(CXX) -O1 -std=c++17 -Wall -Wno-unused-function -Wno-missing-braces -Isrc/core -DRH_TERRAIN_ONLY tests/test_terrain.cpp src/core/bosses.cpp -static -o build/test_terrain.exe
	wine build/test_logic.exe
	wine build/test_progress.exe
	wine build/test_craft.exe
	wine build/test_hunter.exe
	wine build/test_key_edges.exe
	wine build/test_terrain.exe

# dev preview of the screens (no game needed): wine build/preview_ui.exe <screen 0-4> <dir with fonts\> <out.bmp>
preview-ui: src/core/gen/sheets.gen.h
	@mkdir -p build
	$(CXX) -O2 -std=c++17 -DNOMINMAX -DRH_PREVIEW -DIMGUI_USER_CONFIG='"rh_imconfig.h"' -Ithird_party/imgui -Ithird_party/bcdec -Isrc/core \
	    tests/preview_ui.cpp src/core/ui.cpp src/core/progress.cpp src/core/effects.cpp src/core/mem.cpp $(filter-out third_party/imgui/backends/%,$(IMGUI)) \
	    -static -o build/preview_ui.exe

installer: all
	python3 tools/make_installer.py $(VERSION)
	$(WINDRES) build/installer/setup.rc -O coff -o build/installer/setup.res.o
	@mkdir -p out
	$(CXX) -O2 -std=c++17 -Wall -DNOMINMAX -municode -mwindows -static -s -Wl,--no-insert-timestamp \
	    -Ibuild/installer src/installer/setup.cpp build/installer/setup.res.o \
	    -lcomctl32 -lole32 -lshell32 -luxtheme -luuid -o out/RemnantHuntSetup-$(VERSION).exe

.PHONY: all gen test test-logic preview-ui package installer linux
