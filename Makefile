CC = clang
CXX = clang++
CFLAGS= -O3 -std=gnu17 -g -I./libft8 -I.
CXXFLAGS= -O3 -x c++ -g -I./libft8 -I.
LIBS = -lusb-1.0 -lrtlsdr -lpthread -lfftw3f -lcurl -lm -lstdc++ -lncurses

#ifeq ($(findstring armv6,$(shell uname -m)),armv6)
# Broadcom BCM2835 SoC with 700 MHz 32-bit ARM 1176JZF-S (ARMv6 arch)
#PI_VERSION = -DRPI1 --target=arm-linux-gnueabihf -mcpu=arm1176jzf-s -mfloat-abi=hard
#else
# Broadcom BCM2836 SoC with 900 MHz 32-bit quad-core ARM Cortex-A7  (ARMv7 arch)
# Broadcom BCM2837 SoC with 1.2 GHz 64-bit quad-core ARM Cortex-A53 (ARMv8 arch)
#PI_VERSION = -DRPI23
#endif

# Identify the target architecture / Raspberry Pi Version

CPUINFO := $(shell cat /proc/cpuinfo)
MACHINE := $(shell uname -m)

# x86 hosts (PC build): receiver only. The FT8 transmitter (ft8) drives the
# Raspberry Pi's PLLD/GPCLK0 via BCM DMA and cannot run on x86, so it and its
# socket-client helpers (client, sk150lm_beacon) are not built here.
ifneq ($(filter x86_64 i386 i686,$(MACHINE)),)
PI_VERSION = -Dx86
IS_X86 = 1

else ifeq ($(findstring ARMv6,$(CPUINFO)),ARMv6)
# Raspberry Pi 1
PI_VERSION = -DRPI1 --target=arm-linux-gnueabihf -mcpu=arm1176jzf-s -mfloat-abi=hard

else ifneq ($(findstring Cortex-A72,$(CPUINFO)),)
# Raspberry Pi 4 (32-bit OS)
PI_VERSION = -DRPI4 -mcpu=cortex-a72

else ifneq ($(findstring 0xd08,$(CPUINFO)),)
# Raspberry Pi 4 (64-bit OS, CPU part 0xd08 = Cortex-A72)
PI_VERSION = -DRPI4

else
# Raspberry Pi 2 / 3
PI_VERSION = -DRPI23
endif

# Note
#   gcc is a bit faster that clang on this app
#   for dbg: -Wall -fsanitize=address

# Project headers. Objects depend on these so that editing a header -- e.g.
# bumping RTLSDR_FT8D_VERSION in rtlsdr_ft8d.h -- forces the affected objects to
# rebuild (a plain pattern rule has no header prerequisites, which previously
# left the version string stale until a manual 'make clean').
HEADERS = rtlsdr_ft8d.h qsoHandler.h ft8_ncurses.h pskreporter.hpp tsqueue.h txcal.h

OBJSFT8D = rtlsdr_ft8d.o libft8/ft8/constants.o libft8/ft8/text.o libft8/ft8/ldpc.o libft8/ft8/crc.o libft8/ft8/message.o libft8/ft8/encode.o libft8/ft8/decode.o libft8/common/monitor.o libft8/fft/kiss_fft.o libft8/fft/kiss_fftr.o pskreporter.o ft8_ncurses.o qsoHandler.o
OBJSFTX = ft8.o libft8/ft8/constants.o libft8/ft8/text.o libft8/ft8/ldpc.o libft8/ft8/crc.o libft8/ft8/message.o libft8/ft8/encode.o libft8/ft8/decode.o libft8/common/monitor.o libft8/fft/kiss_fft.o libft8/fft/kiss_fftr.o stoargc.o mailbox.o
OBJCLI = client.o
OBJSK  = sk150lm_beacon.o
OBJCAL = calibrate.o

# On x86 (PC) only the receiver is built; on the Pi the transmitter and its
# helpers are built as well.
ifdef IS_X86
TARGETS = rtlsdr_ft8d
else
TARGETS = rtlsdr_ft8d ft8 client sk150lm_beacon calibrate
endif

.PHONY: all clean narrowband

all: $(TARGETS)

# Narrowband build: legacy 3200 sps baseband (usable audio ~200..1500 Hz).
# The default build is now WIDE (6400 sps, ~200..2900 Hz). Use this only to
# fall back to the old narrow chain. See rx-characterization.md.
# Forces a clean rebuild since the change is via a compile-time -D flag that the
# per-object timestamps don't track. Usage: `make narrowband`.
narrowband:
	$(MAKE) clean
	$(MAKE) all PI_VERSION="$(PI_VERSION) -DNARROWBAND"

#%.o: %.c
#	${CXX} ${CXXFLAGS} $(PI_VERSION) -c $< -o $@

%.o: %.c $(HEADERS)
	${CC} ${CFLAGS} $(PI_VERSION) -c $< -o $@

libft8/%.o: libft8/%.c
	${CC} ${CFLAGS} $(PI_VERSION) -Wno-format -c $< -o $@

%.o: %.cpp $(HEADERS)
	${CXX} ${CXXFLAGS} $(PI_VERSION) -c $< -o $@

rtlsdr_ft8d: $(OBJSFT8D)
	$(CXX) -o $@ $^ $(LIBS)

ft8: $(OBJSFTX)
	$(CXX) -o $@ $^ $(LIBS)

client: $(OBJCLI)
	$(CXX) -o $@ $^ $(LIBS)


sk150lm_beacon: $(OBJSK)
	$(CXX) -o $@ $^ $(LIBS)


calibrate: $(OBJCAL)
	$(CXX) -o $@ $^ $(LIBS)


# Host-side test-vector generator (not part of 'all'). Builds an .iq file of
# several standard-callsign FT8 signals for hardware-free decode verification.
OBJMKTEST = mktestiq.o libft8/ft8/constants.o libft8/ft8/text.o libft8/ft8/crc.o libft8/ft8/message.o libft8/ft8/encode.o libft8/ft8/ldpc.o

mktestiq: $(OBJMKTEST)
	$(CXX) -o $@ $^ $(LIBS)


clean:
	rm -f *.o libft8/ft8/*.o libft8/common/*.o libft8/fft/*.o rtlsdr_ft8d ft8 client sk150lm_beacon calibrate mktestiq fftw_wisdom.dat selftest.iq test_band.iq

ifdef IS_X86
install:
	install rtlsdr_ft8d /usr/local/bin/rtlsdr_ft8d
else
install:
	install rtlsdr_ft8d /usr/local/bin/rtlsdr_ft8d
	install ft8 /usr/local/bin/ft8
	install calibrate /usr/local/bin/calibrate
	install ft8tx.service /etc/systemd/system/ft8tx.service
	systemctl enable ft8tx.service
endif
