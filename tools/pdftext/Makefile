# Copyright (c) 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: GPL-3.0-only

# Host build of the zpdf library with a small command line driver, used to
# check the parser against real documents (compare with pdftotext).
CC ?= gcc
CFLAGS ?= -O1 -g -Wall -Wextra -fsanitize=undefined -fno-omit-frame-pointer
LIB := ../../lib/zpdf
SRCS := $(wildcard $(LIB)/*.c) $(wildcard $(LIB)/fonts/*.c) pdftext.c

pdftext: $(SRCS) $(wildcard $(LIB)/*.h) $(wildcard ../../include/zpdf/*.h)
	$(CC) $(CFLAGS) -I../../include -I$(LIB) -o $@ $(SRCS) -lm

clean:
	rm -f pdftext
