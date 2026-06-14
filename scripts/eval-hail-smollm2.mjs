#!/usr/bin/env node

import {
  generateConversation,
  radioLineIssues,
  realHailContextFromNpcSnapshot,
} from "./hail-conversation-smollm2.mjs";

const DEFAULT_INPUT = "/tmp/signal-real-hail-sample/npc-context.json";

function parseArgs(argv) {
  const args = {
    input: DEFAULT_INPUT,
    model: "smollm2",
    ollama: "http://127.0.0.1:11434",
    runs: 3,
    maxSpeakers: 4,
    temperature: 0.45,
    playerX: 0,
    playerY: 0,
    variants: ["choice", "transcript", "terse", "ledger", "bulletin", "exemplars"],
  };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === "--variants") {
      args.variants = argv[++i].split(",").filter(Boolean);
    } else if (arg === "--no-batch-choice") {
      args.batchChoice = false;
    } else if (arg.startsWith("--")) {
      const key = arg.slice(2).replace(/-([a-z])/g, (_, c) => c.toUpperCase());
      const value = argv[++i];
      if (value == null) throw new Error(`missing value for ${arg}`);
      args[key] = value;
    } else {
      throw new Error(`unknown argument: ${arg}`);
    }
  }
  args.runs = Number(args.runs);
  args.maxSpeakers = Number(args.maxSpeakers);
  args.temperature = Number(args.temperature);
  args.playerX = Number(args.playerX);
  args.playerY = Number(args.playerY);
  if (args.batchChoiceRetries != null)
    args.batchChoiceRetries = Number(args.batchChoiceRetries);
  return args;
}

function scoreLine(line) {
  const text = (line || "").trim();
  let score = 0;
  const issues = radioLineIssues(text);
  score += issues.length * 3;
  if (issues.includes("long")) score += 2;
  if (/\b(FE|FR|LM|RK|Prospect|Kepler|Helios|hauler|miner|cargo|ore|route|yard|works)\b/i.test(text)) {
    score -= 1;
  }
  return { score, issues };
}

function summarizeRecords(records) {
  const fallbackCount = records.filter((record) => record.used_fallback).length;
  const acceptedCount = records.length - fallbackCount;
  const promptChars = records.reduce((sum, record) => sum + (record.prompt_chars || 0), 0);
  const generation = records.generation || {};
  const issueCounts = new Map();
  for (const record of records) {
    for (const issue of record.issues || []) {
      issueCounts.set(issue, (issueCounts.get(issue) || 0) + 1);
    }
  }
  return {
    fallbackCount,
    acceptedCount,
    promptChars: generation.prompt_chars || promptChars,
    llmCalls: generation.llm_calls || 0,
    generationMode: generation.mode || "unknown",
    issueCounts,
  };
}

async function readSnapshot(path) {
  const fs = await import("node:fs/promises");
  return JSON.parse(await fs.readFile(path, "utf8"));
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const snapshot = await readSnapshot(args.input);
  const baseContext = realHailContextFromNpcSnapshot(snapshot, {
    x: args.playerX,
    y: args.playerY,
  });

  for (const variant of args.variants) {
    let total = 0;
    let count = 0;
    let accepted = 0;
    let fallback = 0;
    let promptChars = 0;
    let llmCalls = 0;
    const generationModes = new Map();
    const issueCounts = new Map();
    console.log(`\n=== ${variant} ===`);
    for (let run = 0; run < args.runs; run++) {
      const context = structuredClone(baseContext);
      context.prompt_variant = variant;
      const records = await generateConversation(context, args);
      const summary = summarizeRecords(records);
      accepted += summary.acceptedCount;
      fallback += summary.fallbackCount;
      promptChars += summary.promptChars;
      llmCalls += summary.llmCalls;
      generationModes.set(
        summary.generationMode,
        (generationModes.get(summary.generationMode) || 0) + 1);
      for (const [issue, n] of summary.issueCounts) {
        issueCounts.set(issue, (issueCounts.get(issue) || 0) + n);
      }
      const rendered = records.map((record) => {
        const scored = scoreLine(record.text);
        total += scored.score;
        count++;
        const mark = record.used_fallback ? "fallback" : "accept";
        return `${record.speaker}[${mark}]: ${record.text || "(empty)"}`;
      });
      console.log(`run ${run + 1}: ${rendered.join(" | ")}`);
    }
    const avg = count ? total / count : 999;
    const acceptRate = count ? accepted / count : 0;
    const avgPromptChars = count ? promptChars / count : 0;
    const avgCalls = args.runs ? llmCalls / args.runs : 0;
    const modes = [...generationModes.entries()]
      .map(([mode, n]) => `${mode}:${n}`)
      .join(",") || "none";
    const issues = [...issueCounts.entries()]
      .map(([issue, n]) => `${issue}:${n}`)
      .join(", ") || "none";
    console.log(`score avg=${avg.toFixed(2)} accept=${accepted}/${count} (${(acceptRate * 100).toFixed(0)}%) fallback=${fallback} avg_prompt_chars=${avgPromptChars.toFixed(0)} avg_llm_calls=${avgCalls.toFixed(2)} modes=${modes} raw_issues=${issues}`);
  }
}

main().catch((err) => {
  console.error(err.message);
  process.exit(1);
});
