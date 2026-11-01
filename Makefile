PIO ?= .venv/bin/python -m platformio
PYTHON ?= python3
LCODE ?= ./bin/lcode

.PHONY: all lakatos os build-lakatos lcode-tool apps demo1 demo2 build-demo1 build-demo2 monitor clean

all:
	@echo "================================================================"
	@echo "  Yak-CYD Makefile Targets"
	@echo "================================================================"
	@echo "  make lakatos        Build LCode .lapp apps, compile & upload Lakatos OS"
	@echo "  make os             Alias for 'make lakatos'"
	@echo "  make build-lakatos  Build LCode .lapp apps & compile Lakatos OS"
	@echo "  make lcode-tool     Build ./bin/lcode Go compiler/asm/dis/emulator/uploader"
	@echo "  make apps           Compile LCode .lgolf apps into build/apps/*.lapp"
	@echo "  make demo1          Compile and upload demo1 (src/demo1/) to CYD"
	@echo "  make demo2          Compile and upload demo2 (src/demo2/) to CYD"
	@echo "  make build-demo1    Compile demo1 without uploading"
	@echo "  make build-demo2    Compile demo2 without uploading"
	@echo "  make monitor        Open 115200 baud serial monitor"
	@echo "  make clean          Clean PlatformIO and .lapp build artifacts"
	@echo "================================================================"

lcode-tool:
	mkdir -p bin
	cd tools/lcode && GOWORK=off go build -o ../../bin/lcode ./cmd/lcode

apps: lcode-tool
	mkdir -p build/apps
	$(LCODE) compile -title "Conway's Life" -seed-var kSeedApp_life -seed-header src/lakatos/seed_apps.h -o build/apps/life.lapp apps/life/life.lgolf
	$(LCODE) compile -title "Gallery Viewer" -seed-var kSeedApp_gallery -seed-header src/lakatos/seed_gallery.h -o build/apps/gallery.lapp apps/gallery/gallery.lgolf

lakatos: apps
	$(PIO) run -e lakatos -t upload

os: lakatos

build-lakatos: apps
	$(PIO) run -e lakatos

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
	rm -rf build/apps
