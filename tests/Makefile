# Host unit tests: no board, no HAL. Builds the platform-independent layers (Devices/, App/)
# together with fakes and runs them natively.
#
#   make test        build + run, non-zero exit code if any test fails
#   make coverage    same with gcov instrumentation; gcovr report + threshold check
#   make clean

CC ?= gcc
FW := ../firmware

# Units under test (everything that does not include HAL).
SUT := $(FW)/Devices/mpu6050.c $(FW)/Devices/w25q64.c $(FW)/App/motion.c $(FW)/App/app.c
TESTS := test_main.c test_motion.c test_mpu6050.c test_w25q64.c test_app.c fakes.c

INCLUDES := -I. -I$(FW)/App -I$(FW)/Devices
WARN := -Wall -Wextra -Wshadow -Wpedantic -Werror
CFLAGS := -std=c11 -g -O0 $(WARN) $(INCLUDES)

# Minimum line coverage of Devices/ and App/ enforced by `make coverage` (see docs/lab2-report.md
# for the justification of the number).
COVERAGE_MIN_LINE ?= 90
COVERAGE_MIN_BRANCH ?= 70

BUILD := build

.PHONY: test coverage clean

test: $(BUILD)/test_runner
	./$(BUILD)/test_runner

$(BUILD)/test_runner: $(SUT) $(TESTS) $(wildcard *.h) $(wildcard $(FW)/Devices/*.h) $(wildcard $(FW)/App/*.h) | $(BUILD)
	$(CC) $(CFLAGS) $(SUT) $(TESTS) -o $@

$(BUILD):
	mkdir -p $(BUILD)

coverage: | $(BUILD)
	rm -rf $(BUILD)/cov && mkdir -p $(BUILD)/cov
	$(CC) $(CFLAGS) --coverage $(abspath $(SUT)) $(abspath $(TESTS)) -o $(BUILD)/cov/test_runner
	cd $(BUILD)/cov && ./test_runner
	gcovr --root .. --object-directory $(BUILD)/cov \
	      --filter '$(abspath $(FW))/(App|Devices)/' \
	      --txt --print-summary \
	      --html-details $(BUILD)/cov/index.html \
	      --txt $(BUILD)/coverage.txt
	gcovr --root .. --object-directory $(BUILD)/cov \
	      --filter '$(abspath $(FW))/(App|Devices)/' \
	      --branches --txt --print-summary > $(BUILD)/coverage-branches.txt
	gcovr --root .. --object-directory $(BUILD)/cov \
	      --filter '$(abspath $(FW))/(App|Devices)/' \
	      --fail-under-line $(COVERAGE_MIN_LINE) --fail-under-branch $(COVERAGE_MIN_BRANCH) > /dev/null

clean:
	rm -rf $(BUILD)
