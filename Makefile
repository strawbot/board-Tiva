# Makefile for TM4C123 blink
CC = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
CFLAGS = -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -O2 -g \
         -DTARGET_IS_TM4C123 -Iinclude
LDFLAGS = -T linker.ld -nostartfiles
SRCS = src/main.c system_tm4c.c
OBJS = $(SRCS:.c=.o)
OUTPUT = firmware.elf
BIN = firmware.bin
HEX = firmware.hex

all: $(BIN) $(HEX)

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

flash: $(BIN)
	python3 -m mikroe_uhb write $(BIN)
