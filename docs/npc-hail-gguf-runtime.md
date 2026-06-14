# NPC Hail GGUF Runtime

Goal: run the same small local model for native and browser hails without
requiring Ollama. The game should keep using the bounded hail-choice protocol:

```text
local hail choices
YOU:
1 Local traffic, sound off.
2 Open channel; nearby traffic check.
3 Open hail; local traffic check.
MINER N00:
1 Prospect Ref FE seam is talking.
2 FE pressure mark holding near Prospect Ref.
3 FE pressure bright at Prospect Ref.
Example: YOU=2,N00=3
ANSWER:
```

The model only chooses among grounded candidates. C owns the authoritative
candidate generation and response parser; the LLM backend only returns compact
choice text such as `YOU=1,N00=2,N01=3`.
Candidate order and the example digits vary deterministically by hail request,
so even a tiny model that strongly follows the example produces different
grounded lines across repeated hails.

Candidate families are keyed by structured memory kind, including demand,
supply, route danger/risk, route success/reputation, delivery receipts,
station trust/risk, ore pressure, and scaffold pressure. The line still names
the known station, route, commodity, or module instead of letting the model
invent them.

## Shape

- `shared/npc_radio.c` builds the player/NPC choices and applies responses.
- `client/input.c` stores the exact prompt in `g.hail_choice_prompt` on `H`.
- `signal_hail_llm_request_id()`, `signal_hail_llm_prompt_len()`, and
  `signal_hail_llm_prompt()` expose fresh prompts to browser JS.
- `signal_hail_llm_apply_response(const char *response)` applies an async
  backend response to the current player/NPC hail lines.
- The backend may be native llama.cpp, browser llama.cpp/WASM, or a web worker
  wrapper. It should not need access to world state beyond the prompt string.

## Browser Path

Use a separate worker-hosted GGUF runtime rather than linking a second LLM
runtime into the main game WASM. That keeps the frame loop responsive and lets
the LLM use its own WASM build settings.

Recommended worker contract:

```js
// Request from the game shell or an EM_JS shim.
worker.postMessage({
  type: "hail-choice",
  id,
  prompt,
  maxTokens: 32,
  temperature: 0.35
});

// Response from the worker.
Module.ccall("signal_hail_llm_apply_response", "number", ["string"], [
  "YOU=1,N00=2,N01=3"
]);
```

The browser harness lives in `web/signal-hail-llm.js` and is disabled by
default. Enable it with:

```text
play.html?hailLlm=1&hailModel=/models/smollm2.gguf
```

By default it loads `web/signal-hail-gguf-worker.js`, which imports
`web/signal-hail-wllama-runtime.js` when mock mode is off. That runtime uses
`@wllama/wllama` to load a GGUF model directly inside the browser worker and
return compact choice text. The runtime derives a tiny grammar from the active
speaker keys, so SmolLM2 is constrained to emit only strings such as
`YOU=1,N00=2,N01=3`.

For deterministic browser smoke tests without a model, use:

```text
play.html?singleplayer=1&hailLlm=1&hailMock=1
```

For real browser GGUF inference, serve a small quantized GGUF and pass it in:

```text
play.html?singleplayer=1&hailLlm=1&hailModel=/models/smollm2-q4.gguf
```

Optional query parameters:

- `hailRuntime=...` overrides the runtime module URL.
- `hailWasm=...` overrides the Wllama `.wasm` asset URL.
- `hailGpuLayers=...` enables WebGPU layer offload when supported.
- `hailThreads=...` sets the Wllama thread count.

The local dev HTTP server already sends COOP/COEP headers for
`SharedArrayBuffer`, which multi-thread WASM inference may need. If the model
runtime requires pthreads, keep that requirement inside the worker runtime
rather than changing the main game WASM first.

## Native Path

Use the same prompt/response bridge. Native can either:

- link llama.cpp directly behind a small `hail_llm_backend` module, or
- run the same Wllama worker/runtime in a desktop web shell.

Do not make the hail path depend on a blocking HTTP call. Native inference
should run on a background thread or pollable job and call
`signal_hail_llm_apply_response` on completion.

## Model Asset

Use a quantized SmolLM2 GGUF small enough to be cached by the browser. Serve it
as a static model asset or split it into chunks if the runtime supports
parallel loading and OPFS caching.

Selection criteria:

- short completion latency for one 500-900 byte prompt;
- stable support for GGUF in browser WASM;
- permissive deployment story for the chosen quantized file;
- enough quality for style selection, not open-ended authorship.

## Guardrails

- Keep candidate choice generation in C.
- Cap generation to about 32 tokens for the grammar-constrained full key list.
- Treat invalid responses as no-op; fallback lines are already present.
- Prefer deterministic or low-temperature sampling.
- Never let the backend invent new stations, commodities, or contracts.

## Quality Checks

Use the choice-quality evaluator to check prompt size, grounding,
per-hail distinctness, and repeated-hail variety without invoking a model:

```sh
node scripts/eval-hail-choice-quality.mjs \
  --max-speakers 5 \
  --hail-salt 1 \
  --salt-count 3 \
  --min-repeated-npc-transcripts 3
```

For saved or live NPC snapshots, pass `--input snapshot.json` or
`--server http://127.0.0.1:8080`. The browser GGUF evaluator accepts the same
saved snapshot through `--input`.

For the full browser-worker path with the real GGUF, run sequential hails and
require the resolved NPC transcript to vary:

```sh
node scripts/smoke-live-hail-wllama.mjs \
  --timeout-ms 90000 \
  --repeats 3 \
  --min-repeated-npc-transcripts 3
```
