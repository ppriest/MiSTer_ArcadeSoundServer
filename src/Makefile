# arcsnd's native benches. Run from an MSYS2 MINGW64 shell (or with its bin first on PATH); model3_bench also builds on Linux.
#   make hornet_bench   the Hornet board against a MAME capture (hornet/bench.cpp)
#   make model3_bench   the Model 3 sound board against a Supermodel capture (model3/bench.cpp)
CC       ?= gcc
CXX      ?= g++
CFLAGS   ?= -O2
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wno-sign-compare
OUT      ?= ../obj

MUSASHI = hornet/musashi/m68kcpu.c hornet/musashi/m68kops.c hornet/musashi/m68kdasm.c hornet/musashi/softfloat/softfloat.c
MUSASHI_O = $(patsubst %.c,$(OUT)/%.o,$(notdir $(MUSASHI)))
HORNET = hornet/board.cpp hornet/rf5c400.cpp common/logerror.cpp common/m68k_bus.cpp
# the Model 3 board (added with it): its bench, and m68k_bus, which both boards now share for Musashi's callbacks
MODEL3 = model3/board.cpp model3/SCSP.cpp model3/SCSPDSP.cpp model3/scspdsp_jit.cpp model3/dsplink_shm.cpp model3/dsb.cpp model3/z80/Z80.cpp common/m68k_bus.cpp

hornet_bench: $(OUT)/hornet_bench
model3_bench: $(OUT)/model3_bench

$(OUT)/%.o: hornet/musashi/%.c
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -Ihornet/musashi -c -o $@ $<
$(OUT)/softfloat.o: hornet/musashi/softfloat/softfloat.c
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -Ihornet/musashi -c -o $@ $<
# the Digital Sound Board's spin skip reads Musashi's interrupt state, so it is built against Musashi's headers
$(OUT)/dsb_m68k.o: model3/dsb_m68k.c
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -Ihornet/musashi -c -o $@ $<

$(OUT)/hornet_bench: $(HORNET) hornet/bench.cpp $(wildcard hornet/*.h common/*.h) $(MUSASHI_O)
	$(CXX) $(CXXFLAGS) -Ihornet -Icommon -o $@ $(HORNET) hornet/bench.cpp $(MUSASHI_O)

$(OUT)/model3_bench: $(MODEL3) model3/bench.cpp model3/SCSPLFO.cpp $(wildcard model3/*.h model3/*/*.h common/*.h) $(MUSASHI_O) $(OUT)/dsb_m68k.o
	$(CXX) $(CXXFLAGS) -Imodel3 -Icommon -o $@ $(MODEL3) model3/bench.cpp $(MUSASHI_O) $(OUT)/dsb_m68k.o

.PHONY: hornet_bench model3_bench
