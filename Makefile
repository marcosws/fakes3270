CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -Wextra -O2 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
MINGW   ?= x86_64-w64-mingw32-gcc

all: linux

linux:
	mkdir -p bin/linux-x64
	$(CC) $(CFLAGS) -o bin/linux-x64/fakes3270 src/fakes3270.c

windows:
	mkdir -p bin/windows-x64
	$(MINGW) $(CFLAGS) -static -o bin/windows-x64/fakes3270.exe src/fakes3270.c

test: linux
	bash tests/smoke.sh bin/linux-x64/fakes3270
	python3 tests/emulator_contract.py bin/linux-x64/fakes3270

clean:
	rm -rf bin
