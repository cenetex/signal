.PHONY: all build build-web build-server build-test test test-serial test-fast test-soak test-all smoke crap dev dev-logs dev-clean stop deploy clean install-hooks

all: build build-web build-server

# install-hooks — symlink the tracked git hooks under scripts/git-hooks/
# into .git/hooks/ so commits trigger the fast localhost auto-deploy.
# Symlinks (vs copies) so future edits to scripts/git-hooks/ take effect
# without re-running this target.
install-hooks:
	@root=$$(git rev-parse --show-toplevel); \
	for f in $$root/scripts/git-hooks/*; do \
		name=$$(basename $$f); \
		target=$$root/.git/hooks/$$name; \
		ln -sf ../../scripts/git-hooks/$$name $$target; \
		echo "  hook: $$name → scripts/git-hooks/$$name"; \
	done

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

# Browser smoke: builds the WASM client, serves build-web locally, and
# drives the canvas through the same Playwright smoke used after deploy.
smoke: build-web
	npm run smoke

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
	src/commodity.c src/manifest.c src/ship.c src/economy.c \
	src/asteroid.c src/rng.c shared

crap:
	cmake -S . -B build-coverage -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS_ONLY=ON \
		-DCMAKE_C_FLAGS="--coverage -O0 -g" \
		-DCMAKE_EXE_LINKER_FLAGS="--coverage"
	cmake --build build-coverage --target signal_test
	find build-coverage -name '*.gcda' -delete
	ulimit -s 16384 && ./build-coverage/signal_test --quiet --no-soak
	gcovr -r . --json coverage.json --gcov-ignore-parse-errors \
		--filter 'server/.*' --filter 'src/.*' --filter 'shared/.*' \
		--exclude 'server/mongoose\..*' \
		--exclude 'src/stb_image\.h' \
		--exclude 'src/pl_mpeg\.h' \
		--exclude 'src/minimp3\.h' \
		build-coverage
	python3 scripts/crap.py --coverage coverage.json \
		--paths $(CRAP_TESTED_PATHS) \
		--top 30 --threshold 25 --fail-on-exceed \
		--json-out crap.json

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
