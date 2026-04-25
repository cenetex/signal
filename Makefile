.PHONY: all build build-web build-server build-test test crap dev dev-logs dev-clean stop deploy clean

all: build build-web build-server

# --- Native desktop client ---
build:
	cmake -S . -B build
	cmake --build build --target signal

# --- Emscripten web client ---
build-web:
	emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release -DGIT_HASH=$$(git rev-parse --short HEAD)
	cmake --build build-web

# --- Headless game server ---
build-server:
	cmake -S . -B build
	cmake --build build --target signal_server

# --- Tests ---
# Always rebuild signal_test from current source before running, so a stale
# binary cannot hide regressions. Default to --quiet (banners + per-test
# "ok" lines suppressed; failures + summary still print). Override with
# `make test TEST_VERBOSE=1` to get the full per-test stream.
TEST_QUIET := $(if $(TEST_VERBOSE),,--quiet)

build-test:
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
	cmake --build build --target signal_test

test: build-test
	./build/signal_test $(TEST_QUIET)

# --- CRAP (Change Risk Anti-Patterns): complexity * (1 - coverage) ---
# Rebuilds signal_test with --coverage, runs it, then joins gcovr line
# coverage with lizard per-function complexity to score each function.
# Vendored code (mongoose, stb_image, pl_mpeg, minimp3) is excluded on
# both sides — we aren't going to fix it, so it shouldn't pollute the
# report. Requires: lizard, gcovr (pip install lizard gcovr).
crap:
	cmake -S . -B build-coverage -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS_ONLY=ON \
		-DCMAKE_C_FLAGS="--coverage -O0 -g" \
		-DCMAKE_EXE_LINKER_FLAGS="--coverage"
	cmake --build build-coverage --target signal_test
	ulimit -s 16384 && ./build-coverage/signal_test --quiet
	gcovr -r . --json coverage.json --gcov-ignore-parse-errors \
		--filter 'server/.*' --filter 'src/.*' --filter 'shared/.*' \
		--exclude 'server/mongoose\..*' \
		--exclude 'src/stb_image\.h' \
		--exclude 'src/pl_mpeg\.h' \
		--exclude 'src/minimp3\.h' \
		build-coverage
	python3 scripts/crap.py --coverage coverage.json \
		--paths server src shared --json-out crap.json

# --- Local dev = docker compose (single source of truth) ---
# One canonical local path. The container's entrypoint cd's into
# /app/data (bind-mounted from ./data) before launching the server,
# so all persistence stays isolated from the working tree. Same
# binary as production (alpine static build, identical CMake flags).
#
# For client-only iteration (HUD, input, render — anything that
# doesn't need a server) use the offline native build instead:
#   make build && ./build/signal
# That path uses the embedded singleplayer server in src/local_server.c.
dev:
	@mkdir -p data
	docker compose up --build -d
	@echo ""
	@echo "  Web:     http://localhost:8080/signal.html?server=ws://localhost:9091/ws"
	@echo "  Server:  ws://localhost:9091/ws"
	@echo "  Logs:    make dev-logs"
	@echo "  Stop:    make stop  (or  make dev-clean  to wipe state)"

dev-logs:
	docker compose logs -f signal

stop:
	docker compose down
	@echo "Stopped."

# Wipe persisted state. Removes the bind-mounted data dir entirely;
# next 'make dev' starts from a fresh world.
dev-clean: stop
	rm -rf data
	@echo "Persisted state wiped."

# --- Deploy (triggers CI via push) ---
deploy:
	git push origin main

clean:
	rm -rf build build-web build-test build-coverage coverage.json crap.json
