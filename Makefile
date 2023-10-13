.PHONY: build clean

PROGRAM := weave

CC := gcc
INC_DIR := ext
INC_FLAGS := -I$(INC_DIR)
DEPS := $(wildcard ext/*.c)
CFLAGS := $(INC_FLAGS) -lm -DLOG_USE_COLOR

build:
	@echo "Building..."
	@$(CC) $(DEPS) $(PROGRAM).c $(CFLAGS) -o $(PROGRAM)

clean:
	@echo "Cleaning..."
	@rm -rf $(PROGRAM)
