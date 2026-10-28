# hello_blink — toolchain smoke test (no WiFi, no sensors)
FQBN  := esp32:esp32:nodemcu-32s
PORT  ?= /dev/cu.usbserial-0001
BAUD  ?= 115200
BUILD_DIR := ../.build-blink

.PHONY: build upload monitor dev clean

build:
	arduino-cli compile --fqbn $(FQBN) --build-path $(BUILD_DIR) .

upload: build
	arduino-cli upload --fqbn $(FQBN) --port $(PORT) --input-dir $(BUILD_DIR) .

monitor:
	arduino-cli monitor --port $(PORT) --config baudrate=$(BAUD)

dev: upload
	arduino-cli monitor --port $(PORT) --config baudrate=$(BAUD)

clean:
	rm -rf $(BUILD_DIR)
