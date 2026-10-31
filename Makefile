PIO ?= .venv/bin/pio

.PHONY: all demo1 demo2 build-demo1 build-demo2 monitor clean

all:
	@echo "================================================================"
	@echo "  CYD-One Makefile Targets"
	@echo "================================================================"
	@echo "  make demo1        Compile and upload demo1 (src/demo1/) to CYD"
	@echo "  make demo2        Compile and upload demo2 (src/demo2/) to CYD"
	@echo "  make build-demo1  Compile demo1 without uploading"
	@echo "  make build-demo2  Compile demo2 without uploading"
	@echo "  make monitor      Open 115200 baud serial monitor"
	@echo "  make clean        Clean PlatformIO build artifacts"
	@echo "================================================================"

demo1:
	$(PIO) run -e demo1 -t upload

demo2:
	$(PIO) run -e demo2 -t upload

build-demo1:
	$(PIO) run -e demo1

build-demo2:
	$(PIO) run -e demo2

monitor:
	$(PIO) device monitor -b 115200

clean:
	$(PIO) run -t clean
