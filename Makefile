.PHONY: all build build-web build-server build-test test test-fast crap dev dev-logs dev-clean stop deploy clean

all: build build-web build-server

# Use Ninja if installed — significantly faster parallel builds and
# better dependency tracking than Make. Falls back to Make otherwise.
GENERATOR := $(shell command -v ninja >/dev/null 2>&1 && echo "-G Ninja")

# --- Native desktop client ---
build:
	cmake $(GENERATOR) -S . -B build
	@ln -sf build/compile_commands.json compile_commands.json
	cmake --build build --target signal --parallel

# --- Emscripten web client ---
build-web:
	emcmake cmake $(GENERATOR) -S . -B build-web -DCMAKE_BUILD_TYPE=Release -DGIT_HASH=$$(git rev-parse --short HEAD)
	cmake --build build-web --parallel

# --- Headless game server ---
build-server:
	cmake $(GENERATOR) -S . -B build
	@ln -sf build/compile_commands.json compile_commands.json
	cmake --build build --target signal_server --parallel

# --- Tests ---
# Always rebuild signal_test from current source before running, so a stale
# binary cannot hide regressions. Default to --quiet (banners + per-test
# "ok" lines suppressed; failures + summary still print). Override with
# `make test TEST_VERBOSE=1` to get the full per-test stream.
TEST_QUIET := $(if $(TEST_VERBOSE),,--quiet)

# -O2 instead of CMake's default -O0 for Debug: cuts the test suite from
# ~180s to ~56s (3.25x). All 340 tests pass identically — see PR that
# introduced this. Keep -g for usable stack traces on failure.
build-test:
	cmake $(GENERATOR) -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS_DEBUG="-O2 -g"
	@ln -sf build/compile_commands.json compile_commands.json
	cmake --build build --target signal_test --parallel

# Default `test` is serial — the suite has shared `/tmp/test_*.sav`
# paths and order-coupled global catalog state, so naive sharding races.
# See `make test-fast` for the opt-in parallel runner.
test: build-test
	./build/signal_test $(TEST_QUIET)

# Parallel sharded runner (opt-in). The harness supports --shard=K/N
# already; we just fan out across cores. Caveat: ~5 tests in
# test_save / test_construction / test_manifest are order-coupled
# through global catalog state and may flake when sharded. Use this for
# fast iteration on areas you know are independent (math, commodity,
# economy, navigation) — fall back to `make test` before pushing.
NCORES := $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
TEST_SHARDS ?= $(shell echo $$(( $(NCORES) < 8 ? $(NCORES) : 8 )))

test-fast: build-test
	@echo "[test-fast] $(TEST_SHARDS) shards in parallel — heads-up: order-coupled tests may flake; use 'make test' before pushing."
	@rm -f /tmp/signal-test-shard.*.log /tmp/signal-test-shard.*.exit
	@for i in $$(seq 0 $$(($(TEST_SHARDS) - 1))); do \
		( ./build/signal_test --shard=$$i/$(TEST_SHARDS) $(TEST_QUIET) \
			> /tmp/signal-test-shard.$$i.log 2>&1; \
		  echo $$? > /tmp/signal-test-shard.$$i.exit ) & \
	done; \
	wait; \
	fail=0; total_run=0; total_passed=0; total_failed=0; \
	for i in $$(seq 0 $$(($(TEST_SHARDS) - 1))); do \
		ec=$$(cat /tmp/signal-test-shard.$$i.exit); \
		if [ "$$ec" != "0" ]; then \
			echo ""; echo "=== shard $$i failed (exit $$ec) ==="; \
			cat /tmp/signal-test-shard.$$i.log; \
			fail=1; \
		fi; \
		line=$$(grep -E "^[0-9]+ tests run" /tmp/signal-test-shard.$$i.log | tail -1); \
		r=$$(echo $$line | awk '{print $$1}'); \
		p=$$(echo $$line | awk '{print $$4}'); \
		f=$$(echo $$line | awk '{print $$6}'); \
		total_run=$$(( total_run + $${r:-0} )); \
		total_passed=$$(( total_passed + $${p:-0} )); \
		total_failed=$$(( total_failed + $${f:-0} )); \
	done; \
	echo ""; \
	echo "$$total_run tests run, $$total_passed passed, $$total_failed failed (across $(TEST_SHARDS) shards)"; \
	exit $$fail

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
	rm -f /tmp/signal-test-shard.*.log /tmp/signal-test-shard.*.exit
