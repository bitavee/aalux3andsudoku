.PHONY: emulator sim sim-build sim-clean fs_link help

SDCARD  := sdcard
SIM_BIN := .pio/build/simulator/program

help:
	@echo "AALU Makefile targets:"
	@echo "  make emulator    Build, mount ./$(SDCARD)/ as the SD card, launch the simulator"
	@echo "  make sim         Same as make emulator"
	@echo "  make sim-build   Build only — produces $(SIM_BIN)"
	@echo "  make sim-clean   Wipe simulator build cache and on-disk SD-card cache"
	@echo ""
	@echo "Prereq:  brew install sdl2"
	@echo "Books:   drop EPUBs into ./$(SDCARD)/ (gitignored)"

# Headline one-shot: compile, ensure sdcard/ exists, mount it via the fs_
# symlink, then launch.
emulator: sim
sim: sim-build | $(SDCARD) fs_link
	@echo "Launching emulator (SD card: ./$(SDCARD)/) ..."
	@./$(SIM_BIN)

sim-build:
	pio run -e simulator

$(SDCARD):
	@mkdir -p $@
	@echo "Created ./$@/ — drop EPUBs here (gitignored)."

# The crosspoint-simulator hardcodes its sandbox path to ./fs_/ (see its
# HalStorage.cpp). We expose the more intuitive ./sdcard/ to the user and
# keep fs_ as a symlink. On first run, if a real fs_/ directory exists from
# an older sim run, migrate its contents into sdcard/ before replacing.
fs_link: $(SDCARD)
	@if [ -L fs_ ]; then \
	  :; \
	elif [ -d fs_ ]; then \
	  echo "Migrating ./fs_/ → ./$(SDCARD)/ (one-time) ..."; \
	  cp -R fs_/. $(SDCARD)/; \
	  rm -rf fs_; \
	  ln -s $(SDCARD) fs_; \
	elif [ -e fs_ ]; then \
	  echo "ERROR: ./fs_ exists and is not a directory or symlink — refusing to clobber."; \
	  exit 1; \
	else \
	  ln -s $(SDCARD) fs_; \
	fi

sim-clean:
	rm -rf .pio/build/simulator .pio/libdeps/simulator $(SDCARD)/.crosspoint
