CC     = cc
CFLAGS = -std=c99 -Wall -Wextra -Wpedantic -Wno-unused-parameter \
         -g -fsanitize=address,undefined \
         -I . -I test

SRC = mcsigpack.c
OUT = out

.PHONY: all test clean

all: test

test: $(OUT)/test_mcsigpack
	./$(OUT)/test_mcsigpack

$(OUT)/test_mcsigpack: $(SRC) test/test_mcsigpack.c mcsigpack.h | $(OUT)
	$(CC) $(CFLAGS) $(SRC) test/test_mcsigpack.c -o $@

$(OUT):
	mkdir -p $(OUT)

clean:
	rm -rf $(OUT)
