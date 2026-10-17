CC      ?= clang
PKGS    := libavformat libavcodec libavutil
CFLAGS  += -O2 -Wall -Wextra $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS))

udrm_remux: udrm_remux.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f udrm_remux

.PHONY: clean
