CC = clang
CXX = clang++
CFLAGS= -O3 -std=gnu17 -g -I./ft8_lib -I.
CXXFLAGS= -O3 -x c++ -g -I./ft8_lib -I.
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

OBJSFT8D = rtlsdr_ft8d.o ft8_lib/ft8/constants.o ft8_lib/ft8/text.o ft8_lib/ft8/ldpc.o ft8_lib/ft8/crc.o ft8_lib/ft8/message.o ft8_lib/ft8/encode.o ft8_lib/ft8/decode.o ft8_lib/common/monitor.o ft8_lib/fft/kiss_fft.o ft8_lib/fft/kiss_fftr.o pskreporter.o ft8_ncurses.o qsoHandler.o
OBJSFTX = ft8.o ft8_lib/ft8/constants.o ft8_lib/ft8/text.o ft8_lib/ft8/ldpc.o ft8_lib/ft8/crc.o ft8_lib/ft8/message.o ft8_lib/ft8/encode.o ft8_lib/ft8/decode.o ft8_lib/common/monitor.o ft8_lib/fft/kiss_fft.o ft8_lib/fft/kiss_fftr.o stoargc.o mailbox.o
OBJCLI = client.o
OBJSK  = sk150lm_beacon.o

# On x86 (PC) only the receiver is built; on the Pi the transmitter and its
# helpers are built as well.
ifdef IS_X86
TARGETS = rtlsdr_ft8d
else
TARGETS = rtlsdr_ft8d ft8 client sk150lm_beacon
endif

.PHONY: all clean

all: $(TARGETS)

#%.o: %.c
#	${CXX} ${CXXFLAGS} $(PI_VERSION) -c $< -o $@

%.o: %.c
	${CC} ${CFLAGS} $(PI_VERSION) -c $< -o $@

ft8_lib/%.o: ft8_lib/%.c
	${CC} ${CFLAGS} $(PI_VERSION) -Wno-format -c $< -o $@

%.o: %.cpp
	${CXX} ${CXXFLAGS} $(PI_VERSION) -c $< -o $@

rtlsdr_ft8d: $(OBJSFT8D)
	$(CXX) -o $@ $^ $(LIBS)

ft8: $(OBJSFTX)
	$(CXX) -o $@ $^ $(LIBS)

client: $(OBJCLI)
	$(CXX) -o $@ $^ $(LIBS)


sk150lm_beacon: $(OBJSK)
	$(CXX) -o $@ $^ $(LIBS)


clean:
	rm -f *.o ft8_lib/ft8/*.o rtlsdr_ft8d ft8 client sk150lm_beacon fftw_wisdom.dat selftest.iq

ifdef IS_X86
install:
	install rtlsdr_ft8d /usr/local/bin/rtlsdr_ft8d
else
install:
	install rtlsdr_ft8d /usr/local/bin/rtlsdr_ft8d
	install ft8 /usr/local/bin/ft8
	install ft8tx.service /etc/systemd/system/ft8tx.service
	systemctl enable ft8tx.service
endif
