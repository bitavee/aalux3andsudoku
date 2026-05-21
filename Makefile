.PHONY: emulator emulator-detached emulator-stop emulator-logs emulator-clean emulator-rebuild help

EMULATOR_DIR := emulator
SDCARD_DIR   := $(EMULATOR_DIR)/sdcard
COMPOSE      := docker compose

help:
	@echo "AALU Makefile targets:"
	@echo "  make emulator           Build + run the browser emulator (foreground, Ctrl-C to stop)"
	@echo "  make emulator-detached  Same, but run in the background"
	@echo "  make emulator-logs      Tail emulator logs (use after -detached)"
	@echo "  make emulator-stop      Stop the emulator container"
	@echo "  make emulator-rebuild   Force a clean rebuild of the emulator image"
	@echo "  make emulator-clean     Stop + remove containers, volumes, build cache"
	@echo ""
	@echo "Open http://localhost:8080 once it's up. Drop EPUBs into $(SDCARD_DIR)/."

emulator: $(SDCARD_DIR)
	@cd $(EMULATOR_DIR) && $(COMPOSE) up --build

emulator-detached: $(SDCARD_DIR)
	@cd $(EMULATOR_DIR) && $(COMPOSE) up -d --build
	@echo ""
	@echo "Emulator running at http://localhost:8080"
	@echo "Logs:   make emulator-logs"
	@echo "Stop:   make emulator-stop"

emulator-stop:
	@cd $(EMULATOR_DIR) && $(COMPOSE) down

emulator-logs:
	@cd $(EMULATOR_DIR) && $(COMPOSE) logs -f

emulator-rebuild: $(SDCARD_DIR)
	@cd $(EMULATOR_DIR) && $(COMPOSE) build --no-cache
	@cd $(EMULATOR_DIR) && $(COMPOSE) up

emulator-clean:
	@cd $(EMULATOR_DIR) && $(COMPOSE) down -v --remove-orphans

$(SDCARD_DIR):
	@mkdir -p $(SDCARD_DIR)
	@echo "Created $(SDCARD_DIR)/ — drop EPUBs here. (gitignored)"
