.PHONY: build clean

PROGRAM := arachne

CC := gcc
INC_DIR := extern
INC_FLAGS := -I$(INC_DIR)
DEPS := $(wildcard extern/*.c)
CFLAGS := $(INC_FLAGS) -lm -DLOG_USE_COLOR

build:
	@echo "Building..."
	@$(CC) $(DEPS) $(PROGRAM).c $(CFLAGS) -o $(PROGRAM)

cross:
	@echo "Cross compiling via Zig..."
	@zig \
		build-exe \
		$(DEPS) \
		$(PROGRAM).c \
		$(CFLAGS) \
		$(INC_FLAGS) \
		--library c \
		--name $(PROGRAM) \
		-target x86_64-linux

clean:
	@echo "Cleaning..."
	@rm -rf $(PROGRAM)
