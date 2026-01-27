CC = clang
CFLAGS = -Wall -Wextra -O2 -pthread
CFLAGS_DEBUG = -Wall -Wextra -O0 -g -pthread -DDISABLE_RENDERING
LDFLAGS = -pthread

SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin

TARGET = $(BIN_DIR)/spettrum
ULA_OBJ = $(OBJ_DIR)/ula.o
DISASM_OBJ = $(OBJ_DIR)/disasm.o
Z80_OBJ = $(OBJ_DIR)/z80.o
Z80_SNAPSHOT_OBJ = $(OBJ_DIR)/z80snapshot.o
KEYBOARD_OBJ = $(OBJ_DIR)/keyboard.o
TAP_OBJ = $(OBJ_DIR)/tap.o
SOURCES = $(wildcard $(SRC_DIR)/*.c)
OBJECTS = $(SOURCES:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

.PHONY: all clean test run debug

all: $(TARGET)

debug: CFLAGS = $(CFLAGS_DEBUG)
debug: $(TARGET)

$(TARGET): main.c $(Z80_OBJ) $(ULA_OBJ) $(DISASM_OBJ) $(Z80_SNAPSHOT_OBJ) $(KEYBOARD_OBJ) $(TAP_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ main.c $(Z80_OBJ) $(ULA_OBJ) $(DISASM_OBJ) $(Z80_SNAPSHOT_OBJ) $(KEYBOARD_OBJ) $(TAP_OBJ)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BIN_DIR):
	mkdir -p $@

$(OBJ_DIR):
	mkdir -p $@

run: $(TARGET)
	$(TARGET)

spettrum-version: $(TARGET)
	$(TARGET) --version

$(ULA_OBJ): ula.c ula.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ ula.c

$(Z80_OBJ): z80.c z80.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ z80.c

$(DISASM_OBJ): disasm.c disasm.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ disasm.c

$(Z80_SNAPSHOT_OBJ): z80snapshot.c z80snapshot.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ z80snapshot.c

$(KEYBOARD_OBJ): keyboard.c keyboard.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ keyboard.c

$(TAP_OBJ): tap.c tap.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ tap.c

test:
	$(MAKE) -C tests run

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)
	$(MAKE) -C tests clean
