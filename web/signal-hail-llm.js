(function () {
  "use strict";

  var params = new URLSearchParams(window.location.search);
  if (params.get("hailLlm") !== "1") return;

  var workerUrl = params.get("hailWorker") || "./signal-hail-gguf-worker.js";
  var modelUrl = params.get("hailModel") || "";
  var runtimeUrl = params.get("hailRuntime") || "./signal-hail-wllama-runtime.js";
  var wasmUrl = params.get("hailWasm") || "./wllama/esm/wasm/wllama.wasm";
  var nGpuLayers = Number(params.get("hailGpuLayers") || "0");
  var nThreads = Number(params.get("hailThreads") || "");
  var mock = params.get("hailMock") === "1";
  var pollMs = 180;
  var lastRequestId = 0;
  var inFlightId = 0;
  var worker = null;
  var workerReady = false;
  var api = null;

  function log(message) {
    console.log("[hail-llm] " + message);
  }

  function initApi() {
    var Module = window.SignalGameModule;
    if (!Module || !Module.cwrap) return false;
    api = {
      requestId: Module.cwrap("signal_hail_llm_request_id", "number", []),
      promptLen: Module.cwrap("signal_hail_llm_prompt_len", "number", []),
      prompt: Module.cwrap("signal_hail_llm_prompt", "string", []),
      apply: Module.cwrap("signal_hail_llm_apply_response", "number", ["string"])
    };
    return true;
  }

  function ensureWorker() {
    if (worker) return true;
    try {
      worker = new Worker(workerUrl, { type: "module" });
    } catch (err) {
      log("worker unavailable: " + err.message);
      return false;
    }
    worker.onmessage = function (ev) {
      var msg = ev.data || {};
      if (msg.type === "ready") {
        workerReady = true;
        log("worker ready" +
            (typeof msg.loadMs === "number" ? " (" + msg.loadMs + "ms)" : ""));
      } else if (msg.type === "choice") {
        if (msg.id !== inFlightId) return;
        inFlightId = 0;
        if (typeof msg.text === "string" && msg.text.length > 0) {
          var applied = api.apply(msg.text);
          log("applied " + applied + " choice(s)" +
              (typeof msg.elapsedMs === "number" ? " in " + msg.elapsedMs + "ms" : "") +
              ": " + msg.text);
        }
      } else if (msg.type === "error") {
        if (msg.id === inFlightId) inFlightId = 0;
        log(msg.error || "worker error");
      }
    };
    worker.onerror = function (err) {
      log("worker error: " + (err.message || "unknown"));
      inFlightId = 0;
      workerReady = false;
    };
    worker.postMessage({
      type: "init",
      modelUrl: modelUrl,
      runtimeUrl: runtimeUrl,
      wasmUrl: wasmUrl,
      mock: mock,
      temperature: 0.35,
      maxTokens: 32,
      nGpuLayers: Number.isFinite(nGpuLayers) ? nGpuLayers : 0,
      nThreads: Number.isFinite(nThreads) ? nThreads : undefined
    });
    return true;
  }

  function poll() {
    if (!api && !initApi()) {
      window.setTimeout(poll, pollMs);
      return;
    }
    if (!ensureWorker()) return;
    if (!workerReady) {
      window.setTimeout(poll, pollMs);
      return;
    }

    var id = api.requestId() >>> 0;
    if (id !== 0 && id !== lastRequestId && inFlightId === 0) {
      var len = api.promptLen() | 0;
      var prompt = api.prompt();
      lastRequestId = id;
      if (len > 0 && prompt) {
        inFlightId = id;
        log("dispatch hail prompt " + id + " (" + len + " chars)");
        worker.postMessage({
          type: "hail-choice",
          id: id,
          prompt: prompt,
          temperature: 0.35,
          maxTokens: 32
        });
      }
    }
    window.setTimeout(poll, pollMs);
  }

  function start() {
    log("enabled");
    poll();
  }

  var Module = window.SignalGameModule;
  if (Module) {
    var oldPostRun = Module.postRun;
    var hooks = Array.isArray(oldPostRun)
      ? oldPostRun
      : oldPostRun ? [oldPostRun] : [];
    hooks.push(start);
    Module.postRun = hooks;
  } else {
    window.addEventListener("load", start);
  }
})();
