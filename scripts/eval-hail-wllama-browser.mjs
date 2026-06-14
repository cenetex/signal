#!/usr/bin/env node

import { chromium } from "playwright";

import {
  SAMPLE_HAIL_CONTEXT,
  buildChoiceBatchPrompt,
  buildConversationPlan,
  realHailContextFromNpcSnapshot,
} from "./hail-conversation-smollm2.mjs";

function parseArgs(argv) {
  const args = {
    base: "http://127.0.0.1:8082",
    model: "/models/SmolLM2-135M-Instruct-Q4_K_M.gguf",
    maxSpeakers: 3,
    temperature: 0.35,
    maxTokens: 32,
    repeats: 1,
    input: "",
    playerX: 0,
    playerY: 0,
  };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === "--help" || arg === "-h") {
      console.log(`Usage: node scripts/eval-hail-wllama-browser.mjs [options]

Options:
  --base URL         Static build-web server (default: ${args.base})
  --model URL        GGUF model URL relative to base (default: ${args.model})
  --max-speakers N   Max speakers including player (default: ${args.maxSpeakers})
  --temperature N    Sampling temperature (default: ${args.temperature})
  --max-tokens N     Max generated tokens (default: ${args.maxTokens})
  --repeats N        Number of generations after model load (default: ${args.repeats})
  --input FILE       NPC chatter snapshot JSON to evaluate instead of sample
  --player-x N       Player X for snapshot distance sorting
  --player-y N       Player Y for snapshot distance sorting
`);
      process.exit(0);
    }
    if (!arg.startsWith("--")) throw new Error(`unknown argument: ${arg}`);
    const key = arg.slice(2).replace(/-([a-z])/g, (_, c) => c.toUpperCase());
    const value = argv[++i];
    if (value == null) throw new Error(`missing value for ${arg}`);
    args[key] = value;
  }
  args.maxSpeakers = Number(args.maxSpeakers);
  args.temperature = Number(args.temperature);
  args.maxTokens = Number(args.maxTokens);
  args.repeats = Number(args.repeats);
  args.playerX = Number(args.playerX);
  args.playerY = Number(args.playerY);
  return args;
}

function htmlPage() {
  return `<!doctype html>
<meta charset="utf-8">
<title>Signal hail Wllama eval</title>
<body>hail eval</body>`;
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  let context = SAMPLE_HAIL_CONTEXT;
  if (args.input) {
    const fs = await import("node:fs/promises");
    const parsed = JSON.parse(await fs.readFile(args.input, "utf8"));
    context = parsed.npcs && !parsed.player
      ? realHailContextFromNpcSnapshot(parsed, {
          x: args.playerX,
          y: args.playerY,
        })
      : parsed;
  }
  const plan = buildConversationPlan(context, args.maxSpeakers - 1);
  const prompt = buildChoiceBatchPrompt(plan);
  const expected = plan.map((speaker) => speaker.kind === "player"
    ? "YOU"
    : `N${String(speaker.slot).padStart(2, "0")}`);

  const browser = await chromium.launch({ headless: true });
  const page = await browser.newPage();
  await page.route(`${args.base}/hail-eval.html`, (route) => {
    route.fulfill({
      status: 200,
      contentType: "text/html",
      body: htmlPage(),
    });
  });

  const logs = [];
  page.on("console", (msg) => logs.push({ type: msg.type(), text: msg.text() }));
  page.on("pageerror", (err) => logs.push({ type: "pageerror", text: err.message }));
  await page.goto(`${args.base}/hail-eval.html`, { waitUntil: "load" });

  const result = await page.evaluate(async ({ model, prompt, temperature, maxTokens, repeats }) => {
    const module = await import("/signal-hail-wllama-runtime.js");
    const loadStart = performance.now();
    const runtime = await module.SignalHailGgufRuntime.create({
      modelUrl: model,
      nGpuLayers: 0,
    });
    const loadMs = Math.round(performance.now() - loadStart);
    const generations = [];
    for (let i = 0; i < repeats; i++) {
      const started = performance.now();
      const text = await runtime.generateChoice({
        prompt,
        temperature,
        maxTokens,
      });
      generations.push({
        text,
        elapsedMs: Math.round(performance.now() - started),
      });
    }
    return { loadMs, generations };
  }, {
    model: args.model,
    prompt,
    temperature: args.temperature,
    maxTokens: args.maxTokens,
    repeats: args.repeats,
  });

  await browser.close();

  const validKeySet = new Set(expected);
  const generations = result.generations.map((generation) => {
    const keys = Array.from(generation.text.matchAll(/\b(YOU|N\d{2})=/g),
      (match) => match[1]);
    return {
      ...generation,
      keys,
      expected,
      allKeysAllowed: keys.every((key) => validKeySet.has(key)),
      hasPlayer: keys.includes("YOU"),
    };
  });

  console.log(JSON.stringify({
    model: args.model,
    promptChars: prompt.length,
    expected,
    loadMs: result.loadMs,
    generations,
    browserLogTail: logs.slice(-10),
  }, null, 2));
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
