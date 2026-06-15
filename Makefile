# Makefile for TM4C123 blink
CC = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
CFLAGS = -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -O2 -g \
         -DTARGET_IS_TM4C123 -Iinclude
LDFLAGS = -T linker.ld -nostartfiles
SRCS = src/main.c src/build_timestamp.c system_tm4c.c pinout.c
OBJS = $(SRCS:.c=.o)
OUTPUT = tiva.elf
BIN = tiva.bin
HEX = tiva.hex

.PHONY: build_timestamp
build_timestamp:
	python3 ../../TimbreOS/gen_build_timestamp.py src/build_timestamp.c

all: build_timestamp $(BIN) $(HEX)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(OUTPUT): startup_tm4c.s $(OBJS)
	$(CC) $(CFLAGS) -c startup_tm4c.s -o startup_tm4c.o
	$(CC) $(CFLAGS) startup_tm4c.o $(OBJS) $(LDFLAGS) -o $(OUTPUT)

$(BIN): $(OUTPUT)
	$(OBJCOPY) -O binary $(OUTPUT) $(BIN)

$(HEX): $(OUTPUT)
	$(OBJCOPY) -I binary -O ihex $(OUTPUT) $(HEX)

clean:
	rm -f *.o $(OUTPUT) $(BIN) $(HEX)

flash:
	openocd -f openocd.cfg -c "program .pio/build/mini_m4_tiva/firmware.elf verify" -c "reset run" -c "sleep 200" -c "shutdown"
