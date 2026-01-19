CC = clang
CFLAGS = -Wall -Wextra -O2 -pthread
LDFLAGS = -pthread

SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin

TARGET = $(BIN_DIR)/spettrum
DEMO = $(BIN_DIR)/demo
ULA_OBJ = $(OBJ_DIR)/ula.o
Z80_OBJ = $(OBJ_DIR)/z80.o
SOURCES = $(wildcard $(SRC_DIR)/*.c)
OBJECTS = $(SOURCES:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

.PHONY: all clean test demo run-demo run

all: $(TARGET) $(DEMO)

$(TARGET): main.c $(ULA_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ main.c $(ULA_OBJ)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BIN_DIR):
	mkdir -p $@

$(OBJ_DIR):
	mkdir -p $@

run: $(TARGET)
	$(TARGET)

run-spettrum: $(TARGET)
	$(TARGET)

spettrum-version: $(TARGET)
	$(TARGET) --version

demo: $(DEMO)

$(ULA_OBJ): ula.c ula.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ ula.c

$(DEMO): demo.c $(ULA_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ demo.c $(ULA_OBJ)

run-demo: $(DEMO)
	$(DEMO)

run: run-demo

test:
	$(MAKE) -C tests run

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)
	$(MAKE) -C tests clean
