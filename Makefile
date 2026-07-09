CC      = gcc
OBJCOPY = objcopy

# Size-focused freestanding flags. Limit is 0x10000 (JIT_SIZE in LuaC0re).
CFLAGS  = -Oz -ffreestanding -fno-stack-protector -fno-builtin \
          -fpie -mno-red-zone -fomit-frame-pointer -fcf-protection=none \
          -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables \
          -ffunction-sections -fdata-sections -fmerge-all-constants -fno-ident \
          -fno-align-functions -fno-align-jumps -fno-align-loops -fno-align-labels \
          -Wall -Wno-unused-function -Isrc

LDFLAGS = -T linker.ld -nostdlib -nostartfiles -static \
          -Wl,--build-id=none -Wl,--no-dynamic-linker -Wl,-z,norelro -no-pie \
          -Wl,--gc-sections -Wl,--as-needed

SRCS    = src/main.c src/bus.c src/mapper.c src/cpu.c src/ppu.c src/apu.c src/ftp.c
OBJS    = $(SRCS:.c=.o)
TARGET  = nes_emu

all: $(TARGET).bin

%.o: %.c src/core.h src/nes.h src/mapper.h src/tables.h src/ftp.h
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@echo "Built: $@ ($$(wc -c < $@) bytes)"

# Embed shellcode into nes.lua (local sc = "...")
payload: $(TARGET).bin
	python3 tools/update_nes_lua_sc.py

clean:
	rm -f src/*.o $(TARGET).elf $(TARGET).bin tests/core_smoke tests/apu_smoke tests/nes_romtest

test: tests/core_smoke tests/apu_smoke
	./tests/core_smoke
	./tests/apu_smoke

romtest: tests/nes_romtest

test-rom: tests/nes_romtest
	./tests/nes_romtest $(ROM) $(ARGS)

tests/core_smoke: tests/core_smoke.c src/bus.c src/mapper.c src/cpu.c src/core.h src/nes.h src/mapper.h src/tables.h
	$(CC) -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Isrc tests/core_smoke.c src/bus.c src/mapper.c src/cpu.c -o $@

tests/apu_smoke: tests/apu_smoke.c src/apu.c src/core.h src/nes.h src/tables.h
	$(CC) -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Isrc tests/apu_smoke.c src/apu.c -o $@

tests/nes_romtest: tests/nes_romtest.c src/bus.c src/mapper.c src/cpu.c src/ppu.c src/apu.c src/core.h src/nes.h src/mapper.h src/tables.h
	$(CC) -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Isrc tests/nes_romtest.c src/bus.c src/mapper.c src/cpu.c src/ppu.c src/apu.c -o $@

.PHONY: all clean test romtest test-rom payload
