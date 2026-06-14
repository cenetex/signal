#!/usr/bin/env node

import {
  SAMPLE_HAIL_CONTEXT,
  buildChoiceBatchPrompt,
  buildConversationPlan,
  choiceKeyForSpeaker,
  realHailContextFromNpcSnapshot,
  rotatedChoiceCandidatesForSpeaker,
  speakerRadioLineIssues,
} from "./hail-conversation-smollm2.mjs";

function parseArgs(argv) {
  const args = {
    input: "",
    server: "",
    maxSpeakers: 5,
    playerX: 0,
    playerY: 0,
    hailSalt: 1,
    saltCount: 1,
    maxPromptChars: 900,
    minDistinctNpcLines: 3,
    minRepeatedNpcTranscripts: 1,
    json: false,
  };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === "--help" || arg === "-h") {
      console.log(`Usage: node scripts/eval-hail-choice-quality.mjs [options]

Options:
  --input FILE             NPC chatter snapshot JSON
  --server URL             Fetch /api/npc_chatter_context from a live server
  --max-speakers N         Speakers including player (default: ${args.maxSpeakers})
  --player-x N             Player X for snapshot distance sorting
  --player-y N             Player Y for snapshot distance sorting
  --hail-salt N            Request id/salt to evaluate (default: ${args.hailSalt})
  --salt-count N           Evaluate N consecutive request salts (default: ${args.saltCount})
  --max-prompt-chars N     Fail above this prompt size (default: ${args.maxPromptChars})
  --min-distinct-npc-lines N
                           Fail below this many distinct resolved NPC lines
  --min-repeated-npc-transcripts N
                           Fail if repeated salts produce fewer variants
  --json                   Emit JSON only
`);
      process.exit(0);
    }
    if (arg === "--json") {
      args.json = true;
      continue;
    }
    if (!arg.startsWith("--")) throw new Error(`unknown argument: ${arg}`);
    const key = arg.slice(2).replace(/-([a-z])/g, (_, c) => c.toUpperCase());
    const value = argv[++i];
    if (value == null) throw new Error(`missing value for ${arg}`);
    args[key] = value;
  }
  args.maxSpeakers = Number(args.maxSpeakers);
  args.playerX = Number(args.playerX);
  args.playerY = Number(args.playerY);
  args.hailSalt = Number(args.hailSalt);
  args.saltCount = Number(args.saltCount);
  args.maxPromptChars = Number(args.maxPromptChars);
  args.minDistinctNpcLines = Number(args.minDistinctNpcLines);
  args.minRepeatedNpcTranscripts = Number(args.minRepeatedNpcTranscripts);
  return args;
}

async function fetchJson(url) {
  const res = await fetch(url);
  const text = await res.text();
  if (!res.ok) {
    throw new Error(`HTTP ${res.status} from ${url}: ${text.slice(0, 500)}`);
  }
  return JSON.parse(text);
}

async function readContext(args) {
  if (args.server) {
    const url = new URL("/api/npc_chatter_context", args.server);
    url.searchParams.set("limit", String(Math.max(1, args.maxSpeakers - 1)));
    const snapshot = await fetchJson(url);
    return realHailContextFromNpcSnapshot(snapshot, {
      x: args.playerX,
      y: args.playerY,
    });
  }
  if (args.input) {
    const fs = await import("node:fs/promises");
    const parsed = JSON.parse(await fs.readFile(args.input, "utf8"));
    if (parsed.npcs && !parsed.player) {
      return realHailContextFromNpcSnapshot(parsed, {
        x: args.playerX,
        y: args.playerY,
      });
    }
    return parsed;
  }
  return SAMPLE_HAIL_CONTEXT;
}

function exampleChoicesFromPrompt(prompt) {
  const match = String(prompt || "").match(/\bExample:\s*([^\n]+)/);
  const choices = new Map();
  if (!match) return choices;
  for (const part of match[1].split(",")) {
    const assignment = part.trim().match(/^(YOU|N\d{2})=(\d)$/);
    if (assignment) choices.set(assignment[1], Number(assignment[2]));
  }
  return choices;
}

function summarizeMemoryKinds(plan) {
  const counts = new Map();
  for (const speaker of plan) {
    if (speaker.kind !== "npc") continue;
    for (const memory of speaker.market_memories || []) {
      const key = memory.kind_name || "unknown";
      counts.set(key, (counts.get(key) || 0) + 1);
    }
  }
  return Object.fromEntries([...counts.entries()].sort());
}

function evaluateSalt(context, args, salt) {
  context.hail_request_id = salt;
  const plan = buildConversationPlan(context, Math.max(0, args.maxSpeakers - 1));
  const prompt = buildChoiceBatchPrompt(plan, salt);
  const choices = exampleChoicesFromPrompt(prompt);

  const records = plan.map((speaker) => {
    const key = choiceKeyForSpeaker(speaker);
    const candidates = rotatedChoiceCandidatesForSpeaker(speaker, salt);
    const choice = choices.get(key);
    const line = choice >= 1 && choice <= candidates.length
      ? candidates[choice - 1]
      : "";
    return {
      key,
      speaker: speaker.kind === "player" ? "YOU" : `${speaker.role} N${String(speaker.slot).padStart(2, "0")}`,
      role: speaker.role || "player",
      choice,
      line,
      issues: speaker.kind === "npc" ? speakerRadioLineIssues(line, speaker) : [],
      memories: (speaker.market_memories || []).map((memory) => ({
        kind: memory.kind_name || "unknown",
        commodity: memory.commodity_code || "",
        station_a: memory.station_a_name || memory.station_name || "",
        station_b: memory.station_b_name || "",
      })),
    };
  });

  const npcLines = records
    .filter((record) => record.key !== "YOU")
    .map((record) => record.line)
    .filter(Boolean);
  const distinctNpcLineCount = new Set(npcLines).size;
  const issueCounts = new Map();
  for (const record of records) {
    for (const issue of record.issues) {
      issueCounts.set(issue, (issueCounts.get(issue) || 0) + 1);
    }
  }

  return {
    hailSalt: salt,
    promptChars: prompt.length,
    maxPromptChars: args.maxPromptChars,
    speakerCount: plan.length,
    npcCount: Math.max(0, plan.length - 1),
    memoryKinds: summarizeMemoryKinds(plan),
    distinctNpcLineCount,
    minDistinctNpcLines: args.minDistinctNpcLines,
    issueCounts: Object.fromEntries([...issueCounts.entries()].sort()),
    prompt,
    records,
  };
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const context = await readContext(args);
  const saltCount = Math.max(1, args.saltCount);
  const runs = [];
  for (let i = 0; i < saltCount; i++) {
    runs.push(evaluateSalt(structuredClone(context), args, args.hailSalt + i));
  }
  const result = runs[0];
  result.runs = runs;
  result.repeatedNpcTranscriptCount = new Set(runs.map((run) =>
    run.records
      .filter((record) => record.key !== "YOU")
      .map((record) => record.line)
      .join(" | "))).size;
  result.minRepeatedNpcTranscripts = args.minRepeatedNpcTranscripts;

  if (args.json) {
    console.log(JSON.stringify(result, null, 2));
  } else {
    console.log(`prompt=${result.promptChars}/${result.maxPromptChars} chars speakers=${result.speakerCount} npc=${result.npcCount}`);
    console.log(`distinct_npc_lines=${result.distinctNpcLineCount}/${result.minDistinctNpcLines}`);
    if (saltCount > 1)
      console.log(`repeated_npc_transcripts=${result.repeatedNpcTranscriptCount}/${result.minRepeatedNpcTranscripts}`);
    console.log(`memory_kinds=${JSON.stringify(result.memoryKinds)}`);
    console.log(`issues=${JSON.stringify(result.issueCounts)}`);
    for (const record of result.records) {
      console.log(`${record.key} ${record.speaker}: ${record.line || "(missing)"}`);
    }
  }

  for (const run of runs) {
    if (run.promptChars > args.maxPromptChars)
      throw new Error(`hail prompt too long: ${run.promptChars} > ${args.maxPromptChars}`);
    if (run.npcCount > 0 && run.distinctNpcLineCount < args.minDistinctNpcLines)
      throw new Error(`not enough distinct NPC lines: ${run.distinctNpcLineCount} < ${args.minDistinctNpcLines}`);
    if (Object.keys(run.issueCounts).length > 0)
      throw new Error(`grounding issues: ${JSON.stringify(run.issueCounts)}`);
  }
  if (saltCount > 1 &&
      result.repeatedNpcTranscriptCount < args.minRepeatedNpcTranscripts) {
    throw new Error(`not enough repeated hail variants: ${result.repeatedNpcTranscriptCount} < ${args.minRepeatedNpcTranscripts}`);
  }
}

main().catch((err) => {
  console.error(err.message);
  process.exit(1);
});
