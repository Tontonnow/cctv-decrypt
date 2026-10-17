CC      ?= clang
PKGS    := libavformat libavcodec libavutil

ifeq ($(OS),Windows_NT)
EXE     := .exe
else
EXE     :=
endif

TARGET  ?= udrm_remux$(EXE)
CFLAGS  += -O2 -Wall -Wextra $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS))

all: $(TARGET)

$(TARGET): udrm_remux.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f udrm_remux udrm_remux.exe

.PHONY: all clean
