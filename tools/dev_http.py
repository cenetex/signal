#!/usr/bin/env python3
"""
Local dev HTTP server for the Signal WASM client.

Same behavior as `python3 -m http.server`, plus:
- Cache-Control: no-store on every response so the browser always
  fetches the fresh signal.wasm / signal.js after a rebuild.
- Cross-origin isolation headers so SharedArrayBuffer-dependent
  features (audio worklet, threads) work in Chromium.

Usage:   python3 tools/dev_http.py [PORT] [--directory DIR]
Default: 8082, serving the current working directory.
"""
import argparse
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer


class NoCacheHandler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store, max-age=0")
        self.send_header("Pragma", "no-cache")
        # Needed for SharedArrayBuffer under Chromium's COOP/COEP rules.
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("port", nargs="?", type=int, default=8082)
    p.add_argument("--directory", default=".")
    args = p.parse_args()

    # SimpleHTTPRequestHandler honors the `directory` kwarg in 3.7+.
    def handler(*hargs, **hkwargs):
        return NoCacheHandler(*hargs, directory=args.directory, **hkwargs)

    srv = ThreadingHTTPServer(("0.0.0.0", args.port), handler)
    print(f"dev http: serving {args.directory} on :{args.port} with Cache-Control: no-store")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
