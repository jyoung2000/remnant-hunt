# Remnant x Monster Hunter: World mashup. Cross-compiles the Windows binaries with llvm-mingw.
#   make            build/win/{dinput8.dll, remnant_hunt.dll, r2read.exe}
#   make gen        regenerate src/core/gen/sheets.gen.h from the sheets (runs preflight first)
#   make package    out/remnant_hunt-<version>.zip, the release Melty installs into the game folder
#   make test       Linux build of r2read cross-checked against the dev tools (needs Remnant II installed)
TC      ?= $(HOME)/tc/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64/bin
CXX     := $(TC)/x86_64-w64-mingw32-clang++
CC      := $(TC)/x86_64-w64-mingw32-clang
VERSION ?= 0.1.0
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
	$(CXX) -O2 -std=c++17 -Wall -Wno-unused-function -Ithird_party/ww2ogg -Ibuild/gen -c $< -o $@

$(O)/ww2ogg/%.o: third_party/ww2ogg/%
	@mkdir -p $(dir $@)
	$(CXX) -O2 -std=c++17 -Wno-everything -c $< -o $@

$(O)/ww2ogg/crc.c.o: third_party/ww2ogg/crc.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wno-everything -c $< -o $@

$(O)/stb/stb_vorbis.c.o: third_party/stb/stb_vorbis.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wno-everything -DSTB_VORBIS_NO_STDIO -c $< -o $@

R2READ_OBJ := $(O)/r2read/main.cpp.o $(O)/r2read/r2pak.cpp.o $(O)/r2read/wwise.cpp.o \
              $(patsubst third_party/ww2ogg/%,$(O)/ww2ogg/%.o,$(WW2OGG)) $(O)/ww2ogg/crc.c.o $(O)/stb/stb_vorbis.c.o

$(B)/r2read.exe: $(R2READ_OBJ) $(patsubst third_party/ooz/%,$(O)/ooz/%.o,$(OOZ))
	@mkdir -p $(B)
	$(CXX) -static -s -O2 -o $@ $^

test:
	@mkdir -p build/linux
	g++ -O2 -std=c++17 -DOODLE_ENABLE_AVX2 -include immintrin.h -Ithird_party/ooz -Ithird_party/ww2ogg -Ibuild/gen \
	    -o build/linux/r2read src/r2read/main.cpp src/r2read/r2pak.cpp src/r2read/wwise.cpp $(WW2OGG) \
	    -x c third_party/ww2ogg/crc.c third_party/stb/stb_vorbis.c -x none $(OOZ) -w
	python3 tools/test_r2read.py build/linux/r2read "$(R2)"

package: all
	python3 tools/package.py $(VERSION)

.PHONY: all gen test package
