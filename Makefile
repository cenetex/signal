.PHONY: all build build-web build-server build-test build-san test-san test-tsan build-flight-trace flight-trace build-signal-replay build-signal-replay-wasm signal-replay replay-repeatability replay-repeatability-long signal-no-omniscience-soak replay-cross-build replay-cross-build-long replay-native-wasm replay-native-wasm-long build-chain-assets chain-assets build-rati-receipt rati-receipt rati-anchor-batch test-rati-anchor-batch rati-anchor-stamp test-rati-anchor-stamp neural-gap-ab signal-client-brain-shadow signal-hnn-shadow assets protocol-check test test-serial test-fast test-soak test-all smoke smoke-latency smoke-ack-lag smoke-latency-suite banned-apis deterministic-libm deterministic-build-flags cppcheck crap profile-machine latency-proxy latency-proxy-high latency-proxy-ack-lag rtc-gateway deploy-fly site clean install-hooks

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
	emcmake cmake $(GENERATOR) -S . -B build-web -DCMAKE_BUILD_TYPE=Release -DGIT_HASH=$(GIT_HASH)
	emmake cmake --build build-web --parallel
	python3 scripts/check_deterministic_build_flags.py build-web/compile_commands.json

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

# --- Deterministic seed+prefix counterfactual replay harness ---
SIGNAL_REPLAY_SEED ?= 2037
SIGNAL_REPLAY_HISTORY ?= W,W,WA,D
SIGNAL_REPLAY_HORIZON_TICKS ?= 36
SIGNAL_REPLAY_CANDIDATES ?= NONE,W,A,D,S,WA,WD,SA,SD
SIGNAL_REPLAY_OUT ?= /tmp/signal-replay.jsonl
SIGNAL_REPLAY_HNN_TRACE ?=
SIGNAL_REPLAY_ACTIVE_WORKERS ?=
SIGNAL_REPLAY_HNN_CLEANUP_STEPS ?= 3
SIGNAL_REPLAY_DEBUG_BUILD ?= build-replay-debug
SIGNAL_REPLAY_RELEASE_BUILD ?= build-replay-release
SIGNAL_REPLAY_WASM_BUILD ?= build-replay-wasm

build-signal-replay:
	cmake $(GENERATOR) -S . -B build -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DGIT_HASH=$(GIT_HASH)
	@ln -sf build/compile_commands.json compile_commands.json
	cmake --build build --target signal_replay --parallel

signal-replay: build-signal-replay
	./build/signal_replay \
		--seed $(SIGNAL_REPLAY_SEED) \
		--history "$(SIGNAL_REPLAY_HISTORY)" \
		--horizon-ticks $(SIGNAL_REPLAY_HORIZON_TICKS) \
		--candidates "$(SIGNAL_REPLAY_CANDIDATES)" \
		$(if $(SIGNAL_REPLAY_HNN_TRACE),--hnn-trace --hnn-cleanup-steps $(SIGNAL_REPLAY_HNN_CLEANUP_STEPS),) \
		$(if $(SIGNAL_REPLAY_ACTIVE_WORKERS),--active-workers,) \
		--out $(SIGNAL_REPLAY_OUT)

replay-repeatability: build-signal-replay
	python3 scripts/check_replay_repeatability.py ./build/signal_replay

replay-repeatability-long: build-signal-replay
	python3 scripts/check_replay_repeatability.py ./build/signal_replay --scenario-set long

signal-no-omniscience-soak: build-signal-replay
	python3 scripts/check_no_omniscience_soak.py ./build/signal_replay

build-signal-replay-wasm:
	emcmake cmake $(GENERATOR) -S . -B $(SIGNAL_REPLAY_WASM_BUILD) -DCMAKE_BUILD_TYPE=Release -DBUILD_TOOLS=OFF -DBUILD_WASM_REPLAY=ON -DGIT_HASH=$(GIT_HASH)
	emmake cmake --build $(SIGNAL_REPLAY_WASM_BUILD) --target signal_replay --parallel
	python3 scripts/check_deterministic_build_flags.py $(SIGNAL_REPLAY_WASM_BUILD)/compile_commands.json

replay-cross-build:
	cmake $(GENERATOR) -S . -B $(SIGNAL_REPLAY_DEBUG_BUILD) -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS_DEBUG="-O2 -g" -DGIT_HASH=$(GIT_HASH)
	cmake --build $(SIGNAL_REPLAY_DEBUG_BUILD) --target signal_replay --parallel
	python3 scripts/check_deterministic_build_flags.py $(SIGNAL_REPLAY_DEBUG_BUILD)/compile_commands.json
	cmake $(GENERATOR) -S . -B $(SIGNAL_REPLAY_RELEASE_BUILD) -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGIT_HASH=$(GIT_HASH)
	cmake --build $(SIGNAL_REPLAY_RELEASE_BUILD) --target signal_replay --parallel
	python3 scripts/check_deterministic_build_flags.py $(SIGNAL_REPLAY_RELEASE_BUILD)/compile_commands.json
	python3 scripts/check_replay_cross_build.py \
		./$(SIGNAL_REPLAY_DEBUG_BUILD)/signal_replay \
		./$(SIGNAL_REPLAY_RELEASE_BUILD)/signal_replay

replay-cross-build-long:
	cmake $(GENERATOR) -S . -B $(SIGNAL_REPLAY_DEBUG_BUILD) -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS_DEBUG="-O2 -g" -DGIT_HASH=$(GIT_HASH)
	cmake --build $(SIGNAL_REPLAY_DEBUG_BUILD) --target signal_replay --parallel
	python3 scripts/check_deterministic_build_flags.py $(SIGNAL_REPLAY_DEBUG_BUILD)/compile_commands.json
	cmake $(GENERATOR) -S . -B $(SIGNAL_REPLAY_RELEASE_BUILD) -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGIT_HASH=$(GIT_HASH)
	cmake --build $(SIGNAL_REPLAY_RELEASE_BUILD) --target signal_replay --parallel
	python3 scripts/check_deterministic_build_flags.py $(SIGNAL_REPLAY_RELEASE_BUILD)/compile_commands.json
	python3 scripts/check_replay_cross_build.py \
		./$(SIGNAL_REPLAY_DEBUG_BUILD)/signal_replay \
		./$(SIGNAL_REPLAY_RELEASE_BUILD)/signal_replay \
		--scenario-set long

replay-native-wasm: build-signal-replay build-signal-replay-wasm
	python3 scripts/check_replay_cross_build.py \
		./build/signal_replay \
		./$(SIGNAL_REPLAY_WASM_BUILD)/signal_replay.js

replay-native-wasm-long: build-signal-replay build-signal-replay-wasm
	python3 scripts/check_replay_cross_build.py \
		./build/signal_replay \
		./$(SIGNAL_REPLAY_WASM_BUILD)/signal_replay.js \
		--scenario-set long

# --- Chain asset inventory export ---
CHAIN_ASSETS_FORMAT ?= json
CHAIN_ASSETS_INPUT ?= chain
CHAIN_ASSETS_OUT ?=
CHAIN_ASSETS_LINEAGE ?=
CHAIN_ASSETS_BUILT_FROM ?=

build-chain-assets:
	cmake $(GENERATOR) -S . -B build -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DGIT_HASH=$(GIT_HASH)
	@ln -sf build/compile_commands.json compile_commands.json
	cmake --build build --target signal_chain_assets --parallel

chain-assets: build-chain-assets
	./build/signal_chain_assets \
		--format=$(CHAIN_ASSETS_FORMAT) \
		$(if $(CHAIN_ASSETS_OUT),--out=$(CHAIN_ASSETS_OUT),) \
		$(if $(CHAIN_ASSETS_LINEAGE),--lineage=$(CHAIN_ASSETS_LINEAGE),) \
		$(if $(CHAIN_ASSETS_BUILT_FROM),--built-from=$(CHAIN_ASSETS_BUILT_FROM),) \
		$(CHAIN_ASSETS_INPUT)

# --- RATi mining receipt export ---
RATI_RECEIPT_INPUT ?=
RATI_RECEIPT_MIN_PREFIX ?= anonymous
RATI_RECEIPT_EVENT_ID ?=
RATI_RECEIPT_SEGMENT_ID ?=
RATI_RECEIPT_CARGO_PUB ?=
RATI_ANCHOR_RECEIPTS ?=
RATI_ANCHOR_OUT ?= rati-anchor-batch.json
RATI_ANCHOR_PREVIOUS_BATCH_ROOT ?= 0000000000000000000000000000000000000000000000000000000000000000
RATI_ANCHOR_SETTLEMENT_CHECKPOINT_ROOT ?=
RATI_ANCHOR_ARWEAVE_MANIFEST_TX ?=
RATI_ANCHOR_CREATED_AT ?= 0
RATI_ANCHOR_ALLOW_UNVERIFIED ?=
RATI_STAMP_BATCH ?= $(RATI_ANCHOR_OUT)
RATI_STAMP_OTS_OUT ?=
RATI_STAMP_MANIFEST_OUT ?=
RATI_STAMP_OTS_COMMAND ?=
RATI_STAMP_CREATED_AT ?= now
RATI_STAMP_DRY_RUN ?=
RATI_STAMP_OVERWRITE ?=

build-rati-receipt:
	cmake $(GENERATOR) -S . -B build -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DGIT_HASH=$(GIT_HASH)
	@ln -sf build/compile_commands.json compile_commands.json
	cmake --build build --target signal_rati_receipt --parallel

rati-receipt: build-rati-receipt
	@if [ -z "$(RATI_RECEIPT_INPUT)" ]; then \
		echo "Set RATI_RECEIPT_INPUT=<station-chain-log>"; \
		exit 2; \
	fi
	./build/signal_rati_receipt \
		--min-prefix=$(RATI_RECEIPT_MIN_PREFIX) \
		$(if $(RATI_RECEIPT_EVENT_ID),--event-id=$(RATI_RECEIPT_EVENT_ID),) \
		$(if $(RATI_RECEIPT_SEGMENT_ID),--segment-id=$(RATI_RECEIPT_SEGMENT_ID),) \
		$(if $(RATI_RECEIPT_CARGO_PUB),--cargo-pub=$(RATI_RECEIPT_CARGO_PUB),) \
		$(RATI_RECEIPT_INPUT)

rati-anchor-batch:
	@if [ -z "$(RATI_ANCHOR_RECEIPTS)" ]; then \
		echo "Set RATI_ANCHOR_RECEIPTS=<receipt-json...>"; \
		exit 2; \
	fi
	node scripts/build-rati-anchor-batch.mjs \
		--out=$(RATI_ANCHOR_OUT) \
		--created-at-unix=$(RATI_ANCHOR_CREATED_AT) \
		--previous-batch-root=$(RATI_ANCHOR_PREVIOUS_BATCH_ROOT) \
		$(if $(RATI_ANCHOR_SETTLEMENT_CHECKPOINT_ROOT),--settlement-checkpoint-root=$(RATI_ANCHOR_SETTLEMENT_CHECKPOINT_ROOT),) \
		$(if $(RATI_ANCHOR_ARWEAVE_MANIFEST_TX),--arweave-manifest-tx=$(RATI_ANCHOR_ARWEAVE_MANIFEST_TX),) \
		$(if $(RATI_ANCHOR_ALLOW_UNVERIFIED),--allow-unverified,) \
		$(RATI_ANCHOR_RECEIPTS)

test-rati-anchor-batch:
	node scripts/test-rati-anchor-batch.mjs

rati-anchor-stamp:
	@if [ -z "$(RATI_STAMP_BATCH)" ]; then \
		echo "Set RATI_STAMP_BATCH=<rati-anchor-batch.json>"; \
		exit 2; \
	fi
	node scripts/stamp-rati-anchor.mjs \
		--created-at-unix=$(RATI_STAMP_CREATED_AT) \
		$(if $(RATI_STAMP_OTS_OUT),--ots-out=$(RATI_STAMP_OTS_OUT),) \
		$(if $(RATI_STAMP_MANIFEST_OUT),--manifest-out=$(RATI_STAMP_MANIFEST_OUT),) \
		$(if $(RATI_STAMP_OTS_COMMAND),--ots-command=$(RATI_STAMP_OTS_COMMAND),) \
		$(if $(RATI_STAMP_DRY_RUN),--dry-run,) \
		$(if $(RATI_STAMP_OVERWRITE),--overwrite,) \
		$(RATI_STAMP_BATCH)

test-rati-anchor-stamp:
	node scripts/test-rati-anchor-stamp.mjs

# --- Live bot behavior A/B gap harness ---
NEURAL_GAP_DURATION ?= 120
NEURAL_GAP_BOTS ?= 31
NEURAL_GAP_SEED ?= 2037
NEURAL_GAP_OUT ?= /tmp/signal-neural-gap

neural-gap-ab:
	python3 scripts/neural-gap-ab.py \
		--duration $(NEURAL_GAP_DURATION) \
		--bots $(NEURAL_GAP_BOTS) \
		--seed $(NEURAL_GAP_SEED) \
		--world-seq $(NEURAL_GAP_SEED) \
		--out-dir "$(NEURAL_GAP_OUT)"

SIGNAL_CLIENT_BRAIN_SHADOW_LOG ?= /tmp/signal-client-brain-shadow.jsonl
SIGNAL_CLIENT_BRAIN_SHADOW_MIN_ROWS ?= 1
SIGNAL_CLIENT_BRAIN_SHADOW_MIN_TEACHER_ROWS ?= 1
SIGNAL_CLIENT_BRAIN_SHADOW_MIN_MATCH ?=
SIGNAL_CLIENT_BRAIN_SHADOW_MIN_P50_MARGIN ?=

signal-client-brain-shadow:
	node scripts/analyze-signal-client-brain-shadow.mjs \
		--input "$(SIGNAL_CLIENT_BRAIN_SHADOW_LOG)" \
		--min-rows $(SIGNAL_CLIENT_BRAIN_SHADOW_MIN_ROWS) \
		--min-teacher-rows $(SIGNAL_CLIENT_BRAIN_SHADOW_MIN_TEACHER_ROWS) \
		$(if $(SIGNAL_CLIENT_BRAIN_SHADOW_MIN_MATCH),--min-teacher-match-rate $(SIGNAL_CLIENT_BRAIN_SHADOW_MIN_MATCH),) \
		$(if $(SIGNAL_CLIENT_BRAIN_SHADOW_MIN_P50_MARGIN),--min-p50-margin $(SIGNAL_CLIENT_BRAIN_SHADOW_MIN_P50_MARGIN),)

SIGNAL_HNN_SHADOW_LOG ?= /tmp/signal-hnn-shadow.jsonl
SIGNAL_HNN_SHADOW_MIN_ROWS ?= 1
SIGNAL_HNN_SHADOW_MIN_TEACHER_ROWS ?= 1
SIGNAL_HNN_SHADOW_MIN_MATCH ?=
SIGNAL_HNN_SHADOW_MIN_P50_MARGIN ?=
SIGNAL_HNN_SHADOW_MIN_P50_FIDELITY ?=
SIGNAL_HNN_SHADOW_MAX_P90_CAPACITY_LOAD ?=

signal-hnn-shadow:
	node scripts/analyze-signal-hnn-shadow.mjs \
		--input "$(SIGNAL_HNN_SHADOW_LOG)" \
		--min-rows $(SIGNAL_HNN_SHADOW_MIN_ROWS) \
		--min-teacher-rows $(SIGNAL_HNN_SHADOW_MIN_TEACHER_ROWS) \
		$(if $(SIGNAL_HNN_SHADOW_MIN_MATCH),--min-teacher-match-rate $(SIGNAL_HNN_SHADOW_MIN_MATCH),) \
		$(if $(SIGNAL_HNN_SHADOW_MIN_P50_MARGIN),--min-p50-margin $(SIGNAL_HNN_SHADOW_MIN_P50_MARGIN),) \
		$(if $(SIGNAL_HNN_SHADOW_MIN_P50_FIDELITY),--min-p50-fidelity $(SIGNAL_HNN_SHADOW_MIN_P50_FIDELITY),) \
		$(if $(SIGNAL_HNN_SHADOW_MAX_P90_CAPACITY_LOAD),--max-p90-capacity-load $(SIGNAL_HNN_SHADOW_MAX_P90_CAPACITY_LOAD),)

assets:
	./scripts/sync-assets.sh

PROTOCOL_CHECK_URL ?= http://127.0.0.1:9091/api/protocol

protocol-check:
	scripts/protocol-check.py --url "$(PROTOCOL_CHECK_URL)"

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
	cmake --build build --target signal_chain_assets --parallel
	cmake --build build --target signal_rati_receipt --parallel
	# Compile-check the native client too. signal_test doesn't pull in
	# net_sync.c / world_draw.c / hud.c (client-only), so a struct
	# rename that breaks the wire-decode side won't fail signal_test
	# alone. The user has hit this exact gap (npc_ship_t.pos → .ship.pos
	# silently broke autopilot in deployed wasm). Keep this fast: it's
	# an incremental build of the same -O2/-g object cache, so unchanged
	# files don't re-link.
	cmake --build build --target signal --parallel
	python3 scripts/check_deterministic_build_flags.py

# Number of shards for the parallel test runner. Defaults to min(8, ncores).
NCORES := $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
TEST_SHARDS ?= $(shell echo $$(( $(NCORES) < 8 ? $(NCORES) : 8 )))
TEST_BIN ?= ./build/signal_test
TEST_ENV ?=
TEST_PREFIX ?=

# Reusable parallel-shard runner. Caller passes RUN_FLAGS for the test
# binary (e.g. --no-soak / --soak-only); the runner handles sharding,
# wait, and aggregate reporting.
define RUN_PARALLEL_TESTS
	@rm -f /tmp/signal-test-shard.*.log /tmp/signal-test-shard.*.exit
	@for i in $$(seq 0 $$(($(TEST_SHARDS) - 1))); do \
		( $(TEST_PREFIX) $(TEST_ENV) $(TEST_BIN) --shard=$$i/$(TEST_SHARDS) $(1) $(TEST_QUIET) \
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

SANITIZER ?= address,undefined
SAN_BUILD_DIR ?= build-san
SAN_TEST_FLAGS ?= --quiet --no-soak

build-san:
	cmake $(GENERATOR) -S . -B $(SAN_BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug \
		-DBUILD_TESTS_ONLY=ON -DGIT_HASH=$(GIT_HASH) \
		-DCMAKE_C_FLAGS="-O1 -g -fsanitize=$(SANITIZER) -fno-omit-frame-pointer" \
		-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=$(SANITIZER)"
	@ln -sf $(SAN_BUILD_DIR)/compile_commands.json compile_commands.json
	cmake --build $(SAN_BUILD_DIR) --parallel

test-san: TEST_BIN=./$(SAN_BUILD_DIR)/signal_test
test-san: TEST_ENV=ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
test-san: TEST_PREFIX=ulimit -s 16384 &&
test-san: build-san
	$(call RUN_PARALLEL_TESTS,$(SAN_TEST_FLAGS))

test-tsan: TEST_BIN=./build-tsan/signal_test
test-tsan: TEST_ENV=TSAN_OPTIONS=halt_on_error=1
test-tsan: TEST_PREFIX=ulimit -s 16384 &&
test-tsan:
	cmake $(GENERATOR) -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
		-DBUILD_TESTS_ONLY=ON -DGIT_HASH=$(GIT_HASH) \
		-DCMAKE_C_FLAGS="-O1 -g -fsanitize=thread -fno-omit-frame-pointer" \
		-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
	@ln -sf build-tsan/compile_commands.json compile_commands.json
	cmake --build build-tsan --parallel
	$(call RUN_PARALLEL_TESTS,$(SAN_TEST_FLAGS))

banned-apis:
	python3 scripts/check_banned_apis.py

deterministic-libm:
	python3 scripts/check_deterministic_libm.py

deterministic-build-flags:
	python3 scripts/check_deterministic_build_flags.py $(COMPILE_COMMANDS)

# Static analysis for owned C sources. Avoid --project=compile_commands.json
# here: it pulls in test fixtures and single-header vendor libraries whose
# allocation-model warnings swamp actionable project-code findings.
CPPCHECK ?= cppcheck
CPPCHECK_SOURCES := server shared client tools/signal_verify.c tools/signal_chain_assets.c tools/signal_rati_receipt.c tools/flight_trace.c tools/signal_replay.c

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
SMOKE_PORT ?= 39111

smoke: build-web
	SMOKE_PORT=$(SMOKE_PORT) npm run smoke

SMOKE_LATENCY_URL ?= http://localhost:8080/play.html?server=ws://127.0.0.1:19091/ws

smoke-latency:
	SMOKE_URL="$(SMOKE_LATENCY_URL)" SMOKE_LATENCY_ASSERT=1 npx playwright test tests/browser-smoke.spec.ts --project=chromium --grep "high-latency"

smoke-ack-lag:
	SMOKE_URL="$(SMOKE_LATENCY_URL)" SMOKE_ACK_LAG_ASSERT=1 npx playwright test tests/browser-smoke.spec.ts --project=chromium --grep "low-ping high-ack"

smoke-latency-suite: build-web build-server
	node scripts/smoke-latency-suite.mjs

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

RTC_GATEWAY_LISTEN ?= 127.0.0.1:19093
RTC_GATEWAY_UPSTREAM ?= ws://127.0.0.1:9091/ws

rtc-gateway:
	npm run rtc-gateway -- --listen=$(RTC_GATEWAY_LISTEN) --upstream=$(RTC_GATEWAY_UPSTREAM)

# --- Static web bundle / Fly deploy ---
site: build-web
	@rm -rf _site
	@mkdir -p _site
	cp build-web/signal.js build-web/signal.wasm \
	   build-web/play.html build-web/signal-touch-controls.js _site/
	@if [ -d build-web/anime ]; then cp -R build-web/anime _site/; fi
	@if [ -d build-web/music ]; then cp -R build-web/music _site/; fi
	cp web/index.html web/ost.html web/ost-manifest.json web/ost-cover.jpg \
	   web/mine.html _site/
	@echo "Site built in _site/"

deploy-fly:
	flyctl deploy --remote-only --config fly.toml --build-arg GIT_HASH=$(GIT_HASH)



clean:
	rm -rf build build-* _site test-results playwright-report coverage.json crap.json
	rm -f compile_commands.json
	rm -f /tmp/signal-test-shard.*.log /tmp/signal-test-shard.*.exit
