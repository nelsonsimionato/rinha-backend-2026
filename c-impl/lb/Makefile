CC      := gcc
CFLAGS  := -O3 -Wall -Wextra -fno-strict-aliasing -DNDEBUG
LDFLAGS :=

lb: lb.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@ls -la $@

clean:
	rm -f lb
