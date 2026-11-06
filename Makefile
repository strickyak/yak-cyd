PIO ?= .venv/bin/python -m platformio
PYTHON ?= python3
LAKAT ?= ./build/lakat

MKLITTLEFS ?= $(HOME)/.platformio/packages/tool-mklittlefs/mklittlefs
BOOT_APP0 ?= $(HOME)/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin

.PHONY: all push lakatos build-lakatos web-installer lakat apps demo1 demo2 build-demo1 build-demo2 monitor clean

all:
	@echo "================================================================"
	@echo "  YakFace Makefile Targets"
	@echo "================================================================"
	@echo "  make push           Build and push: lakatos & apps"
	@echo "  make lakatos        Build LCode .lapp apps, compile & upload Lakatos OS"
	@echo "  make build-lakatos  Build LCode .lapp apps & compile Lakatos OS"
	@echo "  make web-installer  Build ready-to-host WebSerial installer in build/web-installer/"
	@echo "  make lakat          Build ./build/lakat (compiler, asm, dis, emulator, uploader, tether)"
	@echo "  make apps           Compile LCode .lgolf apps into build/apps/*.lapp"
	@echo "  make demo1          Compile and upload demo1 (src/demo1/) to CYD"
	@echo "  make demo2          Compile and upload demo2 (src/demo2/) to CYD"
	@echo "  make build-demo1    Compile demo1 without uploading"
	@echo "  make build-demo2    Compile demo2 without uploading"
	@echo "  make monitor        Open 115200 baud serial monitor"
	@echo "  make clean          Clean PlatformIO and .lapp build artifacts"
	@echo "================================================================"

push: lakatos
	$(LAKAT) push build/apps/*.lapp build/apps/native.bin /sd/apps/

lakat:
	mkdir -p build build/flash build/sd build/net
	GOWORK=off go build -o build/lakat ./cmd/lakat
	GOWORK=off go build -o build/cyd-resize ./cmd/cyd-resize

apps: lakat
	mkdir -p build/apps
	$(LAKAT) compile -title "Conway's Life" -seed-var kSeedApp_life -seed-header src/lakatos/seed_apps.h -o build/apps/life.lapp apps/life/life.lgolf
	$(LAKAT) compile -title "Gallery Viewer" -seed-var kSeedApp_gallery -seed-header src/lakatos/seed_gallery.h -o build/apps/gallery.lapp apps/gallery/gallery.lgolf
	$(LAKAT) compile -title "Breakout" -o build/apps/breakout.lapp apps/breakout/breakout.lgolf
	$(LAKAT) compile -title "Space Invaders" -o build/apps/invaders.lapp apps/invaders/invaders.lgolf
	$(LAKAT) compile -title "Mandelbrot" -o build/apps/mandelbrot.lapp apps/mandelbrot/mandelbrot.lgolf
	$(LAKAT) compile -title "NOAA Weather" -o build/apps/weather.lapp apps/weather/weather.lgolf
	$(LAKAT) compile -title "Tetris" -o build/apps/tetris.lapp apps/tetris/tetris.lgolf
	$(LAKAT) compile -title "Galaga" -o build/apps/galaga.lapp apps/galaga/galaga.lgolf
	$(LAKAT) compile -title "Defender" -o build/apps/defender.lapp apps/defender/defender.lgolf
	$(LAKAT) compile -title "Bouncing Ball" -o build/apps/bounce.lapp apps/bounce/bounce.lgolf
	$(LAKAT) compile -title "Rubik's Cube" -o build/apps/rubik.lapp apps/rubik/rubik.lgolf
	$(LAKAT) compile -title "Ising Spin" -o build/apps/ising.lapp apps/ising/ising.lgolf
	$(LAKAT) compile -title "15-Puzzle" -o build/apps/puzzle15.lapp apps/puzzle15/puzzle15.lgolf
	$(LAKAT) compile -title "Dendrite DLA" -o build/apps/dendrite.lapp apps/dendrite/dendrite.lgolf
	$(LAKAT) compile -title "Paint" -o build/apps/paint.lapp apps/paint/paint.lgolf
	$(LAKAT) compile -title "Piano" -o build/apps/piano.lapp apps/piano/piano.lgolf
	$(LAKAT) compile -title "MOD Tracker" -o build/apps/modtrack.lapp apps/modtrack/modtrack.lgolf
	$(LAKAT) compile -title "MIDI Player" -o build/apps/midiplay.lapp apps/midiplay/midiplay.lgolf
	$(LAKAT) compile -title "Radio" -o build/apps/radio.lapp apps/radio/radio.lgolf
	$(LAKAT) compile -title "Ham SDR" -o build/apps/ham.lapp apps/ham/ham.lgolf
	$(LAKAT) compile -title "SmallerTalk-80" -o build/apps/smallertalk.lapp apps/smallertalk/smallertalk.lgolf
	$(LAKAT) compile -title "Wind Chimes" -o build/apps/wind.lapp apps/wind/wind.lgolf
	$(PYTHON) tools/build_lapp.py

lakatos: apps
	$(PIO) run -e lakatos -t upload

build-lakatos: apps
	$(PIO) run -e lakatos

web-installer: build-lakatos
	rm -rf build/web-installer
	mkdir -p build/web-installer build/web-installer/apps build/web-installer/fs_root/apps
	cp web-installer/index.html web-installer/manifest.json web-installer/manifest-os-only.json build/web-installer/
	cp .pio/build/lakatos/bootloader.bin .pio/build/lakatos/partitions.bin .pio/build/lakatos/firmware.bin build/web-installer/
	cp $(BOOT_APP0) build/web-installer/boot_app0.bin
	cp build/apps/*.lapp build/apps/native.bin build/web-installer/apps/
	cp build/apps/*.lapp build/apps/native.bin build/web-installer/fs_root/apps/
	$(MKLITTLEFS) -c build/web-installer/fs_root -b 4096 -p 256 -s 1769472 build/web-installer/littlefs.bin
	rm -rf build/web-installer/fs_root
	$(PYTHON) -c 'import json, os; apps = sorted(f for f in os.listdir("build/web-installer/apps") if f.endswith(".lapp")); open("build/web-installer/apps/apps.json", "w").write(json.dumps(apps, indent=2) + "\n")'
	@echo "WebSerial installer bundle ready in build/web-installer/"

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
	rm -rf build/apps build/flash build/sd build/net build/lakat build/cyd-resize
