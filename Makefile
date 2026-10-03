IMAGE ?= stm32-build

.PHONY: image build clean flash monitor

image:
	docker build -t $(IMAGE) docker/

build:
	docker run --rm -v $(CURDIR):/workspace -w /workspace \
		--user $(shell id -u):$(shell id -g) $(IMAGE) \
		make -C firmware all

clean:
	docker run --rm -v $(CURDIR):/workspace -w /workspace \
		--user $(shell id -u):$(shell id -g) $(IMAGE) \
		make -C firmware clean

# Form B (Wokwi): "flashing" = restarting the simulator with the new ELF.
flash: build
	@echo "ELF ready: firmware/build/firmware.elf -> VS Code: F1 -> Wokwi: Start Simulator"

monitor:
	@echo "UART output is shown in the Wokwi terminal in VS Code (USART2, 115200 8N1)"
