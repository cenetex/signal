#!/usr/bin/env node

import { createHash } from "node:crypto";
import { mkdir, writeFile } from "node:fs/promises";
import path from "node:path";

const DEFAULT_OUT_DIR = "corpus/ship-radio";

const SYSTEM_CHOICE = [
  "You are Signal's bounded ship-radio chooser.",
  "Return only compact speaker assignments such as YOU=1,N00=2.",
  "Use every speaker key from the prompt. Do not add words.",
].join(" ");

const SYSTEM_LINE = [
  "You are Signal ship radio.",
  "Speak in short grounded belt chatter.",
  "Use only the provided station, route, commodity, module, and memory facts.",
  "No greetings, no roleplay narration, no invented lore.",
].join(" ");

const STATIONS = [
  "Prospect Refinery",
  "Kepler Yard",
  "Helios Works",
  "Freeport",
];

const ORE = ["FE", "CU", "CR"];
const GOODS = ["FR", "CO", "LN", "FM", "LM", "TM", "RK"];
const MODULES = ["Frame Press", "Laser Fab", "Tractor Fab", "Signal Relay", "Shipyard"];

const SAMPLE_CONTEXT = makeContext("starter-sample", [
  npc({
    slot: 0,
    role: "miner",
    state: "return_to_station",
    home: "Prospect Refinery",
    dest: "Prospect Refinery",
    memory: memory("ore pressure", "FE", "Prospect Refinery", "", 84),
    contracts: [
      contract("haul", "Prospect Refinery", "RK", 1000, 0),
      contract("haul", "Prospect Refinery", "LM", 8, 38),
    ],
  }),
  npc({
    slot: 1,
    role: "miner",
    state: "travel_to_asteroid",
    home: "Prospect Refinery",
    dest: "Prospect Refinery",
    memory: memory("ore pressure", "FE", "Prospect Refinery", "", 92),
    contracts: [
      contract("delivery", "Kepler Yard", "FR", 4, 4),
    ],
  }),
  npc({
    slot: 2,
    role: "hauler",
    state: "travel_to_destination",
    home: "Kepler Yard",
    dest: "Helios Works",
    memory: memory("demand", "FR", "Kepler Yard", "", 108),
    contracts: [
      contract("haul", "Kepler Yard", "FR", 108, 4),
      contract("haul", "Kepler Yard", "LM", 12, 7),
    ],
  }),
  npc({
    slot: 3,
    role: "hauler",
    state: "travel_to_destination",
    home: "Kepler Yard",
    dest: "Helios Works",
    memory: memory("demand", "LM", "Kepler Yard", "", 12),
    contracts: [
      contract("haul", "Kepler Yard", "LM", 12, 7),
    ],
  }),
]);

function usage() {
  console.log(`Usage: node scripts/build-ship-radio-corpus.mjs [options]

Options:
  --out-dir DIR       Output directory (default: ${DEFAULT_OUT_DIR})
  --salts N           Hail request salts per context (default: 9)
  --max-speakers N    NPC speakers per choice prompt (default: 4)
`);
}

function parseArgs(argv) {
  const args = {
    outDir: DEFAULT_OUT_DIR,
    salts: 9,
    maxSpeakers: 4,
  };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === "--help" || arg === "-h") {
      usage();
      process.exit(0);
    }
    if (!arg.startsWith("--")) throw new Error(`unknown argument: ${arg}`);
    const key = arg.slice(2).replace(/-([a-z])/g, (_, c) => c.toUpperCase());
    const value = argv[++i];
    if (value == null) throw new Error(`missing value for ${arg}`);
    args[key] = value;
  }
  args.salts = positiveInt(args.salts, "salts");
  args.maxSpeakers = positiveInt(args.maxSpeakers, "max-speakers");
  return args;
}

function positiveInt(value, label) {
  const parsed = Number(value);
  if (!Number.isInteger(parsed) || parsed <= 0)
    throw new Error(`${label} must be a positive integer`);
  return parsed;
}

function stationShort(name) {
  return String(name || "open signal")
    .replace(/\bProspect Refinery\b/g, "Prospect Ref")
    .replace(/\bRefinery\b/g, "Ref");
}

function stateShort(state) {
  if (state === "travel_to_destination") return "outbound";
  if (state === "travel_to_asteroid") return "prospecting";
  if (state === "return_to_station") return "returning";
  if (state === "mining") return "mining";
  return String(state || "working").replace(/_/g, "-");
}

function memory(kind, commodity, stationA, stationB = "", quantity = 0, extra = {}) {
  return {
    kind_name: kind,
    action_name: kind === "delivery receipt" ? "delivery" : "haul",
    commodity_code: commodity || "",
    station_a_name: stationA || "",
    station_b_name: stationB || "",
    station_name: stationA || "",
    quantity_hint: quantity,
    confidence: 225,
    salience: 205,
    ...extra,
  };
}

function contract(action, station, commodity, quantity, price) {
  return {
    action_name: action,
    station_name: station,
    commodity_code: commodity,
    quantity,
    price,
    age: 3,
  };
}

function npc(opts) {
  return {
    slot: opts.slot,
    role: opts.role,
    state: opts.state || "travel_to_destination",
    home_station_name: opts.home,
    dest_station_name: opts.dest || "no destination",
    position: {
      x: 90 + opts.slot * 75,
      y: -300 - opts.slot * 37,
    },
    hull: opts.role === "hauler" ? 150 : 100,
    cargo: opts.cargo || [],
    cargo_total: (opts.cargo || []).reduce((sum, item) => sum + Number(item.amount || 0), 0),
    accepted_hail: true,
    market_memories: opts.memory ? [opts.memory] : (opts.memories || []),
    known_contracts: opts.contracts || [],
    kind: "npc",
  };
}

function makeContext(name, npcs, player = {}) {
  return {
    name,
    world: { tick: 2400 + name.length, time: 20.0, belt_seed: 2037 },
    player: {
      callsign: "YOU",
      position: { x: 0, y: 0 },
      ship: "player_ship",
      state: "hail_ping",
      station: "local signal",
      cargo: player.cargo || [],
      memories: [],
      kind: "player",
    },
    npcs,
  };
}

function buildScenarioContexts() {
  const contexts = [SAMPLE_CONTEXT];
  let scenario = 0;

  for (let i = 0; i < ORE.length; i++) {
    const station = STATIONS[i % 3];
    const next = STATIONS[(i + 1) % 3];
    contexts.push(makeContext(`ore-pressure-${ORE[i].toLowerCase()}`, [
      npc({ slot: 0, role: "miner", state: "travel_to_asteroid", home: station, dest: station, memory: memory("ore pressure", ORE[i], station, "", 70 + i * 9) }),
      npc({ slot: 1, role: "miner", state: "return_to_station", home: station, dest: station, memory: memory("ore pressure", ORE[(i + 1) % ORE.length], station, "", 86 + i * 7) }),
      npc({ slot: 2, role: "hauler", state: "travel_to_destination", home: station, dest: next, memory: memory("demand", GOODS[i], station, "", 40 + i * 11), contracts: [contract("haul", station, GOODS[i], 40 + i * 11, 5 + i)] }),
      npc({ slot: 3, role: "hauler", state: "travel_to_destination", home: next, dest: station, memory: memory("supply", GOODS[(i + 1) % GOODS.length], next, "", 22 + i * 8) }),
    ]));
  }

  const memoryKinds = [
    "demand",
    "supply",
    "route risk",
    "route danger",
    "route success",
    "route reputation",
    "delivery receipt",
    "station trust",
    "station risk",
  ];
  for (let i = 0; i < memoryKinds.length; i += 4) {
    const group = memoryKinds.slice(i, i + 4);
    contexts.push(makeContext(`hauler-memory-${++scenario}`, group.map((kind, j) => {
      const source = STATIONS[(i + j) % STATIONS.length];
      const dest = STATIONS[(i + j + 1) % STATIONS.length];
      const commodity = GOODS[(i + j) % GOODS.length];
      const routeKind = /route|delivery/.test(kind);
      const primary = routeKind ? dest : source;
      const secondary = routeKind ? source : "";
      return npc({
        slot: j,
        role: "hauler",
        state: j % 2 ? "return_to_station" : "travel_to_destination",
        home: source,
        dest,
        memory: memory(kind, commodity, primary, secondary, 18 + i * 3 + j * 7),
        contracts: [contract(kind === "delivery receipt" ? "delivery" : "haul", primary, commodity, 18 + j * 7, 6 + j)],
      });
    })));
  }

  for (let i = 0; i < MODULES.length; i += 4) {
    contexts.push(makeContext(`scaffold-pressure-${i / 4 + 1}`, MODULES.slice(i, i + 4).map((moduleName, j) => {
      const source = STATIONS[(i + j) % STATIONS.length];
      const dest = STATIONS[(i + j + 2) % STATIONS.length];
      return npc({
        slot: j,
        role: j % 2 ? "hauler" : "miner",
        state: "travel_to_destination",
        home: source,
        dest,
        memory: memory("scaffold pressure", "", dest, source, i + j + 1, { module_name: moduleName }),
      });
    })));
  }

  contexts.push(makeContext("mixed-frontier", [
    npc({ slot: 0, role: "miner", state: "mining", home: "Helios Works", dest: "Helios Works", memory: memory("ore pressure", "CR", "Helios Works", "", 118) }),
    npc({ slot: 1, role: "hauler", state: "travel_to_destination", home: "Freeport", dest: "Prospect Refinery", memory: memory("route risk", "RK", "Prospect Refinery", "Freeport", 33) }),
    npc({ slot: 2, role: "hauler", state: "travel_to_destination", home: "Prospect Refinery", dest: "Kepler Yard", memory: memory("delivery receipt", "FR", "Kepler Yard", "Prospect Refinery", 9) }),
    npc({ slot: 3, role: "miner", state: "return_to_station", home: "Kepler Yard", dest: "Kepler Yard", memory: memory("scaffold pressure", "", "Freeport", "Kepler Yard", 4, { module_name: "Signal Relay" }) }),
  ]));

  return contexts;
}

function distanceSq(a, b) {
  const dx = a.x - b.x;
  const dy = a.y - b.y;
  return dx * dx + dy * dy;
}

function buildPlan(context, maxNpcSpeakers) {
  const player = { ...context.player, kind: "player", distance_sq: 0 };
  const npcs = context.npcs
    .filter((candidate) => candidate.accepted_hail !== false)
    .map((candidate) => ({
      ...candidate,
      kind: "npc",
      distance_sq: distanceSq(context.player.position, candidate.position),
    }))
    .sort((a, b) => a.distance_sq - b.distance_sq)
    .slice(0, maxNpcSpeakers);
  return [player, ...npcs];
}

function speakerId(speaker) {
  if (speaker.kind === "player") return "YOU";
  return `${speaker.role.toUpperCase()} N${String(speaker.slot).padStart(2, "0")}`;
}

function choiceKey(speaker) {
  if (speaker.kind === "player") return "YOU";
  return `N${String(speaker.slot).padStart(2, "0")}`;
}

function memoryKind(mem) {
  return String(mem?.kind_name || "").toLowerCase();
}

function firstMemory(speaker) {
  const memories = speaker.market_memories || [];
  return memories.find((candidate) => candidate.kind_name && (candidate.commodity_code || candidate.module_name)) || memories[0];
}

function routeForMemory(mem, home, dest) {
  const kind = memoryKind(mem);
  if (/route|delivery/.test(kind)) {
    return {
      source: stationShort(mem.station_b_name || home),
      dest: stationShort(mem.station_a_name || dest || home),
    };
  }
  return {
    source: stationShort(mem?.station_a_name || mem?.station_name || home),
    dest: stationShort(dest || ""),
  };
}

function candidateLines(speaker) {
  if (speaker.kind === "player") {
    return [
      "Open hail; local traffic check.",
      "Local traffic, sound off.",
      "Open channel; nearby traffic check.",
    ];
  }

  const mem = firstMemory(speaker);
  const kind = memoryKind(mem);
  const firstContract = (speaker.known_contracts || [])[0];
  const commodity = mem?.commodity_code || firstContract?.commodity_code || "";
  const homeShort = stationShort(speaker.home_station_name);
  const destName = speaker.dest_station_name && speaker.dest_station_name !== "no destination"
    ? speaker.dest_station_name
    : "";
  const destShort = stationShort(destName);
  const route = routeForMemory(mem, speaker.home_station_name, destName);
  const quantity = firstContract?.quantity || mem?.quantity_hint || "";
  const quantityPrefix = quantity ? `${quantity} ` : "";

  if (kind === "scaffold pressure") {
    const moduleName = mem.module_name || `module ${mem.quantity_hint || "kit"}`;
    const source = mem.station_b_name ? stationShort(mem.station_b_name) : "";
    const target = stationShort(mem.station_a_name || speaker.home_station_name);
    return [
      `${moduleName} scaffold awake at ${target}.`,
      source ? `${moduleName} kit path ${source}>${target}.` : `${moduleName} kit path into ${target}.`,
      `${moduleName} build signal holds at ${target}.`,
    ];
  }

  if (speaker.role === "hauler") {
    if (kind === "supply" && commodity) return [
      `${commodity} stack warm at ${route.source}.`,
      `${route.source} ${commodity} stock is moving.`,
      `${commodity} supply glows at ${route.source}.`,
    ];
    if ((kind === "route danger" || kind === "route risk") && commodity) return [
      `${commodity} risk ${route.source}>${route.dest}.`,
      `${route.source}>${route.dest} reads rough.`,
      `${commodity} lane hazard near ${route.dest}.`,
    ];
    if ((kind === "route success" || kind === "route reputation") && commodity) return [
      `${commodity} lane ${route.source}>${route.dest} runs clean.`,
      `${route.source}>${route.dest} has clean pay.`,
      `${commodity} route ${route.source}>${route.dest} is trusted.`,
    ];
    if (kind === "delivery receipt" && commodity) return [
      `${quantityPrefix}${commodity} landed ${route.source}>${route.dest}.`,
      `${commodity} receipt ${route.source}>${route.dest}.`,
      `${commodity} delivery mark at ${route.dest}.`,
    ];
    if (kind === "station trust" && commodity) return [
      `${commodity} desk at ${route.source} pays clean.`,
      `${route.source} trusts ${commodity} work.`,
      `${route.source} ${commodity} mark is clean.`,
    ];
    if (kind === "station risk" && commodity) return [
      `${commodity} desk at ${route.source} reads sharp.`,
      `${route.source} ${commodity} work has teeth.`,
      `${commodity} risk mark at ${route.source}.`,
    ];
    if (destName && commodity) return [
      `${quantityPrefix}${commodity} tagged ${homeShort}>${destShort}.`,
      `${commodity} board at ${homeShort}; ${destShort} wants it.`,
      `${commodity} lane ${homeShort}>${destShort} lit.`,
    ];
    if (commodity) return [
      `${commodity} demand mark holding at ${homeShort}.`,
      `${quantityPrefix}${commodity} haul still open at ${homeShort}.`,
      `${homeShort} ${commodity} board is awake.`,
    ];
  }

  if (speaker.role === "miner") {
    if (kind === "ore pressure" && commodity) return [
      `${commodity} pressure bright at ${homeShort}.`,
      `${homeShort} ${commodity} seam is talking.`,
      `${commodity} pressure mark holding near ${homeShort}.`,
    ];
    if (commodity) return [
      `${commodity} mark awake near ${homeShort}.`,
      `${homeShort} wants ${commodity}; rock is speaking.`,
      `${commodity} trace holding near ${homeShort}.`,
    ];
  }

  return [
    `${homeShort} local signal holding.`,
    `${speakerId(speaker)} holding local signal.`,
    `${homeShort} work mark holding.`,
  ];
}

function rotatedCandidates(speaker, hailSalt = 0) {
  const candidates = candidateLines(speaker).slice(0, 3);
  if (candidates.length <= 1) return candidates;
  const slot = speaker.kind === "npc" ? Math.abs(Number(speaker.slot) || 0) : 0;
  const offset = (slot + Math.abs(Number(hailSalt) || 0)) % candidates.length;
  return candidates.slice(offset).concat(candidates.slice(0, offset));
}

function buildChoicePrompt(plan, hailSalt) {
  const lines = ["local hail choices"];
  for (const speaker of plan) {
    lines.push(`${speakerId(speaker)}:`);
    rotatedCandidates(speaker, hailSalt).forEach((line, index) => {
      lines.push(`${index + 1} ${line}`);
    });
  }
  lines.push("Return all speaker keys. No words.");
  lines.push(`Example: ${choiceAnswer(plan, hailSalt)}`);
  lines.push("ANSWER:");
  return lines.join("\n");
}

function choiceAnswer(plan, hailSalt) {
  const salt = Math.abs(Number(hailSalt) || 0);
  let npcOrdinal = 0;
  return plan.map((speaker) => {
    if (speaker.kind === "player") return `YOU=${(salt % 3) + 1}`;
    const target = (npcOrdinal + salt + 2) % 3;
    const offset = (Math.abs(Number(speaker.slot) || 0) + salt) % 3;
    const choice = ((target + 3 - offset) % 3) + 1;
    npcOrdinal++;
    return `${choiceKey(speaker)}=${choice}`;
  }).join(",");
}

function memoryLabel(speaker) {
  const mem = firstMemory(speaker);
  if (!mem) return "local signal";
  if (memoryKind(mem) === "scaffold pressure") {
    const from = mem.station_b_name ? ` from ${stationShort(mem.station_b_name)}` : "";
    return `${mem.kind_name} ${mem.module_name || "module kit"} at ${stationShort(mem.station_a_name)}${from}`;
  }
  const commodity = mem.commodity_code || "UNK";
  const from = mem.station_a_name || mem.station_name || speaker.home_station_name;
  const to = mem.station_b_name ? ` from ${stationShort(mem.station_b_name)}` : "";
  return `${mem.kind_name} ${commodity} at ${stationShort(from)}${to}`;
}

function buildLinePrompt(speaker, variation) {
  if (speaker.kind === "player") {
    return [
      "Signal ship radio line.",
      "speaker=YOU",
      "role=player",
      "state=hailing",
      "home=local signal",
      `variation=${variation + 1}`,
      "LINE:",
    ].join("\n");
  }
  return [
    "Signal ship radio line.",
    `speaker=${speakerId(speaker)}`,
    `role=${speaker.role}`,
    `state=${stateShort(speaker.state)}`,
    `home=${speaker.home_station_name}`,
    `dest=${speaker.dest_station_name || "no destination"}`,
    `memory=${memoryLabel(speaker)}`,
    `cargo=${cargoText(speaker.cargo)}`,
    `variation=${variation + 1}`,
    "LINE:",
  ].join("\n");
}

function cargoText(cargo) {
  return cargo && cargo.length
    ? cargo.map((item) => `${item.amount}${item.commodity_code}`).join(",")
    : "empty";
}

function buildRecords(contexts, salts, maxSpeakers) {
  const choiceRecords = [];
  const lineRecords = [];
  const rawLines = new Set();

  for (const context of contexts) {
    const linePlan = buildPlan(context, maxSpeakers);
    for (const speaker of linePlan) {
      candidateLines(speaker).forEach((line, index) => {
        rawLines.add(line);
        const linePrompt = buildLinePrompt(speaker, index);
        const lineId = stableId("line", context.name, choiceKey(speaker), index, line);
        lineRecords.push(chatRecord(lineId, "ship_radio_line", SYSTEM_LINE, linePrompt, line, {
          context: context.name,
          speaker: choiceKey(speaker),
          variation: index + 1,
        }));
      });
    }

    for (let salt = 0; salt < salts; salt++) {
      const plan = buildPlan(context, maxSpeakers);
      const prompt = buildChoicePrompt(plan, salt);
      const answer = choiceAnswer(plan, salt);
      const id = stableId("choice", context.name, salt, prompt);
      choiceRecords.push(chatRecord(id, "choice_batch", SYSTEM_CHOICE, prompt, answer, {
        context: context.name,
        hail_salt: salt,
        speaker_count: plan.length,
      }));
    }
  }

  return {
    sft: [...choiceRecords, ...lineRecords],
    choices: choiceRecords,
    lines: lineRecords,
    rawVoice: [...rawLines].sort(),
  };
}

function chatRecord(id, task, system, user, assistant, metadata) {
  return {
    id,
    task,
    messages: [
      { role: "system", content: system },
      { role: "user", content: user },
      { role: "assistant", content: assistant },
    ],
    metadata,
  };
}

function completionRecord(record) {
  const system = record.messages[0].content;
  const user = record.messages[1].content;
  const assistant = record.messages[2].content;
  return {
    id: record.id,
    task: record.task,
    prompt: `SYSTEM:\n${system}\n\nUSER:\n${user}\n\nASSISTANT:\n`,
    completion: assistant,
    metadata: record.metadata,
  };
}

function stableId(...parts) {
  return createHash("sha1").update(parts.join("\0")).digest("hex").slice(0, 16);
}

function sha256(text) {
  return createHash("sha256").update(text).digest("hex");
}

function jsonl(records) {
  return `${records.map((record) => JSON.stringify(record)).join("\n")}\n`;
}

function buildVoiceText(lines) {
  return [
    "# Signal Ship Radio Voice Seed",
    "",
    "Keep ship speech clipped, grounded, and economic.",
    "A ship knows station names, routes, commodities, memory kinds, and its own current work.",
    "A ship does not know omniscient market truth, does not greet, and does not explain itself as an assistant.",
    "",
    "Canonical radio lines:",
    "",
    ...lines.map((line) => `- ${line}`),
    "",
  ].join("\n");
}

function buildReadme(manifest) {
  return `# Signal Ship Radio Corpus

Seed corpus for training a tiny local model to speak for Signal ships and to
obey the bounded hail-choice protocol used by the in-process game client.

## Files

- \`ship-radio-sft.jsonl\`: chat-style SFT records with \`messages\`.
- \`ship-radio-completions.jsonl\`: prompt/completion view of the same records.
- \`ship-radio-voice.txt\`: raw voice lines for style mixing or continued pretraining.
- \`manifest.json\`: counts and SHA-256 hashes for reproducibility.

## Tasks

- \`choice_batch\`: given a prompt shaped like the live C hail prompt, return
  only assignments such as \`YOU=1,N00=3,N01=2\`.
- \`ship_radio_line\`: given a single ship's grounded facts, emit one short
  in-world radio line.

The choice task is the safest runtime target. C still owns station, commodity,
contract, and route grounding; the model only chooses among legal candidates.

## Regenerate

\`\`\`sh
node scripts/build-ship-radio-corpus.mjs
\`\`\`

Current manifest summary:

\`\`\`json
${JSON.stringify(manifest.summary, null, 2)}
\`\`\`
`;
}

function buildManifest(args, records, files) {
  const summary = {
    format_version: 1,
    choice_records: records.choices.length,
    line_records: records.lines.length,
    sft_records: records.sft.length,
    raw_voice_lines: records.rawVoice.length,
    salts: args.salts,
    max_speakers: args.maxSpeakers,
  };
  return {
    summary,
    files: Object.fromEntries(Object.entries(files).map(([name, text]) => [
      name,
      { bytes: Buffer.byteLength(text), sha256: sha256(text) },
    ])),
    notes: [
      "Generated from scripts/build-ship-radio-corpus.mjs.",
      "The corpus is intentionally small and protocol-shaped for tiny models.",
      "Regeneration is deterministic for the same script and arguments.",
    ],
  };
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const outDir = path.resolve(args.outDir);
  const contexts = buildScenarioContexts();
  const records = buildRecords(contexts, args.salts, args.maxSpeakers);

  const sftText = jsonl(records.sft);
  const completionsText = jsonl(records.sft.map(completionRecord));
  const voiceText = buildVoiceText(records.rawVoice);
  const preManifestFiles = {
    "ship-radio-sft.jsonl": sftText,
    "ship-radio-completions.jsonl": completionsText,
    "ship-radio-voice.txt": voiceText,
  };
  const manifest = buildManifest(args, records, preManifestFiles);
  const manifestText = `${JSON.stringify(manifest, null, 2)}\n`;
  const readmeText = buildReadme(manifest);
  const files = {
    ...preManifestFiles,
    "manifest.json": manifestText,
    "README.md": readmeText,
  };

  await mkdir(outDir, { recursive: true });
  await Promise.all(Object.entries(files).map(([name, text]) =>
    writeFile(path.join(outDir, name), text, "utf8")));

  console.log(`wrote ${outDir}`);
  console.log(`sft=${records.sft.length} choice=${records.choices.length} lines=${records.lines.length} raw=${records.rawVoice.length}`);
}

main().catch((err) => {
  console.error(err.message);
  process.exit(1);
});
