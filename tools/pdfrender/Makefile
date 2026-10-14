# Copyright (c) 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: GPL-3.0-only

# Host build of the renderer with a command line driver, for comparing
# pages against pdftoppm.
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -fsanitize=undefined -fno-omit-frame-pointer
LIB := ../../lib/zpdf
SRCS := $(wildcard $(LIB)/*.c) $(wildcard $(LIB)/fonts/*.c) pdfrender.c

pdfrender: $(SRCS) $(wildcard $(LIB)/*.h) $(wildcard ../../include/zpdf/*.h)
	$(CC) $(CFLAGS) -I../../include -I$(LIB) -o $@ $(SRCS) -lm

clean:
	rm -f pdfrender
