/* GGUF hail worker adapter.
 *
 * This file is intentionally a thin runtime seam. The game sends a compact
 * grounded choice prompt; the worker must return compact choice text such as
 * `YOU=1,N00=2,N01=3`. Wire a GGUF/WASM runtime here without changing game C.
 */
(function () {
  "use strict";

  var runtime = null;
  var config = {
    modelUrl: "",
    runtimeUrl: "./signal-hail-wllama-runtime.js",
    wasmUrl: "./wllama/esm/wasm/wllama.wasm",
    mock: false,
    temperature: 0.35,
    maxTokens: 32,
    nGpuLayers: 0,
    nThreads: undefined
  };

  function post(type, payload) {
    payload = payload || {};
    payload.type = type;
    self.postMessage(payload);
  }

  async function initRuntime(message) {
    var startedAt = Date.now();
    if (message.modelUrl !== undefined) config.modelUrl = message.modelUrl;
    if (message.runtimeUrl !== undefined) config.runtimeUrl = message.runtimeUrl;
    if (message.wasmUrl !== undefined) config.wasmUrl = message.wasmUrl;
    config.mock = !!message.mock;
    config.temperature = Number(message.temperature || config.temperature);
    config.maxTokens = Number(message.maxTokens || config.maxTokens);
    if (message.nGpuLayers !== undefined)
      config.nGpuLayers = Number(message.nGpuLayers);
    if (message.nThreads !== undefined)
      config.nThreads = Number(message.nThreads);

    if (!config.mock) {
      var runtimeApi = self.SignalHailGgufRuntime;
      if (!runtimeApi && config.runtimeUrl) {
        if (typeof self.document === "undefined")
          self.document = { baseURI: self.location.href };
        var mod = await import(config.runtimeUrl);
        runtimeApi = mod.SignalHailGgufRuntime;
      }
      if (runtimeApi && typeof runtimeApi.create === "function")
        runtime = await runtimeApi.create(config);
    }
    post("ready", {
      hasRuntime: !!runtime,
      mock: config.mock,
      loadMs: Date.now() - startedAt
    });
  }

  function mockChoice(prompt) {
    var ids = [];
    var seen = Object.create(null);
    String(prompt || "").split(/\r?\n/).forEach(function (line) {
      var match = line.match(/^(YOU|(?:MINER|HAULER|WORKER)\s+N\d{2}):$/);
      if (!match) return;
      var id = match[1] === "YOU"
        ? "YOU"
        : match[1].match(/N\d{2}/)[0];
      if (!seen[id]) {
        seen[id] = true;
        ids.push(id);
      }
    });
    return ids.map(function (id, index) {
      return id + "=" + String((index % 3) + 1);
    }).join(",");
  }

  async function generateChoice(message) {
    var startedAt = Date.now();
    if (config.mock) {
      post("choice", {
        id: message.id,
        text: mockChoice(message.prompt),
        elapsedMs: Date.now() - startedAt
      });
      return;
    }
    if (!runtime || typeof runtime.generateChoice !== "function") {
      post("error", {
        id: message.id,
        error: "GGUF runtime not installed for hail worker"
      });
      return;
    }
    var text = await runtime.generateChoice({
      prompt: message.prompt || "",
      temperature: Number(message.temperature || config.temperature),
      maxTokens: Number(message.maxTokens || config.maxTokens)
    });
    post("choice", {
      id: message.id,
      text: String(text || ""),
      elapsedMs: Date.now() - startedAt
    });
  }

  self.onmessage = function (ev) {
    var message = ev.data || {};
    if (message.type === "init") {
      initRuntime(message).catch(function (err) {
        post("error", { error: err && err.message ? err.message : String(err) });
      });
    } else if (message.type === "hail-choice") {
      generateChoice(message).catch(function (err) {
        post("error", {
          id: message.id,
          error: err && err.message ? err.message : String(err)
        });
      });
    }
  };
})();
