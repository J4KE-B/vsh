# ============================================================================
# vsh - Vanguard Shell
# Advanced, memory-safe Linux shell written in C
# ============================================================================

CC       := gcc
CFLAGS   := -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE \
            -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes \
            -Wmissing-prototypes -Wold-style-definition \
            -I./include
LDFLAGS  := -lm

# Source files
SRC_DIR  := src
INC_DIR  := include
BUILD_DIR:= build

SRCS     := $(wildcard $(SRC_DIR)/*.c) $(wildcard $(SRC_DIR)/builtins/*.c)
OBJS     := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
DEPS     := $(OBJS:.o=.d)

# Test files
TEST_SRCS := $(wildcard tests/*.c)
TEST_OBJS := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%.o,$(TEST_SRCS))
# Objects excluding main.o for test linking
LIB_OBJS  := $(filter-out $(BUILD_DIR)/main.o,$(OBJS))

TARGET   := vsh

# ============================================================================
# Targets
# ============================================================================

.PHONY: all release debug sanitize test clean install

all: release

release: CFLAGS += -O2 -DNDEBUG
release: $(TARGET)
	@echo "[OK] Built $(TARGET) (release)"

debug: CFLAGS += -O0 -g3 -DVSH_DEBUG
debug: TARGET := vsh_debug
debug: $(TARGET)
	@echo "[OK] Built $(TARGET) (debug)"

sanitize: CFLAGS += -O0 -g3 -DVSH_DEBUG -fsanitize=address,undefined -fno-omit-frame-pointer
sanitize: LDFLAGS += -fsanitize=address,undefined
sanitize: TARGET := vsh_debug
sanitize: $(TARGET)
	@echo "[OK] Built $(TARGET) (sanitize)"

$(TARGET): $(OBJS)
	@echo "[LD] $@"
	@$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "[CC] $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Tests
test: CFLAGS += -O0 -g3 -DVSH_DEBUG -DVSH_TEST
test: $(LIB_OBJS) $(TEST_OBJS)
	@echo "[LD] vsh_test"
	@$(CC) $(LIB_OBJS) $(TEST_OBJS) -o vsh_test $(LDFLAGS)
	@echo "[TEST] Running tests..."
	@./vsh_test

$(BUILD_DIR)/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	@echo "[CC] $<"
	@$(CC) $(CFLAGS) -I./tests -MMD -MP -c $< -o $@

clean:
	@rm -rf $(BUILD_DIR) $(TARGET) vsh_debug vsh_test
	@echo "[CLEAN] Done"

install: release
	@install -m 755 $(TARGET) /usr/local/bin/
	@echo "[INSTALL] $(TARGET) -> /usr/local/bin/"

-include $(DEPS)
