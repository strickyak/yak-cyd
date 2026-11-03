PIO ?= .venv/bin/python -m platformio
PYTHON ?= python3
LAKAT ?= ./build/lakat

.PHONY: all push lakatos build-lakatos lakat apps demo1 demo2 build-demo1 build-demo2 monitor clean

all:
	@echo "================================================================"
	@echo "  Yak-CYD Makefile Targets"
	@echo "================================================================"
	@echo "  make push           Build and push: lakatos & apps"
	@echo "  make lakatos        Build LCode .lapp apps, compile & upload Lakatos OS"
	@echo "  make build-lakatos  Build LCode .lapp apps & compile Lakatos OS"
	@echo "  make lakat          Build ./build/lakat (compiler, asm, dis, emulator, uploader, tether)"
	@echo "  make apps           Compile LCode .lgolf apps into build/apps/*.lapp"
	@echo "  make demo1          Compile and upload demo1 (src/demo1/) to CYD"
	@echo "  make demo2          Compile and upload demo2 (src/demo2/) to CYD"
	@echo "  make build-demo1    Compile demo1 without uploading"
	@echo "  make build-demo2    Compile demo2 without uploading"
	@echo "  make monitor        Open 115200 baud serial monitor"
	@echo "  make clean          Clean PlatformIO and .lapp build artifacts"
	@echo "================================================================"

push: lakatos apps
	TODO

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
	$(LAKAT) compile -title "Pix-2" -o build/apps/pix-2.lapp apps/pix-2/pix-2.lgolf
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

lakatos: apps
	$(PIO) run -e lakatos -t upload

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
	rm -rf build/apps build/flash build/sd build/net build/lakat build/cyd-resize
