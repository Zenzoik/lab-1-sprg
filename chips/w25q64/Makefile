# Builds the Wokwi custom chip (WebAssembly). Needs clang + lld
# (included in the project's Docker image: `make chips` from the repo root).
TARGET := w25q64.chip.wasm

.PHONY: all clean
all: $(TARGET)

$(TARGET): main.c wokwi-api.h
	clang --target=wasm32-unknown-wasi -O2 -ffreestanding -fno-builtin -nostdlib \
		-Wall -Wextra -Werror -Wno-unused-function \
		-Wl,--import-memory -Wl,--export-table -Wl,--no-entry -Wl,-z,stack-size=8192 \
		-o $@ main.c

clean:
	rm -f $(TARGET)
