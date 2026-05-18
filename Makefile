.PHONY: all build build-web build-server build-test build-flight-trace flight-trace test test-serial test-fast test-soak test-all smoke smoke-latency smoke-ack-lag cppcheck crap profile-machine latency-proxy latency-proxy-high latency-proxy-ack-lag dev dev-logs dev-clean stop deploy clean install-hooks

all: build build-web build-server

# install-hooks - install small wrappers in the shared git hooks dir.
#
# Linked worktrees share one common .git/hooks directory. A symlink from
# that common directory back to one checkout can run stale hook code when
# pushing from another worktree, so wrappers resolve the active worktree
# at runtime and then exec its tracked script under scripts/git-hooks/.
install-hooks:
	@root=$$(git rev-parse --show-toplevel); \
	hooks_dir=$$(git rev-parse --git-common-dir)/hooks; \
	mkdir -p "$$hooks_dir"; \
	for f in "$$root"/scripts/git-hooks/*; do \
		name=$$(basename "$$f"); \
		target="$$hooks_dir/$$name"; \
		rm -f "$$target"; \
		{ \
			printf '%s\n' '#!/bin/sh'; \
			printf '%s\n' 'root=$$(git rev-parse --show-toplevel) || exit 0'; \
			printf '%s\n' 'exec "$$root/scripts/git-hooks/'"$$name"'" "$$@"'; \
		} > "$$target"; \
		chmod +x "$$target"; \
		echo "  hook: $$name -> scripts/git-hooks/$$name"; \
	done

# Use Ninja if installed — significantly faster parallel builds and
# better dependency tracking than Make. Falls back to Make otherwise.
GENERATOR := $(shell command -v ninja >/dev/null 2>&1 && echo "-G Ninja")
BUILD_TYPE ?= RelWithDebInfo
GIT_HASH ?= $(shell git rev-parse --short HEAD 2>/dev/null || echo dev)

# --- Native desktop client ---
build:
	cmake $(GENERATOR) -S . -B build -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DGIT_HASH=$(GIT_HASH)
	@ln -sf build/compile_commands.json compile_commands.json
	cmake --build build --target signal --parallel

# --- Emscripten web client ---
build-web:
	emcmake cmake $(GENERATOR) -S . -B build-web -DCMAKE_BUILD_TYPE=Release -DGIT_HASH=$$(git rev-parse --short HEAD)
	cmake --build build-web --parallel

# --- Headless game server ---
build-server:
	cmake $(GENERATOR) -S . -B build -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DGIT_HASH=$(GIT_HASH)
	@ln -sf build/compile_commands.json compile_commands.json
	cmake --build build --target signal_server --parallel

# --- Offline WASD flight-brain training traces ---
FLIGHT_TRACE_EPISODES ?= 1000
FLIGHT_TRACE_TICKS ?= 600
FLIGHT_TRACE_SEED ?= 2037
FLIGHT_TRACE_SHARD ?= 0/1
FLIGHT_TRACE_FORMAT ?= csv
FLIGHT_TRACE_OUT ?= /tmp/signal-flight-trace.csv

build-flight-trace:
	cmake $(GENERATOR) -S . -B build -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DGIT_HASH=$(GIT_HASH)
	@ln -sf build/compile_commands.json compile_commands.json
	cmake --build build --target flight_trace --parallel

flight-trace: build-flight-trace
	./build/flight_trace \
		--episodes $(FLIGHT_TRACE_EPISODES) \
		--ticks $(FLIGHT_TRACE_TICKS) \
		--seed $(FLIGHT_TRACE_SEED) \
		--shard $(FLIGHT_TRACE_SHARD) \
		--format $(FLIGHT_TRACE_FORMAT) \
		--out $(FLIGHT_TRACE_OUT)

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
	cmake $(GENERATOR) -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS_DEBUG="-O2 -g" -DGIT_HASH=$(GIT_HASH)
	@ln -sf build/compile_commands.json compile_commands.json
	cmake --build build --target signal_test --parallel
	# test_signal_verify shells out to signal_verify for CLI-only
	# invariant coverage, so rebuild it here too; otherwise a stale
	# tool binary can make the sharded suite fail or pass incorrectly.
	cmake --build build --target signal_verify --parallel
	# Compile-check the native client too. signal_test doesn't pull in
	# net_sync.c / world_draw.c / hud.c (client-only), so a struct
	# rename that breaks the wire-decode side won't fail signal_test
	# alone. The user has hit this exact gap (npc_ship_t.pos → .ship.pos
	# silently broke autopilot in deployed wasm). Keep this fast: it's
	# an incremental build of the same -O2/-g object cache, so unchanged
	# files don't re-link.
	cmake --build build --target signal --parallel

# Number of shards for the parallel test runner. Defaults to min(8, ncores).
NCORES := $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
TEST_SHARDS ?= $(shell echo $$(( $(NCORES) < 8 ? $(NCORES) : 8 )))

# Reusable parallel-shard runner. Caller passes RUN_FLAGS for the test
# binary (e.g. --no-soak / --soak-only); the runner handles sharding,
# wait, and aggregate reporting.
define RUN_PARALLEL_TESTS
	@rm -f /tmp/signal-test-shard.*.log /tmp/signal-test-shard.*.exit
	@for i in $$(seq 0 $$(($(TEST_SHARDS) - 1))); do \
		( ./build/signal_test --shard=$$i/$(TEST_SHARDS) $(1) $(TEST_QUIET) \
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
endef

# `make test` runs the fast tests sharded across cores. Same coverage
# as the old serial path minus RUN_SOAK, ~4× faster wall-clock (~3-5s
# vs ~60s on a 14-core box). Soak tests (autopilot scenarios, e2e
# contract lifecycle, multi-thousand-tick conservation) are skipped
# here and live in `make test-soak`.
#
# Other targets:
#   make test-soak    Only RUN_SOAK tests, sharded. ~10-15s.
#   make test-all     Both fast + soak, sharded. The full suite.
#   make test-serial  Single-process, in-order, fast tests only —
#                     for debugging a shard-related flake.
#   make test-fast    Alias for `make test` (backward compat).
test test-fast: build-test
	$(call RUN_PARALLEL_TESTS,--no-soak)

test-soak: build-test
	$(call RUN_PARALLEL_TESTS,--soak-only)

test-all: build-test
	$(call RUN_PARALLEL_TESTS,--soak)

test-serial: build-test
	./build/signal_test --no-soak $(TEST_QUIET)

# Static analysis for owned C sources. Avoid --project=compile_commands.json
# here: it pulls in test fixtures and single-header vendor libraries whose
# allocation-model warnings swamp actionable project-code findings.
CPPCHECK ?= cppcheck
CPPCHECK_SOURCES := server shared client tools/signal_verify.c tools/flight_trace.c

cppcheck:
	$(CPPCHECK) --quiet --std=c11 --enable=warning,portability --error-exitcode=1 \
		--suppress=missingIncludeSystem \
		--suppress='*:client/pl_mpeg.h' \
		--suppress='*:client/minimp3.h' \
		--suppress='*:client/stb_image.h' \
		--suppress='*:server/mongoose.h' \
		--suppress='*:server/mongoose.c' \
		--platform=unix64 \
		-DMG_ARCH=MG_ARCH_UNIX \
		-DMG_ENABLE_LOG=0 \
		-DSOKOL_METAL=1 \
		-DGIT_HASH=\"cppcheck\" \
		-Iclient -Ishared -Iserver \
		-i server/mongoose.c \
		$(CPPCHECK_SOURCES)

# Browser smoke: builds the WASM client, serves build-web locally, and
# drives the canvas through the same Playwright smoke used after deploy.
smoke: build-web
	npm run smoke

SMOKE_LATENCY_URL ?= http://localhost:8080/play.html?server=ws://127.0.0.1:19091/ws

smoke-latency:
	SMOKE_URL="$(SMOKE_LATENCY_URL)" SMOKE_LATENCY_ASSERT=1 npx playwright test tests/browser-smoke.spec.ts --project=chromium --grep "high-latency"

smoke-ack-lag:
	SMOKE_URL="$(SMOKE_LATENCY_URL)" SMOKE_ACK_LAG_ASSERT=1 npx playwright test tests/browser-smoke.spec.ts --project=chromium --grep "low-ping high-ack"

# --- CRAP (Change Risk Anti-Patterns): complexity * (1 - coverage) ---
# Rebuilds signal_test with --coverage, runs the fast/non-soak tests,
# then joins gcovr line coverage with lizard per-function complexity to
# score each function. Long-horizon sim coverage belongs in test-soak or
# a scheduled coverage pass; duplicating it here is especially expensive
# under --coverage -O0.
# Vendored code (mongoose, stb_image, pl_mpeg, minimp3) is excluded on
# both sides — we aren't going to fix it, so it shouldn't pollute the
# report. Requires: lizard, gcovr (pip install lizard gcovr).
CRAP_TESTED_PATHS := server/game_sim.c server/sim_ai.c server/sim_autopilot.c \
	server/sim_flight.c server/sim_nav.c server/sim_save.c \
	server/sim_catalog.c server/sim_asteroid.c server/sim_physics.c \
	server/sim_production.c server/sim_construction.c \
	shared/commodity.c shared/manifest.c shared/ship.c shared/economy.c \
	shared/asteroid.c shared/rng.c shared

crap:
	cmake -S . -B build-coverage -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS_ONLY=ON \
		-DCMAKE_C_FLAGS="--coverage -O0 -g" \
		-DCMAKE_EXE_LINKER_FLAGS="--coverage"
	cmake --build build-coverage --target signal_test
	find build-coverage -name '*.gcda' -delete
	ulimit -s 16384 && ./build-coverage/signal_test --quiet --no-soak
	gcovr -r . --json coverage.json --gcov-ignore-parse-errors \
		--filter 'server/.*' --filter 'client/.*' --filter 'shared/.*' \
		--exclude 'server/mongoose\..*' \
		--exclude 'client/stb_image\.h' \
		--exclude 'client/pl_mpeg\.h' \
		--exclude 'client/minimp3\.h' \
		build-coverage
	python3 scripts/crap.py --coverage coverage.json \
		--paths $(CRAP_TESTED_PATHS) \
		--top 30 --threshold 25 --fail-on-exceed \
		--json-out crap.json

profile-machine:
	tools/profile_machine.sh

LATENCY_LISTEN ?= 127.0.0.1:19091
LATENCY_UPSTREAM ?= ws://127.0.0.1:9091/ws
LATENCY_CLIENT_MS ?= 250
LATENCY_SERVER_MS ?= 250
LATENCY_WORLD_PLAYERS_MS ?= 0
LATENCY_JITTER_MS ?= 80

latency-proxy:
	node scripts/ws-latency-proxy.mjs \
		--listen=$(LATENCY_LISTEN) \
		--upstream=$(LATENCY_UPSTREAM) \
		--client-ms=$(LATENCY_CLIENT_MS) \
		--server-ms=$(LATENCY_SERVER_MS) \
		--server-world-players-ms=$(LATENCY_WORLD_PLAYERS_MS) \
		--jitter-ms=$(LATENCY_JITTER_MS)

latency-proxy-high:
	$(MAKE) latency-proxy LATENCY_CLIENT_MS=450 LATENCY_SERVER_MS=450 LATENCY_JITTER_MS=150

latency-proxy-ack-lag:
	$(MAKE) latency-proxy LATENCY_CLIENT_MS=20 LATENCY_SERVER_MS=20 LATENCY_WORLD_PLAYERS_MS=550 LATENCY_JITTER_MS=10

# --- Local dev = docker compose (single source of truth) ---
# One canonical local path. The container's entrypoint cd's into
# /app/data (bind-mounted from ./data) before launching the server,
# so all persistence stays isolated from the working tree. Same
# binary as production (alpine static build, identical CMake flags).
#
# For client-only iteration (HUD, input, render — anything that
# doesn't need a server) use the offline native build instead:
#   make build && ./build/signal
# That path uses the embedded singleplayer server in client/local_server.c.
dev:
	@mkdir -p data
	docker compose up --build -d
	@echo ""
	@echo "  Web:     http://localhost:8080/play.html?server=ws://localhost:9091/ws"
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
	rm -rf build build-* _site test-results playwright-report coverage.json crap.json
	rm -f compile_commands.json
	rm -f /tmp/signal-test-shard.*.log /tmp/signal-test-shard.*.exit
