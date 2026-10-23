IMAGE ?= stm32-build
DOCKER_RUN = docker run --rm -v $(CURDIR):/workspace -w /workspace \
	--user $(shell id -u):$(shell id -g) $(IMAGE)

.PHONY: image build clean flash monitor test coverage check check-deps check-pinout \
	check-deps-selftest chips decode measure figures all-checks

image:
	docker build -t $(IMAGE) docker/

# Firmware: CubeMX Makefile + our layers (firmware/Makefile.app), -Wall -Wextra -Werror on our code.
build:
	$(DOCKER_RUN) make -C firmware -f Makefile.app all

clean:
	$(DOCKER_RUN) make -C firmware -f Makefile.app clean
	$(DOCKER_RUN) make -C tests clean

# Form B (Wokwi): "flashing" = restarting the simulator with the new ELF.
flash: build
	@echo "ELF ready: firmware/build/firmware.elf -> VS Code: F1 -> Wokwi: Start Simulator"

monitor:
	@echo "UART output is shown in the Wokwi terminal in VS Code (USART2, 115200 8N1)"

# Host unit tests: no board needed.
test:
	$(DOCKER_RUN) make -C tests test

# Line/branch coverage of Devices/ and App/ (report in tests/build/coverage).
coverage:
	$(DOCKER_RUN) make -C tests coverage

# Layering rule (App/Devices must not see HAL) and pinout consistency.
check-deps:
	$(DOCKER_RUN) python3 tools/check_deps.py

check-deps-selftest:
	$(DOCKER_RUN) python3 tools/check_deps.py --selftest

check-pinout:
	$(DOCKER_RUN) python3 tools/check_pinout.py

check: check-deps check-deps-selftest check-pinout

# Wokwi custom chip (SPI flash model) -> chips/w25q64/w25q64.chip.wasm
chips:
	$(DOCKER_RUN) make -C chips/w25q64

# Decode every capture in docs/captures with sigrok-cli (text output next to the capture).
decode:
	$(DOCKER_RUN) python3 tools/decode_captures.py

# Numbers quoted in docs/captures/analysis.md (VCD -> docs/captures/measurements.txt).
measure:
	$(DOCKER_RUN) sh -c 'python3 tools/measure.py > docs/captures/measurements.txt'

# Annotated waveform figures (needs matplotlib on the host: pip install matplotlib).
figures:
	python3 tools/plot_captures.py

all-checks: build test check
