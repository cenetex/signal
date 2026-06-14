#!/usr/bin/env node

import { chromium } from "playwright";

function parseArgs(argv) {
  const args = {
    base: "http://127.0.0.1:8082",
    model: "/models/SmolLM2-135M-Instruct-Q4_K_M.gguf",
    timeoutMs: 60000,
    repeats: 1,
    minRepeatedNpcTranscripts: 1,
  };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === "--help" || arg === "-h") {
      console.log(`Usage: node scripts/smoke-live-hail-wllama.mjs [options]

Options:
  --base URL       Static build-web server (default: ${args.base})
  --model URL      GGUF model URL relative to base (default: ${args.model})
  --timeout-ms N   Wait for SmolLM2 response (default: ${args.timeoutMs})
  --repeats N      Trigger N sequential hails (default: ${args.repeats})
  --min-repeated-npc-transcripts N
                   Fail if repeated hails produce fewer NPC transcript variants
`);
      process.exit(0);
    }
    if (!arg.startsWith("--")) throw new Error(`unknown argument: ${arg}`);
    const key = arg.slice(2).replace(/-([a-z])/g, (_, c) => c.toUpperCase());
    const value = argv[++i];
    if (value == null) throw new Error(`missing value for ${arg}`);
    args[key] = value;
  }
  args.timeoutMs = Number(args.timeoutMs);
  args.repeats = Number(args.repeats);
  args.minRepeatedNpcTranscripts = Number(args.minRepeatedNpcTranscripts);
  return args;
}

async function waitForHailLog(logs, timeoutMs, startIndex = 0) {
  const started = Date.now();
  while (Date.now() - started < timeoutMs) {
    const applied = logs.slice(startIndex).find((entry) =>
      entry.text.includes("[hail-llm] applied"));
    if (applied) return applied;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`timed out waiting for hail apply log after ${timeoutMs}ms`);
}

function choicesFromApplied(text) {
  const choices = new Map();
  for (const match of text.matchAll(/\b(YOU|N\d{2})=(\d)\b/g)) {
    choices.set(match[1], Number(match[2]));
  }
  return choices;
}

function promptChoices(prompt) {
  const sections = new Map();
  let current = "";
  for (const line of String(prompt || "").split(/\r?\n/)) {
    const header = line.match(/^(YOU|(?:MINER|HAULER|WORKER)\s+(N\d{2})):/);
    if (header) {
      current = header[1] === "YOU" ? "YOU" : header[2];
      sections.set(current, []);
      continue;
    }
    const choice = line.match(/^([1-3])\s+(.+)$/);
    if (current && choice) sections.get(current)?.push(choice[2]);
  }
  return sections;
}

function resolveAppliedLines(prompt, appliedText) {
  const sections = promptChoices(prompt);
  const choices = choicesFromApplied(appliedText);
  const resolved = [];
  for (const [key, choice] of choices.entries()) {
    const lines = sections.get(key) || [];
    resolved.push({
      key,
      choice,
      line: lines[choice - 1] || "",
    });
  }
  return resolved;
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const browser = await chromium.launch({ headless: true });
  const page = await browser.newPage({ viewport: { width: 1280, height: 720 } });
  const logs = [];
  page.on("console", (msg) => logs.push({ type: msg.type(), text: msg.text() }));
  page.on("pageerror", (err) => logs.push({ type: "pageerror", text: err.message }));

  const url = `${args.base}/play.html?singleplayer=1&hailLlm=1&hailModel=${encodeURIComponent(args.model)}&v=${Date.now()}`;
  await page.goto(url, { waitUntil: "load" });
  await page.waitForFunction(
    () => document.getElementById("loading")?.classList.contains("done"),
    null,
    { timeout: 10000 },
  );
  await page.waitForFunction(
    () => !!(window.SignalGameModule &&
      window.SignalGameModule._signal_hail_llm_debug_seed_nearby &&
      window.SignalGameModule._signal_hail_llm_debug_trigger_hail),
    null,
    { timeout: 10000 },
  );

  const runs = [];
  let seeded = 0;
  for (let i = 0; i < Math.max(1, args.repeats); i++) {
    const logStart = logs.length;
    const setup = await page.evaluate(() => {
      const module = window.SignalGameModule;
      const seeded = module._signal_hail_llm_debug_seed_nearby();
      const conversationCount = module._signal_hail_llm_debug_trigger_hail();
      return {
        seeded,
        conversationCount,
        requestId: module._signal_hail_llm_request_id(),
        promptLen: module._signal_hail_llm_prompt_len(),
        prompt: module.cwrap("signal_hail_llm_prompt", "string", [])(),
      };
    });
    seeded = setup.seeded;
    if (setup.seeded < 4) throw new Error(`expected 4 seeded NPCs, got ${setup.seeded}`);
    if (setup.conversationCount < 4)
      throw new Error(`expected 4 hail NPCs, got ${setup.conversationCount}\n${setup.prompt}`);

    const applied = await waitForHailLog(logs, args.timeoutMs, logStart);
    const keys = Array.from(applied.text.matchAll(/\b(YOU|N\d{2})=/g),
      (match) => match[1]);
    const expectedKeys = Array.from(promptChoices(setup.prompt).keys());
    const resolvedLines = resolveAppliedLines(setup.prompt, applied.text);
    const npcResolvedLines = resolvedLines
      .filter((entry) => entry.key !== "YOU")
      .map((entry) => entry.line)
      .filter(Boolean);
    const distinctNpcLines = new Set(npcResolvedLines);
    runs.push({
      trigger: setup,
      applied: applied.text,
      keys,
      expectedKeys,
      resolvedLines,
      distinctNpcLineCount: distinctNpcLines.size,
      npcTranscript: npcResolvedLines.join(" | "),
      hasPlayer: keys.includes("YOU"),
      npcKeyCount: keys.filter((key) => key !== "YOU").length,
      expectedNpcKeyCount: expectedKeys.filter((key) => key !== "YOU").length,
    });
  }

  const repeatedNpcTranscriptCount =
    new Set(runs.map((run) => run.npcTranscript)).size;
  const first = runs[0];

  await browser.close();
  console.log(JSON.stringify({
    url,
    seeded,
    trigger: first.trigger,
    applied: first.applied,
    keys: first.keys,
    resolvedLines: first.resolvedLines,
    distinctNpcLineCount: first.distinctNpcLineCount,
    repeatedNpcTranscriptCount,
    hasPlayer: first.hasPlayer,
    npcKeyCount: first.npcKeyCount,
    runs,
    hailLogs: logs.filter((entry) => entry.text.includes("[hail-llm]")),
  }, null, 2));

  for (const run of runs) {
    if (!run.hasPlayer) throw new Error("hail response missing YOU key");
    for (const key of run.expectedKeys) {
      if (!run.keys.includes(key))
        throw new Error(`hail response missing expected key ${key}`);
    }
    if (run.npcKeyCount < run.expectedNpcKeyCount)
      throw new Error("hail response missing one or more prompted NPC keys");
    const minDistinct = Math.min(3, run.expectedNpcKeyCount);
    if (run.distinctNpcLineCount < minDistinct)
      throw new Error(`expected varied NPC hail lines, got ${run.distinctNpcLineCount}`);
  }
  if (args.repeats > 1 &&
      repeatedNpcTranscriptCount < args.minRepeatedNpcTranscripts) {
    throw new Error(`expected repeated hail variation, got ${repeatedNpcTranscriptCount}`);
  }
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
