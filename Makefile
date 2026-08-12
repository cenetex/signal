.PHONY: all build build-web build-server build-test build-san test-san test-san-soak build-msan test-msan build-tsan test-tsan memzero-codegen build-mode-contract client-memory-budget build-flight-trace flight-trace build-signal-replay build-signal-replay-wasm signal-replay replay-repeatability replay-repeatability-long signal-no-omniscience-soak replay-cross-build replay-cross-build-long replay-native-wasm replay-native-wasm-long build-chain-assets chain-assets build-rati-receipt rati-receipt rati-anchor-batch test-rati-anchor-batch rati-anchor-stamp neural-gap-ab signal-client-brain-shadow signal-hnn-shadow assets protocol-check test test-serial test-fast test-soak test-all asteroid-physics-bench smoke smoke-latency smoke-ack-lag smoke-latency-suite relay-traffic-probe ws-backpressure-soak ws-backpressure-soak-short cargo-trust-audit banned-apis deterministic-libm deterministic-build-flags doc-freshness soak-automation vendor-drift fuzz-receipts fuzz-receipts-standalone cppcheck crap profile-machine latency-proxy latency-proxy-high latency-proxy-ack-lag rtc-gateway test-rtc-gateway deploy-fly site clean install-hooks

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
SIGNAL_HOST_OS := $(shell uname -s 2>/dev/null || echo Unknown)
BUILD_TYPE ?= RelWithDebInfo
GIT_HASH ?= $(shell git rev-parse --short HEAD 2>/dev/null || echo dev)
SIM_PROFILE ?=
SIM_PROFILE_CMAKE := -DSIGNAL_SIM_PROFILE=$(if $(SIM_PROFILE),ON,OFF)
SERVER_BUILD_DIR ?= build-server
SERVER_BUILD_BIN := $(SERVER_BUILD_DIR)/signal_server
SERVER_COMPAT_BIN := build/signal_server

# --- Native desktop client ---
build:
	cmake $(GENERATOR) -S . -B build -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DGIT_HASH=$(GIT_HASH) $(SIM_PROFILE_CMAKE)
	@ln -sf build/compile_commands.json compile_commands.json
	cmake --build build --target signal --parallel

# --- Emscripten web client ---
build-web:
	emcmake cmake $(GENERATOR) -S . -B build-web -DCMAKE_BUILD_TYPE=Release -DGIT_HASH=$(GIT_HASH) $(SIM_PROFILE_CMAKE)
	emmake cmake --build build-web --parallel
	python3 scripts/check_deterministic_build_flags.py build-web/compile_commands.json
	python3 scripts/check_client_memory_budget.py build-web/signal.wasm

# --- Headless game server ---
# Keep the server in its own CMake cache. CMake option values are sticky:
# reusing `build/` after a BUILD_TESTS_ONLY=ON configure can otherwise make
# `cmake --build ... --target signal_server` return success while leaving an
# old on-disk binary untouched. Copy the verified result to the historical
# path so existing scripts and operator docs remain compatible.
build-server:
	cmake $(GENERATOR) -S . -B $(SERVER_BUILD_DIR) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DBUILD_TESTS_ONLY=OFF -DBUILD_SERVER_ONLY=ON -DBUILD_TOOLS=OFF \
		-DGIT_HASH=$(GIT_HASH) $(SIM_PROFILE_CMAKE)
	@ln -sf $(SERVER_BUILD_DIR)/compile_commands.json compile_commands.json
	cmake --build $(SERVER_BUILD_DIR) --target signal_server --parallel
	cmake -E make_directory build
	cmake -E copy_if_different $(SERVER_BUILD_BIN) $(SERVER_COMPAT_BIN)

# --- Offline WASD flight-brain training traces ---
FLIGHT_TRACE_EPISODES ?= 1000
FLIGHT_TRACE_TICKS ?= 600
FLIGHT_TRACE_SEED ?= 2037
FLIGHT_TRACE_SHARD ?= 0/1
FLIGHT_TRACE_FORMAT ?= csv
FLIGHT_TRACE_OUT ?= /tmp/signal-flight-trace.csv

build-flight-trace:
	cmake $(GENERATOR) -S . -B build -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DGIT_HASH=$(GIT_HASH) $(SIM_PROFILE_CMAKE)
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
SIGNAL_REPLAY_EVALUATION_WORLD ?=
SIGNAL_REPLAY_HNN_CLEANUP_STEPS ?= 3
SIGNAL_REPLAY_DEBUG_BUILD ?= build-replay-debug
SIGNAL_REPLAY_RELEASE_BUILD ?= build-replay-release
SIGNAL_REPLAY_WASM_BUILD ?= build-replay-wasm

build-signal-replay:
	cmake $(GENERATOR) -S . -B build -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DGIT_HASH=$(GIT_HASH) $(SIM_PROFILE_CMAKE)
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
		$(if $(SIGNAL_REPLAY_EVALUATION_WORLD),--evaluation-world $(SIGNAL_REPLAY_EVALUATION_WORLD),) \
		--out $(SIGNAL_REPLAY_OUT)

replay-repeatability: build-signal-replay
	python3 scripts/check_replay_repeatability.py ./build/signal_replay

replay-repeatability-long: build-signal-replay
	python3 scripts/check_replay_repeatability.py ./build/signal_replay --scenario-set long

replay-ai-eval-repeatability: build-signal-replay
	python3 scripts/check_replay_repeatability.py \
		./build/signal_replay --scenario-set ai-eval-fast

replay-ai-eval-repeatability-long: build-signal-replay
	python3 scripts/check_replay_repeatability.py \
		./build/signal_replay --scenario-set ai-eval-long

signal-no-omniscience-soak: build-signal-replay
	python3 scripts/check_no_omniscience_soak.py ./build/signal_replay

build-signal-replay-wasm:
	emcmake cmake $(GENERATOR) -S . -B $(SIGNAL_REPLAY_WASM_BUILD) -DCMAKE_BUILD_TYPE=Release -DBUILD_TOOLS=OFF -DBUILD_WASM_REPLAY=ON -DGIT_HASH=$(GIT_HASH) $(SIM_PROFILE_CMAKE)
	emmake cmake --build $(SIGNAL_REPLAY_WASM_BUILD) --target signal_replay --parallel
	python3 scripts/check_deterministic_build_flags.py $(SIGNAL_REPLAY_WASM_BUILD)/compile_commands.json

replay-cross-build:
	cmake $(GENERATOR) -S . -B $(SIGNAL_REPLAY_DEBUG_BUILD) -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS_DEBUG="-O2 -g" -DGIT_HASH=$(GIT_HASH) $(SIM_PROFILE_CMAKE)
	cmake --build $(SIGNAL_REPLAY_DEBUG_BUILD) --target signal_replay --parallel
	python3 scripts/check_deterministic_build_flags.py $(SIGNAL_REPLAY_DEBUG_BUILD)/compile_commands.json
	cmake $(GENERATOR) -S . -B $(SIGNAL_REPLAY_RELEASE_BUILD) -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGIT_HASH=$(GIT_HASH) $(SIM_PROFILE_CMAKE)
	cmake --build $(SIGNAL_REPLAY_RELEASE_BUILD) --target signal_replay --parallel
	python3 scripts/check_deterministic_build_flags.py $(SIGNAL_REPLAY_RELEASE_BUILD)/compile_commands.json
	python3 scripts/check_replay_cross_build.py \
		./$(SIGNAL_REPLAY_DEBUG_BUILD)/signal_replay \
		./$(SIGNAL_REPLAY_RELEASE_BUILD)/signal_replay

replay-cross-build-long:
	cmake $(GENERATOR) -S . -B $(SIGNAL_REPLAY_DEBUG_BUILD) -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS_DEBUG="-O2 -g" -DGIT_HASH=$(GIT_HASH) $(SIM_PROFILE_CMAKE)
	cmake --build $(SIGNAL_REPLAY_DEBUG_BUILD) --target signal_replay --parallel
	python3 scripts/check_deterministic_build_flags.py $(SIGNAL_REPLAY_DEBUG_BUILD)/compile_commands.json
	cmake $(GENERATOR) -S . -B $(SIGNAL_REPLAY_RELEASE_BUILD) -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGIT_HASH=$(GIT_HASH) $(SIM_PROFILE_CMAKE)
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

replay-ai-eval-native-wasm: build-signal-replay build-signal-replay-wasm
	python3 scripts/check_replay_cross_build.py \
		./build/signal_replay \
		./$(SIGNAL_REPLAY_WASM_BUILD)/signal_replay.js \
		--scenario-set ai-eval-fast

# --- Chain asset inventory export ---
CHAIN_ASSETS_FORMAT ?= json
CHAIN_ASSETS_INPUT ?= chain
CHAIN_ASSETS_OUT ?=
CHAIN_ASSETS_LINEAGE ?=
CHAIN_ASSETS_BUILT_FROM ?=

build-chain-assets:
	cmake $(GENERATOR) -S . -B build -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DGIT_HASH=$(GIT_HASH) $(SIM_PROFILE_CMAKE)
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
	cmake $(GENERATOR) -S . -B build -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DGIT_HASH=$(GIT_HASH) $(SIM_PROFILE_CMAKE)
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

# The former S3 sync script was removed with the retired AWS deployment.
# Keep the old target fail-loud so a stale command cannot silently succeed
# merely because the assets/ directory exists.
assets:
	@echo "make assets is retired: assets/manifest.txt inventories external/local media, but this repository has no download command" >&2
	@exit 2

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
	cmake $(GENERATOR) -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS_DEBUG="-O2 -g" -DGIT_HASH=$(GIT_HASH) $(SIM_PROFILE_CMAKE)
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
	# alone. The user has hit this exact gap (npc_ship_t.pos → .ship->pos
	# silently broke autopilot in deployed wasm). Keep this fast: it's
	# an incremental build of the same -O2/-g object cache, so unchanged
	# files don't re-link.
	cmake --build build --target signal --parallel
	cmake --build build --target signal_client_memory --parallel
	./build/signal_client_memory
	python3 scripts/check_deterministic_build_flags.py

client-memory-budget:
	cmake $(GENERATOR) -S . -B build -DCMAKE_BUILD_TYPE=Release -DGIT_HASH=$(GIT_HASH) $(SIM_PROFILE_CMAKE)
	cmake --build build --target signal_client_memory --parallel
	./build/signal_client_memory

# Number of shards for the parallel test runner. Defaults to min(8, ncores).
NCORES := $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
TEST_SHARDS ?= $(shell echo $$(( $(NCORES) < 8 ? $(NCORES) : 8 )))
TEST_BIN ?= ./build/signal_test
TEST_ENV ?=
TEST_SUITE_LABEL ?= native
TEST_EXPECTED_COUNT ?=
# CI callers can retain a concurrency-safe copy of every shard log on
# failure. Each invocation creates a unique run.* subdirectory.
TEST_FAILURE_LOG_DIR ?=
# world_t is larger than Linux's usual 8 MiB soft stack limit. The launcher
# raises the limit before exec on Linux; macOS reserves the same 64 MiB in the
# signal_test link options. Keep documented direct invocations on this path.
TEST_RUNNER ?= ./scripts/run_signal_test.sh

# Reusable parallel-shard runner. Caller passes RUN_FLAGS for the test
# binary (e.g. --no-soak / --soak-only); the runner handles sharding,
# wait, and aggregate reporting.
define RUN_PARALLEL_TESTS
	@log_dir=$$(mktemp -d "$${TMPDIR:-/tmp}/signal-test-shards.XXXXXX") || exit 1; \
	trap 'rm -rf "$$log_dir"' EXIT HUP INT TERM; \
	for i in $$(seq 0 $$(($(TEST_SHARDS) - 1))); do \
		( $(TEST_ENV) $(TEST_RUNNER) $(TEST_BIN) --shard=$$i/$(TEST_SHARDS) $(1) $(TEST_QUIET) \
			> "$$log_dir/$$i.log" 2>&1; \
		  echo $$? > "$$log_dir/$$i.exit" ) & \
	done; \
	wait; \
	fail=0; total_run=0; total_passed=0; total_failed=0; \
	for i in $$(seq 0 $$(($(TEST_SHARDS) - 1))); do \
		if [ ! -f "$$log_dir/$$i.exit" ]; then \
			echo ""; echo "=== shard $$i did not record an exit status ==="; \
			[ ! -f "$$log_dir/$$i.log" ] || cat "$$log_dir/$$i.log"; \
			fail=1; \
			continue; \
		fi; \
		ec=$$(cat "$$log_dir/$$i.exit"); \
		if [ "$$ec" != "0" ]; then \
			echo ""; echo "=== shard $$i failed (exit $$ec) ==="; \
			cat "$$log_dir/$$i.log"; \
			fail=1; \
		fi; \
		summary_count=$$(grep -Ec "^[0-9]+ tests run, [0-9]+ passed, [0-9]+ failed(, [0-9]+ warnings)?$$" "$$log_dir/$$i.log" || true); \
		if [ "$$summary_count" != "1" ]; then \
			echo ""; echo "=== shard $$i has $$summary_count valid test summaries (expected 1) ==="; \
			[ ! -f "$$log_dir/$$i.log" ] || cat "$$log_dir/$$i.log"; \
			fail=1; \
			continue; \
		fi; \
		line=$$(grep -E "^[0-9]+ tests run, [0-9]+ passed, [0-9]+ failed(, [0-9]+ warnings)?$$" "$$log_dir/$$i.log"); \
		r=$$(echo $$line | awk '{print $$1}'); \
		p=$$(echo $$line | awk '{print $$4}'); \
		f=$$(echo $$line | awk '{print $$6}'); \
		if [ "$$r" -ne $$((p + f)) ]; then \
			echo ""; echo "=== shard $$i has an inconsistent test summary ==="; \
			cat "$$log_dir/$$i.log"; \
			fail=1; \
			continue; \
		fi; \
		if [ "$$f" -ne 0 ]; then \
			fail=1; \
		fi; \
		total_run=$$(( total_run + $${r:-0} )); \
		total_passed=$$(( total_passed + $${p:-0} )); \
		total_failed=$$(( total_failed + $${f:-0} )); \
	done; \
	echo ""; \
	echo "$(TEST_SUITE_LABEL): $$total_run tests run, $$total_passed passed, $$total_failed failed (across $(TEST_SHARDS) shards)"; \
	if [ -n "$(TEST_EXPECTED_COUNT)" ] && [ "$$total_run" -ne "$(TEST_EXPECTED_COUNT)" ]; then \
		echo "=== $(TEST_SUITE_LABEL) discovered $(TEST_EXPECTED_COUNT) tagged tests but ran $$total_run ==="; \
		fail=1; \
	fi; \
	if [ "$$fail" != "0" ] && [ -n "$(TEST_FAILURE_LOG_DIR)" ]; then \
		mkdir -p "$(TEST_FAILURE_LOG_DIR)" || exit 1; \
		failure_dir=$$(mktemp -d "$(TEST_FAILURE_LOG_DIR)/run.XXXXXX") || exit 1; \
		for file in "$$log_dir"/*; do \
			[ ! -f "$$file" ] || cp "$$file" "$$failure_dir/"; \
		done; \
		echo "failure logs saved to $$failure_dir"; \
	fi; \
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
SOAK_TEST_COUNT := $(shell grep -hE '^[[:space:]]*RUN_SOAK\([[:space:]]*[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\)[[:space:]]*;' tests/c/test_*.c | wc -l | tr -d '[:space:]')

test test-fast: TEST_SUITE_LABEL=non-soak
test test-fast: build-test
	$(call RUN_PARALLEL_TESTS,--no-soak)

test-soak: TEST_SUITE_LABEL=functional soak (RUN_SOAK)
test-soak: TEST_EXPECTED_COUNT=$(SOAK_TEST_COUNT)
test-soak: build-test
	$(call RUN_PARALLEL_TESTS,--soak-only)

test-all: TEST_SUITE_LABEL=full native
test-all: build-test
	$(call RUN_PARALLEL_TESTS,--soak)

test-serial: build-test
	$(TEST_ENV) $(TEST_RUNNER) ./build/signal_test --no-soak $(TEST_QUIET)

asteroid-physics-bench: build-test
	SIGNAL_RUN_ASTEROID_PHYSICS_BENCH=1 \
		$(TEST_RUNNER) ./build/signal_test \
		--filter=asteroid_physics_density_benchmark --no-soak

SANITIZER ?= address,undefined
SAN_BUILD_DIR ?= build-san
SAN_TEST_FLAGS ?= --quiet --no-soak
SAN_SOAK_TEST_FLAGS ?= --quiet --soak-only
# LeakSanitizer ships with Linux ASan. Apple's runtime does not support it,
# so Linux CI is the authoritative leak gate while macOS keeps ASan usable.
ASAN_DETECT_LEAKS ?= $(if $(filter Linux,$(SIGNAL_HOST_OS)),1,0)

build-san:
	cmake $(GENERATOR) -S . -B $(SAN_BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug \
		-DBUILD_TESTS_ONLY=ON -DGIT_HASH=$(GIT_HASH) $(SIM_PROFILE_CMAKE) \
		-DCMAKE_C_FLAGS="-O1 -g -fsanitize=$(SANITIZER) -fno-omit-frame-pointer" \
		-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=$(SANITIZER)"
	@ln -sf $(SAN_BUILD_DIR)/compile_commands.json compile_commands.json
	cmake --build $(SAN_BUILD_DIR) --parallel

test-san test-san-soak: TEST_BIN=$(SAN_BUILD_DIR)/signal_test
test-san test-san-soak: TEST_ENV=ASAN_OPTIONS=detect_leaks=$(ASAN_DETECT_LEAKS):halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
test-san: TEST_SUITE_LABEL=sanitized non-soak
test-san: build-san
	$(call RUN_PARALLEL_TESTS,$(SAN_TEST_FLAGS))

test-san-soak: TEST_SUITE_LABEL=sanitized functional soak (RUN_SOAK)
test-san-soak: TEST_EXPECTED_COUNT=$(SOAK_TEST_COUNT)
test-san-soak: build-san
	$(call RUN_PARALLEL_TESTS,$(SAN_SOAK_TEST_FLAGS))

MSAN_CC ?= clang
MSAN_BUILD_DIR ?= build-msan
MSAN_TEST_FILTERS ?= signal_memzero identity_clear station_authority_cleanup manifest_reset

build-msan:
	cmake $(GENERATOR) -S . -B $(MSAN_BUILD_DIR) \
		-DCMAKE_C_COMPILER=$(MSAN_CC) -DCMAKE_BUILD_TYPE=Debug \
		-DBUILD_TESTS_ONLY=ON -DGIT_HASH=$(GIT_HASH) $(SIM_PROFILE_CMAKE) \
		-DCMAKE_C_FLAGS="-O1 -g -fsanitize=memory -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer -fPIE" \
		-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=memory -fsanitize-memory-track-origins=2 -pie"
	@ln -sf $(MSAN_BUILD_DIR)/compile_commands.json compile_commands.json
	cmake --build $(MSAN_BUILD_DIR) --target signal_test --parallel

test-msan: build-msan
	@set -e; \
	for filter in $(MSAN_TEST_FILTERS); do \
		echo "MSan: $$filter"; \
		MSAN_OPTIONS=halt_on_error=1:exit_code=86:poison_in_dtor=1 \
			$(TEST_RUNNER) $(MSAN_BUILD_DIR)/signal_test \
			--quiet --no-soak --filter=$$filter; \
	done

MEMZERO_CC ?= clang
MEMZERO_IR ?= build-safety/signal_memzero.ll

memzero-codegen:
	@mkdir -p $(dir $(MEMZERO_IR))
	$(MEMZERO_CC) -std=c11 -O3 -DSIGNAL_MEMZERO_FORCE_FALLBACK=1 \
		-S -emit-llvm -Ishared shared/signal_memzero.c -o $(MEMZERO_IR)
	python3 scripts/check_memzero_codegen.py $(MEMZERO_IR)

build-mode-contract:
	python3 scripts/check_make_build_isolation.py
	python3 scripts/test_check_make_build_isolation.py

TSAN_CC ?= clang
TSAN_BUILD_DIR ?= build-tsan
TSAN_TEST_FLAGS ?= --quiet --no-soak --filter=hnn_reentrant_key_state_and_simulation

build-tsan:
	cmake $(GENERATOR) -S . -B $(TSAN_BUILD_DIR) \
		-DCMAKE_C_COMPILER=$(TSAN_CC) -DCMAKE_BUILD_TYPE=Debug \
		-DBUILD_TESTS_ONLY=ON -DGIT_HASH=$(GIT_HASH) $(SIM_PROFILE_CMAKE) \
		-DCMAKE_C_FLAGS="-O1 -g -fsanitize=thread -fno-omit-frame-pointer -fPIE" \
		-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread -pie"
	@ln -sf $(TSAN_BUILD_DIR)/compile_commands.json compile_commands.json
	cmake --build $(TSAN_BUILD_DIR) --target signal_test --parallel

test-tsan: TEST_BIN=$(TSAN_BUILD_DIR)/signal_test
test-tsan: TEST_ENV=TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1
test-tsan: TEST_SHARDS=1
test-tsan: TEST_SUITE_LABEL=bounded ThreadSanitizer
test-tsan: build-tsan
	$(call RUN_PARALLEL_TESTS,$(TSAN_TEST_FLAGS))

# libFuzzer harness for untrusted receipt/handoff decode paths.
# Requires a clang that ships the libFuzzer runtime — Xcode CLT clang
# does NOT (missing libclang_rt.fuzzer_osx.a), so prefer Homebrew LLVM
# when present. Override with FUZZ_CC=... . FUZZ_TIME bounds the run;
# crash artifacts land in tests/fuzz/artifacts/ for standalone replay.
# New coverage inputs land in the ignored build tree so routine fuzz runs
# do not dirty the curated, tracked seed corpus. Keep this configuration
# headless: decoder fuzzing must not depend on desktop audio/window libraries.
FUZZ_CC ?= $(shell if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then echo /opt/homebrew/opt/llvm/bin/clang; else echo clang; fi)
FUZZ_TIME ?= 60
FUZZ_TIMEOUT ?= 10
FUZZ_WORK_CORPUS ?= build-fuzz/corpus
FUZZ_MODES := receipt-chain receipt-store handoff
# Large enough for the ticket prefix plus HANDOFF_SHIP_SNAPSHOT_MAX_SIZE.
# Without this override libFuzzer defaults to 4096 bytes and cannot reach
# multi-cargo snapshots with full receipt chains.
FUZZ_MAX_LEN ?= 131072
fuzz-receipts:
	cmake $(GENERATOR) -S . -B build-fuzz -DCMAKE_BUILD_TYPE=RelWithDebInfo \
		-DBUILD_TESTS_ONLY=ON -DBUILD_TOOLS=OFF \
		-DSIGNAL_BUILD_FUZZERS=ON -DCMAKE_C_COMPILER=$(FUZZ_CC)
	cmake --build build-fuzz --parallel --target fuzz_cargo_receipt
	@test -d tests/fuzz/corpus || { \
		echo "tracked fuzz corpus missing: tests/fuzz/corpus" >&2; exit 2; \
	}
	mkdir -p tests/fuzz/artifacts $(FUZZ_WORK_CORPUS)
	@echo "Replaying the complete tracked mixed-mode corpus"
	./build-fuzz/fuzz_cargo_receipt tests/fuzz/corpus \
		-artifact_prefix=tests/fuzz/artifacts/replay- \
		-runs=0 -timeout=$(FUZZ_TIMEOUT) -max_len=$(FUZZ_MAX_LEN) \
		-print_final_stats=1
	@set -e; \
	for mode in $(FUZZ_MODES); do \
		echo "Exploring fuzz mode $$mode for $(FUZZ_TIME)s"; \
		mkdir -p "$(FUZZ_WORK_CORPUS)/$$mode"; \
		SIGNAL_FUZZ_MODE="$$mode" \
			./build-fuzz/fuzz_cargo_receipt \
			"$(FUZZ_WORK_CORPUS)/$$mode" tests/fuzz/corpus \
			-artifact_prefix="tests/fuzz/artifacts/$$mode-" \
			-max_total_time=$(FUZZ_TIME) \
			-timeout=$(FUZZ_TIMEOUT) -max_len=$(FUZZ_MAX_LEN) \
			-print_final_stats=1; \
	done

# Replays corpus/crash artifacts through the harness under plain
# ASan/UBSan (no libFuzzer) — use for triage of tests/fuzz/artifacts/.
# Exit is nonzero if any artifact crashes. Compile the upstream TweetNaCl
# translation unit separately so its reviewed UB exemptions cannot disable
# instrumentation in the project-owned entropy, wrapper, or wipe code.
FUZZ_STANDALONE_CFLAGS := -std=c11 -O1 -g -DSIGNAL_FUZZ_STANDALONE \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-Ishared -Iserver -Iclient -Ivendor/tweetnacl
FUZZ_STANDALONE_OBJECTS := \
	build-fuzz/fuzz_cargo_receipt_standalone.o \
	build-fuzz/cargo_receipt_standalone.o \
	build-fuzz/handoff_ticket_standalone.o \
	build-fuzz/manifest_standalone.o \
	build-fuzz/commodity_standalone.o \
	build-fuzz/randombytes_standalone.o \
	build-fuzz/signal_crypto_tweetnacl_standalone.o \
	build-fuzz/signal_memzero_standalone.o \
	build-fuzz/tweetnacl_upstream_standalone.o

fuzz-receipts-standalone:
	@mkdir -p build-fuzz tests/fuzz/artifacts tests/fuzz/corpus
	$(FUZZ_CC) $(FUZZ_STANDALONE_CFLAGS) -c \
		tests/fuzz/fuzz_cargo_receipt.c \
		-o build-fuzz/fuzz_cargo_receipt_standalone.o
	$(FUZZ_CC) $(FUZZ_STANDALONE_CFLAGS) -c \
		shared/cargo_receipt.c -o build-fuzz/cargo_receipt_standalone.o
	$(FUZZ_CC) $(FUZZ_STANDALONE_CFLAGS) -c \
		shared/handoff_ticket.c -o build-fuzz/handoff_ticket_standalone.o
	$(FUZZ_CC) $(FUZZ_STANDALONE_CFLAGS) -c \
		shared/manifest.c -o build-fuzz/manifest_standalone.o
	$(FUZZ_CC) $(FUZZ_STANDALONE_CFLAGS) -c \
		shared/commodity.c -o build-fuzz/commodity_standalone.o
	$(FUZZ_CC) $(FUZZ_STANDALONE_CFLAGS) -c \
		vendor/tweetnacl/randombytes.c \
		-o build-fuzz/randombytes_standalone.o
	$(FUZZ_CC) $(FUZZ_STANDALONE_CFLAGS) -c \
		vendor/tweetnacl/signal_crypto_tweetnacl.c \
		-o build-fuzz/signal_crypto_tweetnacl_standalone.o
	$(FUZZ_CC) $(FUZZ_STANDALONE_CFLAGS) -c \
		shared/signal_memzero.c -o build-fuzz/signal_memzero_standalone.o
	$(FUZZ_CC) $(FUZZ_STANDALONE_CFLAGS) -w \
		-fno-sanitize=shift -fno-sanitize=signed-integer-overflow \
		-c vendor/tweetnacl/tweetnacl.c \
		-o build-fuzz/tweetnacl_upstream_standalone.o
	$(FUZZ_CC) $(FUZZ_STANDALONE_OBJECTS) \
		-fsanitize=address,undefined -lm \
		-o build-fuzz/fuzz_cargo_receipt_standalone
	@set --; \
	for f in tests/fuzz/artifacts/* tests/fuzz/corpus/*; do \
		if [ -f "$$f" ]; then set -- "$$@" "$$f"; fi; \
	done; \
	if [ "$$#" -gt 0 ]; then \
		./build-fuzz/fuzz_cargo_receipt_standalone "$$@"; \
	fi

cargo-trust-audit:
	python3 scripts/check_cargo_trust_boundaries.py
	python3 scripts/test_check_cargo_trust_boundaries.py

banned-apis:
	python3 scripts/check_banned_apis.py

deterministic-libm:
	python3 scripts/check_deterministic_libm.py

doc-freshness:
	python3 scripts/check_doc_freshness.py
	python3 scripts/test_check_doc_freshness.py

soak-automation:
	python3 scripts/check_soak_automation.py
	python3 scripts/test_check_soak_automation.py

vendor-drift:
	bash scripts/check_vendor_drift.sh
	bash scripts/test_vendor_drift_detection.sh
	python3 scripts/check_container_workflow_inputs.py
	python3 scripts/test_check_container_workflow_inputs.py

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

RELAY_PROBE_URL ?= ws://127.0.0.1:9091/ws
RELAY_PROBE_CLIENTS ?= 2
RELAY_PROBE_WARMUP_MS ?= 1500
RELAY_PROBE_DURATION_MS ?= 4000
RELAY_PROBE_PING_HZ ?= 0.5
RELAY_PROBE_INPUT_ACK_HZ ?= 2
RELAY_PROBE_EXTRA ?=

relay-traffic-probe:
	node scripts/relay-traffic-probe.mjs --url=$(RELAY_PROBE_URL) --clients=$(RELAY_PROBE_CLIENTS) --warmup-ms=$(RELAY_PROBE_WARMUP_MS) --duration-ms=$(RELAY_PROBE_DURATION_MS) --ping-hz=$(RELAY_PROBE_PING_HZ) --input-ack-hz=$(RELAY_PROBE_INPUT_ACK_HZ) $(RELAY_PROBE_EXTRA)

WS_BACKPRESSURE_SOAK_EXTRA ?=

ws-backpressure-soak: build-server
	node scripts/ws-backpressure-soak.mjs $(WS_BACKPRESSURE_SOAK_EXTRA)

ws-backpressure-soak-short: build-server
	node scripts/ws-backpressure-soak.mjs --short $(WS_BACKPRESSURE_SOAK_EXTRA)

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
	$(TEST_RUNNER) ./build-coverage/signal_test --quiet --no-soak
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
LATENCY_INPUT_APPLIED_MS ?= 0
LATENCY_JITTER_MS ?= 80

latency-proxy:
	node scripts/ws-latency-proxy.mjs \
		--listen=$(LATENCY_LISTEN) \
		--upstream=$(LATENCY_UPSTREAM) \
		--client-ms=$(LATENCY_CLIENT_MS) \
		--server-ms=$(LATENCY_SERVER_MS) \
		--server-world-players-ms=$(LATENCY_WORLD_PLAYERS_MS) \
		--server-input-applied-ms=$(LATENCY_INPUT_APPLIED_MS) \
		--jitter-ms=$(LATENCY_JITTER_MS)

latency-proxy-high:
	$(MAKE) latency-proxy LATENCY_CLIENT_MS=450 LATENCY_SERVER_MS=450 LATENCY_JITTER_MS=150

latency-proxy-ack-lag:
	$(MAKE) latency-proxy LATENCY_CLIENT_MS=20 LATENCY_SERVER_MS=20 LATENCY_WORLD_PLAYERS_MS=550 LATENCY_INPUT_APPLIED_MS=550 LATENCY_JITTER_MS=10

RTC_GATEWAY_LISTEN ?= 127.0.0.1:19093
RTC_GATEWAY_UPSTREAM ?= ws://127.0.0.1:9091/ws

rtc-gateway:
	npm run rtc-gateway -- --listen=$(RTC_GATEWAY_LISTEN) --upstream=$(RTC_GATEWAY_UPSTREAM)

test-rtc-gateway:
	npm run test:rtc-gateway

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
