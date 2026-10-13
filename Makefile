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

CXXFLAGS := -O2 -std=c++17 -Wall -Wno-unused-function -Wno-missing-braces -DNOMINMAX \
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
	$(CXX) -O2 -std=c++17 -DOODLE_ENABLE_AVX2 -include immintrin.h -Wno-everything -Ithird_party/ooz -c $< -o $@

$(O)/r2read/%.o: src/r2read/%
	@mkdir -p $(dir $@)
	$(CXX) -O2 -std=c++17 -Wall -c $< -o $@

$(B)/r2read.exe: $(O)/r2read/main.cpp.o $(O)/r2read/r2pak.cpp.o $(patsubst third_party/ooz/%,$(O)/ooz/%.o,$(OOZ))
	@mkdir -p $(B)
	$(CXX) -static -s -O2 -o $@ $^

test:
	@mkdir -p build/linux
	g++ -O2 -std=c++17 -DOODLE_ENABLE_AVX2 -include immintrin.h -Ithird_party/ooz -o build/linux/r2read \
	    src/r2read/main.cpp src/r2read/r2pak.cpp $(OOZ) -w
	python3 tools/test_r2read.py build/linux/r2read "$(R2)"

package: all
	python3 tools/package.py $(VERSION)

.PHONY: all gen test package
