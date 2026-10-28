# esp-current-sense — build/flash/monitor via arduino-cli
#
# Toolchain: arduino-cli + esp32:esp32 core (already installed on this Mac).
# Board:     NodeMCU-32S, FQBN esp32:esp32:nodemcu-32s
# Port:      override with `make upload PORT=/dev/cu.usbserial-XXXX`

FQBN      := esp32:esp32:nodemcu-32s
PORT      ?= /dev/cu.usbserial-0001
BAUD      ?= 115200
SKETCH    := firmware
BUILD_DIR := .build

.PHONY: build test scope metrics test-scope upload monitor dev clean

build:
	arduino-cli compile --fqbn $(FQBN) --build-path $(BUILD_DIR) $(SKETCH)

test:
	mkdir -p $(BUILD_DIR)/tests
	$(CXX) -std=c++11 -Wall -Wextra -Werror -pedantic tests/adc_samples_test.cpp -o $(BUILD_DIR)/tests/adc_samples_test
	$(BUILD_DIR)/tests/adc_samples_test
	$(CXX) -std=c++11 -Wall -Wextra -Werror -pedantic -Itests/host tests/power_sampler_test.cpp -o $(BUILD_DIR)/tests/power_sampler_test
	$(BUILD_DIR)/tests/power_sampler_test

scope:
	python3 tools/serve_scope.py

metrics:
	python3 tools/serve_scope.py --metrics

test-scope:
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_scope.py'
	node --test tests/scope_test.mjs tests/metrics_test.mjs

upload: build
	arduino-cli upload --fqbn $(FQBN) --port $(PORT) --input-dir $(BUILD_DIR) $(SKETCH)

monitor:
	arduino-cli monitor --port $(PORT) --config baudrate=$(BAUD)

# build + flash + tail serial output in one go
dev: upload
	arduino-cli monitor --port $(PORT) --config baudrate=$(BAUD)

clean:
	rm -rf $(BUILD_DIR)
