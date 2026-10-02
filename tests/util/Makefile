
CC=gcc

INCS=-I. -I../../util
CFLAGS=-Wall -Wextra -g
LDFLAGS=

BIN=bitmap_test
SRC=test_bitmap.c ../../util/bitmap.c
HDRS=../../util/bitmap.h

CRC_BIN=crc32c_test
CRC_SRC=test_crc32c.c ../../util/crc32c.c
CRC_HDRS=../../util/crc32c.h

$(BIN): $(SRC) $(HDRS)
	@echo 'Building program: $@'
	@$(CC) $(CFLAGS) $(INCS) $(SRC) -o $@ $(LDFLAGS)

$(CRC_BIN): $(CRC_SRC) $(CRC_HDRS)
	@echo 'Building program: $@'
	@$(CC) $(CFLAGS) -O2 $(INCS) $(CRC_SRC) -o $@ $(LDFLAGS) -lpthread

all: $(BIN) $(CRC_BIN)

test: $(BIN) $(CRC_BIN)
	@./$(BIN)
	@./$(CRC_BIN)

clean:
	@rm -rf $(BIN) $(CRC_BIN)

.PHONY: all test clean
