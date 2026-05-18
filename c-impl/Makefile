CC      := gcc
CFLAGS  := -O3 -mavx2 -march=haswell -mtune=haswell -flto \
           -fno-plt -fno-asynchronous-unwind-tables -DNDEBUG \
           -Wall -Wextra -fno-strict-aliasing \
           -D_GNU_SOURCE \
           -Isrc
LDFLAGS := -static -flto -lm

SRC_C   := $(wildcard src/*.c)
SRC_S   := $(wildcard src/*.s)
OBJ     := $(SRC_C:.c=.o) $(SRC_S:.s=.o)

TEST_C  := $(wildcard tests/*.c)
TEST_BIN := $(TEST_C:.c=)

.PHONY: all clean test

all: api

api: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@which strip >/dev/null 2>&1 && strip --strip-all $@ || true
	@ls -la $@

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

src/%.o: src/%.s
	$(CC) $(CFLAGS) -c -o $@ $<

# Tests link against src objects except main.o (avoid duplicate main symbol).
NONMAIN_OBJ := $(filter-out src/main.o, $(OBJ))

tests/%: tests/%.c $(NONMAIN_OBJ)
	$(CC) $(CFLAGS) -o $@ $< $(NONMAIN_OBJ) $(LDFLAGS)

test: $(TEST_BIN)
	@for t in $(TEST_BIN); do echo "=== $$t ==="; ./$$t || exit 1; done
	@echo "All tests passed."

clean:
	rm -f src/*.o tests/*.o $(TEST_BIN) api
