.PHONY: all build build-web build-server build-test test dev stop deploy clean

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
build-test:
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
	cmake --build build --target signal_test

test: build-test
	./build/signal_test

# --- Local dev (server on :9091, web on :8082) ---
dev: build-server build-web
	@echo "Starting local dev environment..."
	@pkill -f signal_server 2>/dev/null || true
	@pkill -f "python3 -m http.server 8082" 2>/dev/null || true
	@sleep 0.3
	PORT=9091 ./build/signal_server &
	python3 -m http.server 8082 --directory build-web &
	@sleep 0.5
	@echo ""
	@echo "  Server:  ws://localhost:9091/ws"
	@echo "  Client:  http://localhost:8082/signal.html?server=ws://localhost:9091/ws"
	@echo ""
	@echo "  make stop  to shut down"

stop:
	@pkill -f signal_server 2>/dev/null || true
	@pkill -f "python3 -m http.server 8082" 2>/dev/null || true
	@echo "Stopped."

# --- Deploy (triggers CI via push) ---
deploy:
	git push origin main

clean:
	rm -rf build build-web build-test
