PIO ?= .venv/bin/python -m platformio
PYTHON ?= python3
LAKAT ?= ./build/lakat
SECRETS ?= /etc/lakatos/secrets
REQ ?= $(shell date +%s)

MKLITTLEFS ?= $(HOME)/.platformio/packages/tool-mklittlefs/mklittlefs
BOOT_APP0 ?= $(HOME)/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin

.PHONY: all push push-to-net lakatos build-lakatos web-installer lakat apps demo1 demo2 build-demo1 build-demo2 monitor cf wipe clean

all:
	@echo "================================================================"
	@echo "  YakFace Makefile Targets"
	@echo "================================================================"
	@echo "  make push           Build and push: lakatos & apps"
	@echo "  make push-to-net    Build all and push kernel, native.bin, apps, subapps to /net"
	@echo "  make lakatos        Build LCode .lapp apps, compile & upload Lakatos OS"
	@echo "  make cf             Write config from $(SECRETS) to attached CYD"
	@echo "  make build-lakatos  Build LCode .lapp apps & compile Lakatos OS"
	@echo "  make web-installer  Build ready-to-host WebSerial installer in build/web-installer/"
	@echo "  make lakat          Build ./build/lakat (compiler, asm, dis, emulator, uploader, tether)"
	@echo "  make apps           Compile LCode .lgolf apps into build/apps/*.lapp"
	@echo "  make demo1          Compile and upload demo1 (src/demo1/) to CYD"
	@echo "  make demo2          Compile and upload demo2 (src/demo2/) to CYD"
	@echo "  make build-demo1    Compile demo1 without uploading"
	@echo "  make build-demo2    Compile demo2 without uploading"
	@echo "  make monitor        Open 115200 baud serial monitor"
	@echo "  make wipe           Erase all flash (kernel, NVS config, /flash) on CYD"
	@echo "  make clean          Clean PlatformIO and .lapp build artifacts"
	@echo "================================================================"

push: lakatos
	$(LAKAT) push build/apps/*.lapp build/apps/native.bin /sd/apps/
	$(LAKAT) push build/subapps/*.sapp /sd/subapps/

push-to-net: build-lakatos
	$(LAKAT) push build/apps/lakatos.krn /net/apps/
	$(LAKAT) push build/apps/native.bin /net/apps/
	$(LAKAT) push build/apps/*.lapp /net/apps/
	$(LAKAT) push build/subapps/*.sapp /net/subapps/
push-scp-to-net: build-lakatos
	scp build/apps/lakatos.krn root@pizga.net:/home/highway/superhighway/fs/apps/
	scp build/apps/native.bin  root@pizga.net:/home/highway/superhighway/fs/apps/
	scp build/apps/*.lapp  root@pizga.net:/home/highway/superhighway/fs/apps/
	scp build/subapps/*.sapp  root@pizga.net:/home/highway/superhighway/fs/subapps/
	ssh root@pizga.net 'chown -R highway /home/highway/superhighway/fs/'

lakat:
	mkdir -p build build/flash build/sd build/net
	GOWORK=off go build -o build/lakat ./cmd/lakat
	GOWORK=off go build -o build/cyd-resize ./cmd/cyd-resize

apps: lakat
	mkdir -p build/apps build/subapps
	$(LAKAT) compile -req $(REQ) -title "Conway's Life" -o build/apps/life.lapp apps/life/life.lgolf
	$(LAKAT) compile -req $(REQ) -title "Gallery Viewer" -o build/apps/gallery.lapp apps/gallery/gallery.lgolf
	$(LAKAT) compile -req $(REQ) -title "Breakout" -o build/apps/breakout.lapp apps/breakout/breakout.lgolf
	$(LAKAT) compile -req $(REQ) -title "Space Invaders" -o build/apps/invaders.lapp apps/invaders/invaders.lgolf
	$(LAKAT) compile -req $(REQ) -title "Mandelbrot" -o build/apps/mandelbrot.lapp apps/mandelbrot/mandelbrot.lgolf
	$(LAKAT) compile -req $(REQ) -title "NOAA Weather" -o build/apps/weather.lapp apps/weather/weather.lgolf
	$(LAKAT) compile -req $(REQ) -title "Tetris" -o build/apps/tetris.lapp apps/tetris/tetris.lgolf
	$(LAKAT) compile -req $(REQ) -title "Galaga" -o build/apps/galaga.lapp apps/galaga/galaga.lgolf
	$(LAKAT) compile -req $(REQ) -title "Defender" -o build/apps/defender.lapp apps/defender/defender.lgolf
	$(LAKAT) compile -req $(REQ) -title "Bouncing Ball" -o build/apps/bounce.lapp apps/bounce/bounce.lgolf
	$(LAKAT) compile -req $(REQ) -title "Rubik's Cube" -o build/apps/rubik.lapp apps/rubik/rubik.lgolf
	$(LAKAT) compile -req $(REQ) -title "Ising Spin" -o build/apps/ising.lapp apps/ising/ising.lgolf
	$(LAKAT) compile -req $(REQ) -title "15-Puzzle" -o build/apps/puzzle15.lapp apps/puzzle15/puzzle15.lgolf
	$(LAKAT) compile -req $(REQ) -title "Dendrite DLA" -o build/apps/dendrite.lapp apps/dendrite/dendrite.lgolf
	$(LAKAT) compile -req $(REQ) -title "Paint" -o build/apps/paint.lapp apps/paint/paint.lgolf
	$(LAKAT) compile -req $(REQ) -title "Piano" -o build/apps/piano.lapp apps/piano/piano.lgolf
	$(LAKAT) compile -req $(REQ) -title "MOD Tracker" -o build/apps/modtrack.lapp apps/modtrack/modtrack.lgolf
	$(LAKAT) compile -req $(REQ) -title "MIDI Player" -o build/apps/midiplay.lapp apps/midiplay/midiplay.lgolf
	$(LAKAT) compile -req $(REQ) -title "Radio" -o build/apps/radio.lapp apps/radio/radio.lgolf
	$(LAKAT) compile -req $(REQ) -title "Ham SDR" -o build/apps/ham.lapp apps/ham/ham.lgolf
	$(LAKAT) compile -req $(REQ) -title "SmallerTalk-80" -o build/apps/smallertalk.lapp apps/smallertalk/smallertalk.lgolf
	$(LAKAT) compile -req $(REQ) -title "Wind Chimes" -o build/apps/wind.lapp apps/wind/wind.lgolf
	$(LAKAT) compile -req $(REQ) -title "3D Dice Roller" -o build/apps/dice-3d.lapp apps/dice-3d/dice-3d.lgolf
	$(LAKAT) compile -req $(REQ) -title "Croquet 3D World" -o build/apps/croquet.lapp apps/croquet/croquet.lgolf
	$(LAKAT) compile -req $(REQ) -title "basic" -o build/apps/basic.lapp apps/basic/basic.lgolf
	$(LAKAT) compile -req $(REQ) -title "logo" -o build/apps/logo.lapp apps/logo/logo.lgolf
	$(LAKAT) compile -req $(REQ) -title "chat" -o build/apps/chat.lapp apps/chat/chat.lgolf
	$(LAKAT) compile -req $(REQ) -title "calc" -o build/apps/calc.lapp apps/calc/calc.lgolf
	$(LAKAT) compile -req $(REQ) -title "graph" -o build/apps/graph.lapp apps/graph/graph.lgolf
	$(LAKAT) compile -req $(REQ) -title "ambient" -o build/apps/ambient.lapp apps/ambient/ambient.lgolf
	$(LAKAT) compile -req $(REQ) -title "World Sun Clock" -o build/apps/sunclock.lapp apps/sunclock/sunclock.lgolf
	$(LAKAT) compile -req $(REQ) -title "Color Bars" -o build/subapps/colorbars.sapp subapps/colorbars/colorbars.lgolf
	$(LAKAT) compile -req $(REQ) -title "Green Rain" -o build/subapps/green-rain.sapp subapps/green-rain/green-rain.lgolf
	$(LAKAT) compile -req $(REQ) -title "Clock" -o build/subapps/clock.sapp subapps/clock/clock.lgolf
	$(LAKAT) compile -req $(REQ) -title "LED Clock" -o build/subapps/led-clock.sapp subapps/led-clock/led-clock.lgolf
	$(LAKAT) compile -req $(REQ) -title "Life 4x4" -o build/subapps/life44.sapp subapps/life44/life44.lgolf
	$(LAKAT) compile -req $(REQ) -title "Ants" -o build/subapps/ants.sapp subapps/ants/ants.lgolf
	$(LAKAT) compile -req $(REQ) -title "Color Slice" -o build/subapps/color-slice.sapp subapps/color-slice/color-slice.lgolf
	$(LAKAT) compile -req $(REQ) -title "Clear 1" -o build/subapps/clear1.sapp subapps/clear1/clear1.lgolf
	$(LAKAT) compile -req $(REQ) -title "Clear 2" -o build/subapps/clear2.sapp subapps/clear2/clear2.lgolf
	$(LAKAT) compile -req $(REQ) -title "Clear 3" -o build/subapps/clear3.sapp subapps/clear3/clear3.lgolf
	$(LAKAT) compile -req $(REQ) -title "Clear 4" -o build/subapps/clear4.sapp subapps/clear4/clear4.lgolf
	$(LAKAT) compile -req $(REQ) -title "Clear 5" -o build/subapps/clear5.sapp subapps/clear5/clear5.lgolf
	$(LAKAT) compile -req $(REQ) -title "Clear 6" -o build/subapps/clear6.sapp subapps/clear6/clear6.lgolf
	$(LAKAT) compile -req $(REQ) -title "Clear 7" -o build/subapps/clear7.sapp subapps/clear7/clear7.lgolf
	$(LAKAT) compile -req $(REQ) -title "Clear 8" -o build/subapps/clear8.sapp subapps/clear8/clear8.lgolf
	$(LAKAT) compile -req $(REQ) -title "Clear 9" -o build/subapps/clear9.sapp subapps/clear9/clear9.lgolf
	$(LAKAT) compile -req $(REQ) -title "Clear 10" -o build/subapps/clear10.sapp subapps/clear10/clear10.lgolf
	$(LAKAT) compile -req $(REQ) -title "Clear 11" -o build/subapps/clear11.sapp subapps/clear11/clear11.lgolf
	$(LAKAT) compile -req $(REQ) -title "Clear 12" -o build/subapps/clear12.sapp subapps/clear12/clear12.lgolf
	$(PYTHON) tools/build_lapp.py --req $(REQ)

lakatos: apps
	$(PYTHON) tools/gen_version.py
	$(PIO) run -e updater
	$(PIO) run -e lakatos -t upload
	$(PYTHON) tools/pack_krn.py

build-lakatos: apps
	$(PYTHON) tools/gen_version.py
	$(PIO) run -e updater
	$(PIO) run -e lakatos
	$(PYTHON) tools/pack_krn.py

web-installer: build-lakatos
	rm -rf build/web-installer
	mkdir -p build/web-installer build/web-installer/fs_root/apps
	cp web-installer/index.html web-installer/manifest.json build/web-installer/
	cp .pio/build/lakatos/bootloader.bin .pio/build/lakatos/partitions.bin .pio/build/lakatos/firmware.bin build/web-installer/
	cp .pio/build/updater/firmware.bin build/web-installer/updater.bin
	cp $(BOOT_APP0) build/web-installer/boot_app0.bin
	$(PYTHON) tools/gen_nvs.py $(SECRETS) build/web-installer/nvs.bin
	$(MKLITTLEFS) -c build/web-installer/fs_root -b 4096 -p 256 -s 1769472 build/web-installer/littlefs.bin
	rm -rf build/web-installer/fs_root
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

cf: lakat
	$(LAKAT) tether -mount="" -drain=2s < $(SECRETS)

wipe:
	$(PIO) run -e lakatos -t erase

build/superhighway-server:
	go build -o build/superhighway-server  ../superhighway/cmd/superhighway-server/

clean:
	$(PIO) run -t clean
	rm -rf build/apps build/subapps build/flash build/sd build/net build/lakat build/cyd-resize
