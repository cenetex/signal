#!/usr/bin/env node

import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import vm from "node:vm";

function createWorkerHarness() {
  const posted = [];
  const context = {
    console,
    self: {
      postMessage(message) {
        posted.push(message);
      },
    },
  };
  context.self.self = context.self;
  vm.createContext(context);
  vm.runInContext(
    readFileSync(new URL("../web/signal-hail-gguf-worker.js", import.meta.url), "utf8"),
    context,
    { filename: "signal-hail-gguf-worker.js" },
  );
  return { worker: context.self, posted };
}

function plain(value) {
  return JSON.parse(JSON.stringify(value));
}

test("GGUF worker mock returns compact choices for real hail prompt shape", async () => {
  const { worker, posted } = createWorkerHarness();
  worker.onmessage({
    data: {
      type: "init",
      mock: true,
      modelUrl: "",
      temperature: 0.35,
      maxTokens: 24,
    },
  });
  await Promise.resolve();
  const ready = plain(posted.shift());
  assert.equal(typeof ready.loadMs, "number");
  delete ready.loadMs;
  assert.deepEqual(ready, {
    type: "ready",
    hasRuntime: false,
    mock: true,
  });

  worker.onmessage({
    data: {
      type: "hail-choice",
      id: 7,
      prompt: [
        "local hail choices",
        "YOU:",
        "1 Open hail; local traffic check.",
        "2 Local traffic, sound off.",
        "3 Open channel; nearby traffic check.",
        "MINER N00:",
        "1 FE pressure hot at Prospect Ref.",
        "2 Prospect Ref FE face is active.",
        "3 FE pressure mark near Prospect Ref.",
        "HAULER N02:",
        "1 FR demand at Kepler Yard; Helios Works-side.",
        "2 108 FR open at Kepler Yard; en route.",
        "3 FR boards at Kepler Yard; Helios Works wants it.",
        "Return all speaker keys. No words.",
        "Example: YOU=1,N00=2,N02=2",
        "ANSWER:",
      ].join("\n"),
    },
  });
  await Promise.resolve();

  const choice = plain(posted.shift());
  assert.equal(typeof choice.elapsedMs, "number");
  delete choice.elapsedMs;
  assert.deepEqual(choice, {
    type: "choice",
    id: 7,
    text: "YOU=1,N00=2,N02=3",
  });
});

test("GGUF worker reports missing runtime when mock is disabled", async () => {
  const { worker, posted } = createWorkerHarness();
  worker.onmessage({ data: { type: "init", mock: false, runtimeUrl: "" } });
  await Promise.resolve();
  const ready = plain(posted.shift());
  assert.equal(typeof ready.loadMs, "number");
  delete ready.loadMs;
  assert.deepEqual(ready, {
    type: "ready",
    hasRuntime: false,
    mock: false,
  });

  worker.onmessage({
    data: {
      type: "hail-choice",
      id: 8,
      prompt: "local hail choices\nYOU:\n",
    },
  });
  await Promise.resolve();

  assert.deepEqual(plain(posted.shift()), {
    type: "error",
    id: 8,
    error: "GGUF runtime not installed for hail worker",
  });
});
