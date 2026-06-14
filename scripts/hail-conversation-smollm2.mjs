#!/usr/bin/env node

const DEFAULT_OLLAMA = "http://127.0.0.1:11434";
const DEFAULT_MODEL = "smollm2";

export const SAMPLE_HAIL_CONTEXT = {
  world: { tick: 2359, time: 19.658, belt_seed: 2037, world_seq: 77 },
  player: {
    callsign: "YOU",
    position: { x: 0, y: 0 },
    ship: "player_ship",
    state: "hail_ping",
    station: "local signal",
    cargo: [],
    memories: [],
  },
  npcs: [
    {
      slot: 0,
      role: "miner",
      state: "return_to_station",
      home_station: 0,
      home_station_name: "Prospect Refinery",
      dest_station: 0,
      dest_station_name: "Prospect Refinery",
      position: { x: 35.1, y: -2405 },
      hull: 100,
      cargo_total: 0,
      cargo: [],
      accepted_hail: true,
      market_memories: [
        { kind_name: "ore pressure", action_name: "work", commodity_code: "FE", station_a_name: "Prospect Refinery", station_b_name: "", quantity_hint: 84, confidence: 210, salience: 212, hops: 1 },
        { kind_name: "demand", action_name: "haul", commodity_code: "FE", station_a_name: "Prospect Refinery", station_b_name: "", quantity_hint: 1, confidence: 235, salience: 180, hops: 1 },
        { kind_name: "demand", action_name: "haul", commodity_code: "RK", station_a_name: "Prospect Refinery", station_b_name: "", quantity_hint: 1000, confidence: 235, salience: 180, hops: 1 },
        { kind_name: "demand", action_name: "haul", commodity_code: "LM", station_a_name: "Prospect Refinery", station_b_name: "", quantity_hint: 8, confidence: 235, salience: 180, hops: 1 },
      ],
      known_contracts: [
        { action_name: "haul", station_name: "Prospect Refinery", commodity_code: "RK", quantity: 1000, price: 0, age: 19 },
        { action_name: "haul", station_name: "Prospect Refinery", commodity_code: "LM", quantity: 8, price: 38, age: 4 },
        { action_name: "delivery", station_name: "Kepler Yard", commodity_code: "FR", quantity: 4, price: 4, age: 1 },
      ],
    },
    {
      slot: 1,
      role: "miner",
      state: "travel_to_asteroid",
      home_station: 0,
      home_station_name: "Prospect Refinery",
      dest_station: 0,
      dest_station_name: "Prospect Refinery",
      position: { x: 142.8, y: -2546.4 },
      hull: 100,
      cargo_total: 0,
      cargo: [],
      accepted_hail: true,
      market_memories: [
        { kind_name: "ore pressure", action_name: "work", commodity_code: "FE", station_a_name: "Prospect Refinery", station_b_name: "", quantity_hint: 92, confidence: 210, salience: 220, hops: 1 },
        { kind_name: "demand", action_name: "haul", commodity_code: "FE", station_a_name: "Prospect Refinery", station_b_name: "", quantity_hint: 1, confidence: 235, salience: 180, hops: 1 },
        { kind_name: "demand", action_name: "haul", commodity_code: "RK", station_a_name: "Prospect Refinery", station_b_name: "", quantity_hint: 1000, confidence: 235, salience: 180, hops: 1 },
        { kind_name: "demand", action_name: "haul", commodity_code: "LM", station_a_name: "Prospect Refinery", station_b_name: "", quantity_hint: 8, confidence: 235, salience: 180, hops: 1 },
      ],
      known_contracts: [
        { action_name: "haul", station_name: "Prospect Refinery", commodity_code: "RK", quantity: 1000, price: 0, age: 19 },
        { action_name: "haul", station_name: "Prospect Refinery", commodity_code: "LM", quantity: 8, price: 38, age: 3 },
        { action_name: "delivery", station_name: "Kepler Yard", commodity_code: "FR", quantity: 4, price: 4, age: 0 },
      ],
    },
    {
      slot: 2,
      role: "hauler",
      state: "travel_to_destination",
      home_station: 1,
      home_station_name: "Kepler Yard",
      dest_station: 2,
      dest_station_name: "Helios Works",
      position: { x: 731, y: 3124.3 },
      hull: 150,
      cargo_total: 0,
      cargo: [],
      accepted_hail: true,
      market_memories: [
        { kind_name: "demand", action_name: "haul", commodity_code: "FR", station_a_name: "Kepler Yard", station_b_name: "", quantity_hint: 108, confidence: 235, salience: 180, hops: 17 },
        { kind_name: "demand", action_name: "haul", commodity_code: "LM", station_a_name: "Kepler Yard", station_b_name: "", quantity_hint: 12, confidence: 235, salience: 180, hops: 17 },
      ],
      known_contracts: [
        { action_name: "haul", station_name: "Kepler Yard", commodity_code: "FR", quantity: 108, price: 4, age: 3 },
        { action_name: "haul", station_name: "Kepler Yard", commodity_code: "LM", quantity: 12, price: 7, age: 3 },
      ],
    },
    {
      slot: 3,
      role: "hauler",
      state: "travel_to_destination",
      home_station: 1,
      home_station_name: "Kepler Yard",
      dest_station: 2,
      dest_station_name: "Helios Works",
      position: { x: 675.7, y: 3158 },
      hull: 150,
      cargo_total: 0,
      cargo: [],
      accepted_hail: true,
      market_memories: [
        { kind_name: "demand", action_name: "haul", commodity_code: "FR", station_a_name: "Kepler Yard", station_b_name: "", quantity_hint: 108, confidence: 235, salience: 180, hops: 17 },
        { kind_name: "demand", action_name: "haul", commodity_code: "LM", station_a_name: "Kepler Yard", station_b_name: "", quantity_hint: 12, confidence: 235, salience: 180, hops: 17 },
      ],
      known_contracts: [
        { action_name: "haul", station_name: "Kepler Yard", commodity_code: "FR", quantity: 108, price: 4, age: 3 },
        { action_name: "haul", station_name: "Kepler Yard", commodity_code: "LM", quantity: 12, price: 7, age: 3 },
      ],
    },
  ],
};

function usage() {
  console.log(`Usage: node scripts/hail-conversation-smollm2.mjs [options]

Options:
  --sample           Use built-in sample hail data
  --server URL       Read real NPC context from a Signal server
  --input FILE       Read hail context JSON from file
  --ollama URL       Ollama API base (default: ${DEFAULT_OLLAMA})
  --model NAME       Ollama model (default: ${DEFAULT_MODEL})
  --temperature N    Ollama temperature (default: 0.72)
  --max-speakers N   Max accepted NPC replies after player (default: 4)
  --first-delay N    Seconds before first NPC reply (default: 1.2)
  --line-gap N       Seconds between radio lines (default: 2.4)
  --variant NAME     Prompt variant: ledger, terse, transcript, choice, bulletin, or exemplars (default: choice)
  --mode NAME        Ollama mode: generate or chat (default: chat)
  --player-x N       Player X for proximity sort with --server (default: 0)
  --player-y N       Player Y for proximity sort with --server (default: 0)
  --dry-run          Print prompts without calling Ollama
  --json             Emit JSON records
  --runtime-json     Emit compact client-ready hail payload
  --no-batch-choice  Disable one-call choice generation
  --batch-choice-retries N
                     Retry malformed batch replies (default: 1)
`);
}

function parseArgs(argv) {
  const args = {
    ollama: DEFAULT_OLLAMA,
    model: DEFAULT_MODEL,
    temperature: 0.72,
    maxSpeakers: 4,
    firstDelay: 1.2,
    lineGap: 2.4,
    variant: "choice",
    mode: "chat",
    playerX: 0,
    playerY: 0,
    sample: false,
    dryRun: false,
    json: false,
    runtimeJson: false,
    batchChoice: true,
    batchChoiceRetries: 1,
  };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === "--help" || arg === "-h") {
      usage();
      process.exit(0);
    } else if (arg === "--sample") {
      args.sample = true;
    } else if (arg === "--dry-run") {
      args.dryRun = true;
    } else if (arg === "--json") {
      args.json = true;
    } else if (arg === "--runtime-json") {
      args.runtimeJson = true;
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
  args.temperature = Number(args.temperature);
  args.maxSpeakers = Number(args.maxSpeakers);
  args.firstDelay = Number(args.firstDelay);
  args.lineGap = Number(args.lineGap);
  args.batchChoiceRetries = Number(args.batchChoiceRetries);
  return args;
}

async function readContext(args) {
  if (args.server) return readServerContext(args);
  if (args.sample || !args.input) return SAMPLE_HAIL_CONTEXT;
  const fs = await import("node:fs/promises");
  const parsed = JSON.parse(await fs.readFile(args.input, "utf8"));
  if (parsed.npcs && !parsed.player) {
    return realHailContextFromNpcSnapshot(parsed, {
      x: Number(args.playerX),
      y: Number(args.playerY),
    });
  }
  return parsed;
}

async function readServerContext(args) {
  const url = new URL("/api/npc_chatter_context", args.server);
  url.searchParams.set("limit", String(Math.max(1, args.maxSpeakers)));
  const snapshot = await fetchJson(url);
  return realHailContextFromNpcSnapshot(snapshot, {
    x: Number(args.playerX),
    y: Number(args.playerY),
  });
}

function distSq(a, b) {
  const dx = a.x - b.x;
  const dy = a.y - b.y;
  return dx * dx + dy * dy;
}

function cargoText(cargo) {
  return cargo && cargo.length
    ? cargo.map((c) => `${c.amount} ${c.commodity_code}`).join(", ")
    : "empty hold";
}

function compactCargoText(cargo) {
  return cargo && cargo.length
    ? cargo.map((c) => `${c.amount}${c.commodity_code}`).join(",")
    : "empty";
}

function compactStateText(state) {
  if (state === "travel_to_destination") return "outbound";
  if (state === "travel_to_asteroid") return "prospecting";
  if (state === "return_to_station") return "returning";
  return (state || "working").replace(/_/g, "-");
}

function speakerId(speaker) {
  if (speaker.kind === "player") return speaker.callsign || "YOU";
  return `${speaker.role.toUpperCase()} N${String(speaker.slot).padStart(2, "0")}`;
}

function memoryText(memory) {
  if (typeof memory === "string") return memory;
  const from = memory.station_a_name || memory.station_name || "somewhere";
  const to = memory.station_b_name ? ` to ${memory.station_b_name}` : "";
  const action = memory.action_name && memory.action_name !== "work"
    ? `${memory.action_name} `
    : "";
  return `${memory.kind_name}: ${action}${memory.commodity_code || "UNK"} around ${from}${to}`;
}

function validStationName(name, fallback) {
  return name || fallback || "open channel";
}

function compactStationName(station) {
  return station
    .replace(/\bRefinery\b/g, "Ref")
    .replace(/\bWorks\b/g, "Works")
    .replace(/\bYard\b/g, "Yard");
}

function memoryKind(memory) {
  return String(memory?.kind_name || "").toLowerCase();
}

function memoryRoute(memory, fallbackHome, fallbackDest) {
  const kind = memoryKind(memory);
  const routeKinds = new Set([
    "route danger",
    "route success",
    "delivery receipt",
    "route reputation",
    "route risk",
  ]);
  if (routeKinds.has(kind)) {
    return {
      source: compactStationName(memory.station_b_name || fallbackHome),
      dest: compactStationName(memory.station_a_name || fallbackDest || fallbackHome),
    };
  }
  return {
    source: compactStationName(memory?.station_a_name || memory?.station_name || fallbackHome),
    dest: compactStationName(fallbackDest || ""),
  };
}

export function realHailContextFromNpcSnapshot(snapshot, playerPosition = { x: 0, y: 0 }) {
  const npcs = (snapshot.npcs || []).map((npc) => ({
    ...npc,
    accepted_hail: true,
    home_station_name: validStationName(npc.home_station_name, "unknown home"),
    dest_station_name: validStationName(npc.dest_station_name, "no destination"),
    position: npc.position || { x: 0, y: 0 },
    cargo: npc.cargo || [],
    market_memories: npc.market_memories || [],
    known_contracts: npc.known_contracts || [],
  }));
  return {
    world: snapshot.world,
    player: {
      callsign: "YOU",
      position: playerPosition,
      ship: "player_ship",
      state: "hail_ping",
      station: "local signal",
      cargo: [],
      memories: [],
    },
    npcs,
  };
}

function contractText(contract) {
  return `${contract.action_name} ${contract.quantity} ${contract.commodity_code} for ${contract.station_name || "unknown station"}`;
}

function compactMemoryText(memory) {
  if (typeof memory === "string") return memory;
  const commodity = memory.commodity_code || "UNK";
  const station = compactStationName(memory.station_a_name || memory.station_name || "open");
  const route = memory.station_b_name ? `>${compactStationName(memory.station_b_name)}` : "";
  const kind = memory.kind_name || "memory";
  if (kind === "ore pressure") return `${commodity} ore pressure ${station}`;
  if (kind === "demand") return `${commodity} demand ${station}${route}`;
  if (kind === "supply") return `${commodity} supply ${station}${route}`;
  return `${commodity} ${kind} ${station}${route}`;
}

function compactContractText(contract) {
  const quantity = contract.quantity ? `${contract.quantity} ` : "";
  return `${quantity}${contract.commodity_code} ${contract.action_name} ${compactStationName(contract.station_name || "open")}`;
}

function hologramMemoryLabel(memory) {
  if (typeof memory === "string") return memory;
  const kind = memory.kind_name || "memory";
  const commodity = memory.commodity_code || "UNK";
  const station = memory.station_a_name || memory.station_name || "open signal";
  const route = memory.station_b_name ? ` -> ${memory.station_b_name}` : "";
  const action = memory.action_name && memory.action_name !== "work"
    ? `${memory.action_name} `
    : "";
  return `${kind}: ${action}${commodity} @ ${station}${route}`;
}

function hologramContractLabel(contract) {
  const quantity = contract.quantity ? `${contract.quantity} ` : "";
  return `${contract.action_name || "work"} ${quantity}${contract.commodity_code || "UNK"} @ ${contract.station_name || "open signal"}`;
}

export function knowledgeHologramsForSpeaker(speaker) {
  if (speaker.kind !== "npc") return [];
  const holograms = [];
  for (const memory of (speaker.market_memories || []).slice(0, 4)) {
    holograms.push({
      type: "market_memory",
      label: hologramMemoryLabel(memory),
      payload: memory,
    });
  }
  for (const contract of (speaker.known_contracts || []).slice(0, 3)) {
    holograms.push({
      type: "contract",
      label: hologramContractLabel(contract),
      payload: contract,
    });
  }
  return holograms;
}

export function scheduledAtForSpeakerIndex(index, args = {}) {
  if (index <= 0) return 0;
  const firstDelay = Number.isFinite(Number(args.firstDelay)) ? Number(args.firstDelay) : 1.2;
  const lineGap = Number.isFinite(Number(args.lineGap)) ? Number(args.lineGap) : 2.4;
  return Number((firstDelay + (index - 1) * lineGap).toFixed(3));
}

function speakerFacts(speaker) {
  const id = speakerId(speaker);
  if (speaker.kind === "player") {
    return {
      id,
      job: "traffic check",
      home: speaker.station || "local signal",
      dest: "",
      state: "hailing",
      cargo: cargoText(speaker.cargo),
      memories: speaker.memories || [],
      contracts: [],
    };
  }
  return {
    id,
    job: speaker.role || "worker",
    home: speaker.home_station_name || "unknown home",
    dest: speaker.dest_station_name || "no destination",
    state: speaker.state || "working",
    cargo: cargoText(speaker.cargo),
    memories: (speaker.market_memories || []).slice(0, 4).map(memoryText),
    contracts: (speaker.known_contracts || []).slice(0, 3).map(contractText),
  };
}

function buildLedgerPrompt(speaker, previousLines = []) {
  const facts = speakerFacts(speaker);
  const lines = ["SIGNAL BELT COMMS"];
  if (speaker.kind === "player") {
    lines.push(`${facts.id} // opening hail // traffic check // cargo ${facts.cargo}`);
  } else {
    lines.push(`${facts.id} // ${facts.state} // home ${facts.home} // dest ${facts.dest} // cargo ${facts.cargo}`);
  }
  for (const line of previousLines.filter((line) => line.text).slice(-2)) {
    lines.push(`heard ${line.speaker}: "${line.text}"`);
  }
  for (const memory of facts.memories) lines.push(`memory ${memory}`);
  for (const contract of facts.contracts) lines.push(`contract ${contract}`);
  lines.push(`${facts.id} radio:`);
  return lines.join("\n");
}

function buildTersePrompt(speaker, previousLines = []) {
  const facts = speakerFacts(speaker);
  if (speaker.kind === "player") {
    return [
      "YOU local pilot",
      "open hail near local signal",
      "you:",
    ].join("\n");
  }
  const heard = previousLines.filter((line) => line.text).slice(-1)[0];
  const memory = (speaker.market_memories || [])[0]
    ? compactMemoryText(speaker.market_memories[0])
    : facts.memories[0] || "signal clear";
  const contract = (speaker.known_contracts || [])[0]
    ? compactContractText(speaker.known_contracts[0])
    : facts.contracts[0] || "";
  const route = facts.dest && facts.dest !== "no destination"
    ? `${compactStationName(facts.home)}>${compactStationName(facts.dest)}`
    : compactStationName(facts.home);
  const lines = [
    `${facts.id} ${compactStateText(facts.state)} ${route}`,
    `hold ${compactCargoText(speaker.cargo)}; knows ${memory}`,
  ];
  if (contract) lines.push(`job ${contract}`);
  if (heard) lines.push(`heard ${heard.speaker}: ${clipText(heard.text, 36)}`);
  lines.push(`${facts.id.toLowerCase()}:`);
  return lines.join("\n");
}

function buildBulletinPrompt(speaker, previousLines = []) {
  const facts = speakerFacts(speaker);
  const lines = [
    `[Signal belt radio]`,
    `speaker=${facts.id}`,
    `work=${facts.state}`,
    `route=${facts.home}${facts.dest ? ` to ${facts.dest}` : ""}`,
    `cargo=${facts.cargo}`,
  ];
  if (facts.memories[0]) lines.push(`knows=${facts.memories[0]}`);
  if (facts.contracts[0]) lines.push(`contract=${facts.contracts[0]}`);
  const heard = previousLines.filter((line) => line.text).slice(-1)[0];
  if (heard) lines.push(`last=${heard.speaker}: ${heard.text}`);
  lines.push("line=");
  return lines.join("\n");
}

function buildTranscriptPrompt(speaker, previousLines = []) {
  const facts = speakerFacts(speaker);
  if (speaker.kind === "player") {
    return [
      "local signal",
      "YOU:",
    ].join("\n");
  }
  const memory = (speaker.market_memories || [])[0]
    ? compactMemoryText(speaker.market_memories[0])
    : facts.memories[0] || "signal clear";
  const contract = (speaker.known_contracts || [])[0]
    ? compactContractText(speaker.known_contracts[0])
    : facts.contracts[0] || "";
  const route = facts.dest && facts.dest !== "no destination"
    ? `${compactStationName(facts.home)}>${compactStationName(facts.dest)}`
    : compactStationName(facts.home);
  const lines = ["local signal"];
  for (const line of previousLines.filter((line) => line.text).slice(-2)) {
    lines.push(`${line.speaker}: ${clipText(line.text, 42)}`);
  }
  lines.push(`${facts.id} ${compactStateText(facts.state)} ${route}`);
  lines.push(memory);
  if (contract) lines.push(contract);
  lines.push(`${facts.id}:`);
  return lines.join("\n");
}

export function candidateRadioLinesForSpeaker(speaker) {
  const facts = speakerFacts(speaker);
  if (speaker.kind === "player") {
    return [
      "Open hail; local traffic check.",
      "Local traffic, sound off.",
      "Open channel; nearby traffic check.",
    ];
  }

  const memory = firstUsefulMemory(speaker);
  const contract = (speaker.known_contracts || [])[0];
  const commodity = memory?.commodity_code || contract?.commodity_code || "";
  const homeShort = compactStationName(facts.home);
  const dest = facts.dest && facts.dest !== "no destination" ? facts.dest : "";
  const destShort = compactStationName(dest);
  const route = memoryRoute(memory, facts.home, dest);
  const quantity = contract?.quantity || memory?.quantity_hint || "";
  const quantityPrefix = quantity ? `${quantity} ` : "";
  const kind = memoryKind(memory);

  if (kind === "scaffold pressure") {
    const module = memory.module_name || `module ${memory.quantity_hint || "kit"}`;
    const source = memory.station_b_name ? compactStationName(memory.station_b_name) : "";
    const target = compactStationName(memory.station_a_name || facts.home);
    return [
      `${module} scaffold awake at ${target}.`,
      source ? `${module} kit path ${source}>${target}.` : `${module} kit path into ${target}.`,
      `${module} build signal holds at ${target}.`,
    ];
  }

  if ((speaker.role || "") === "hauler") {
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
    if (dest && commodity) return [
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

  if ((speaker.role || "") === "miner") {
    if (memory?.kind_name === "ore pressure" && commodity) return [
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
    fallbackRadioLine(speaker),
    `${facts.id} holding local signal.`,
    `${homeShort} local signal holding.`,
  ];
}

export function rotatedChoiceCandidatesForSpeaker(speaker, hailSalt = 0) {
  let candidates = candidateRadioLinesForSpeaker(speaker).slice(0, 3);
  if (candidates.length > 1) {
    const slot = speaker.kind === "npc" ? Math.abs(Number(speaker.slot) || 0) : 0;
    const offset = (slot + Math.abs(Number(hailSalt) || 0)) % candidates.length;
    candidates = candidates.slice(offset).concat(candidates.slice(0, offset));
  }
  return candidates;
}

function buildChoicePrompt(speaker, previousLines = []) {
  const facts = speakerFacts(speaker);
  const lines = ["local signal"];
  const heard = previousLines.filter((line) => line.text).slice(-1)[0];
  if (heard) lines.push(`${heard.speaker}: ${clipText(heard.text, 42)}`);
  lines.push(speaker.kind === "player"
    ? "YOU"
    : `${facts.id} ${compactStateText(facts.state)}`);
  rotatedChoiceCandidatesForSpeaker(speaker)
    .forEach((line, index) => lines.push(`${index + 1} ${line}`));
  lines.push(`${facts.id}:`);
  return lines.join("\n");
}

export function buildChoiceBatchPrompt(plan, hailSalt = 0) {
  const lines = ["local hail choices"];
  for (const speaker of plan) {
    const id = speakerId(speaker);
    lines.push(`${id}:`);
    rotatedChoiceCandidatesForSpeaker(speaker, hailSalt)
      .forEach((line, index) => lines.push(`${index + 1} ${line}`));
  }
  const salt = Math.abs(Number(hailSalt) || 0);
  let npcOrdinal = 0;
  const keys = plan.map((speaker) => {
    if (speaker.kind === "player") return `YOU=${(salt % 3) + 1}`;
    const target = (npcOrdinal + salt + 2) % 3;
    const offset = (Math.abs(Number(speaker.slot) || 0) + salt) % 3;
    const choice = ((target + 3 - offset) % 3) + 1;
    npcOrdinal++;
    return `N${String(speaker.slot).padStart(2, "0")}=${choice}`;
  });
  lines.push(`Return all speaker keys. No words.`);
  lines.push(`Example: ${keys.join(",")}`);
  lines.push("ANSWER:");
  return lines.join("\n");
}

export function choiceKeyForSpeaker(speaker) {
  if (speaker.kind === "player") return "YOU";
  return `N${String(speaker.slot).padStart(2, "0")}`;
}

function normalizeChoiceKey(key) {
  const text = String(key || "").toUpperCase().replace(/\s+/g, " ").trim();
  if (text === "YOU" || text === "PLAYER") return "YOU";
  const n = text.match(/\bN\s*(\d{1,2})\b/);
  if (n) return `N${n[1].padStart(2, "0")}`;
  const role = text.match(/\b(?:MINER|HAULER)\s+N?(\d{1,2})\b/);
  if (role) return `N${role[1].padStart(2, "0")}`;
  return text.replace(/\s+/g, "");
}

export function parseChoiceBatchResponse(text, plan) {
  const result = new Map();
  const raw = String(text || "");
  const pairRe = /([A-Za-z]+(?:\s+N?\d{1,2})?|N\s*\d{1,2})\s*=\s*([1-3])/gi;
  for (const match of raw.matchAll(pairRe)) {
    result.set(normalizeChoiceKey(match[1]), Number(match[2]));
  }

  if (result.size === 0) {
    const numbers = raw.match(/\b[1-3]\b/g) || [];
    if (numbers.length >= plan.length) {
      for (let i = 0; i < plan.length; i++) {
        result.set(choiceKeyForSpeaker(plan[i]), Number(numbers[i]));
      }
    }
  }
  return result;
}

function buildExemplarPrompt(speaker, previousLines = []) {
  const facts = speakerFacts(speaker);
  const memory = facts.memories[0] || facts.contracts[0] || "local signal clear";
  const route = facts.dest && facts.dest !== "no destination"
    ? `${facts.home} > ${facts.dest}`
    : facts.home;
  const heard = previousLines.filter((line) => line.text).slice(-1)[0];
  const lines = [
    "Prospect miner / FE ore pressure: Ferrite dust inbound for Prospect.",
    "Kepler hauler / LM demand: Kepler load moving Helios-side.",
    "Helios miner / crystal route: Crystal mark acquired, returning bright.",
  ];
  if (speaker.kind === "player") {
    lines.push(`local pilot / traffic check:`);
  } else {
    lines.push(`${facts.id.toLowerCase()} / ${facts.state} / ${route} / ${memory}:`);
  }
  if (heard) lines.push(`heard ${heard.speaker}: ${heard.text}`);
  return lines.join("\n");
}

export function buildConversationPlan(context, maxNpcSpeakers = 4) {
  const playerPos = context.player.position;
  const npcs = (context.npcs || [])
    .filter((npc) => npc.accepted_hail !== false)
    .map((npc) => ({ ...npc, kind: "npc", distance_sq: distSq(playerPos, npc.position) }))
    .sort((a, b) => a.distance_sq - b.distance_sq)
    .slice(0, maxNpcSpeakers);
  return [{ ...context.player, kind: "player", distance_sq: 0 }, ...npcs];
}

export function buildSpeakerPrompt(context, speaker, previousLines = []) {
  const variant = context.prompt_variant || "terse";
  if (variant === "choice") return buildChoicePrompt(speaker, previousLines);
  if (variant === "terse") return buildTersePrompt(speaker, previousLines);
  if (variant === "transcript") return buildTranscriptPrompt(speaker, previousLines);
  if (variant === "bulletin") return buildBulletinPrompt(speaker, previousLines);
  if (variant === "exemplars") return buildExemplarPrompt(speaker, previousLines);
  return buildLedgerPrompt(speaker, previousLines);
}

function cleanLine(text) {
  let line = (text || "")
    .replace(/^[\s"'`]+|[\s"'`]+$/g, "")
    .split(/\r?\n/)
    .map((candidate) => candidate.trim())
    .find(Boolean) || "";
  line = line.replace(/^[A-Z]+(?:\s+N\d+)?\s*(?:radio)?:\s*/i, "").trim();
  line = line.replace(/^\d+[\).]?\s+/, "").trim();
  const sentence = line.match(/^(.{8,120}?[.!?])(?:\s|$)/);
  if (sentence) line = sentence[1];
  line = line.replace(/["“”].*$/, "").trim() || line;
  line = line.replace(/\s+said\s+.*$/i, "").trim();
  return line;
}

function clipText(text, maxChars) {
  const clean = String(text || "").replace(/\s+/g, " ").trim();
  if (clean.length <= maxChars) return clean;
  return clean.slice(0, maxChars).replace(/\s+\S*$/, "").trim();
}

function normalizeChoiceText(text) {
  return String(text || "")
    .toLowerCase()
    .replace(/^\d+[\).]?\s+/, "")
    .replace(/\bboard\b/g, "boards")
    .replace(/\bopens\b/g, "open")
    .replace(/\bprospect refinery\b/g, "prospect ref")
    .replace(/\s+/g, " ")
    .replace(/[.;:,!?-]+/g, "")
    .trim();
}

function choiceTokens(text) {
  return normalizeChoiceText(text)
    .split(/\s+/)
    .filter((token) => token.length > 1);
}

export function resolveChoiceLine(rawText, speaker) {
  const candidates = rotatedChoiceCandidatesForSpeaker(speaker);
  const raw = String(rawText || "").trim();
  const numbered = raw.match(/^(\d+)[\).]?\s+/);
  if (numbered) {
    const index = Number(numbered[1]) - 1;
    if (index >= 0 && index < candidates.length) return candidates[index];
  }

  const clean = cleanLine(raw);
  const normalized = normalizeChoiceText(clean);
  for (const candidate of candidates) {
    if (normalized === normalizeChoiceText(candidate)) return candidate;
  }
  for (const candidate of candidates) {
    const candidateNorm = normalizeChoiceText(candidate);
    if (normalized.length >= 12 &&
        (candidateNorm.startsWith(normalized) ||
         normalized.startsWith(candidateNorm))) {
      return candidate;
    }
  }
  const rawTokens = new Set(choiceTokens(clean));
  if (rawTokens.size >= 4) {
    let best = null;
    let bestScore = 0;
    for (const candidate of candidates) {
      const candidateTokens = choiceTokens(candidate);
      if (!candidateTokens.length) continue;
      const hits = candidateTokens.filter((token) => rawTokens.has(token)).length;
      const score = hits / candidateTokens.length;
      if (score > bestScore) {
        bestScore = score;
        best = candidate;
      }
    }
    if (best && bestScore >= 0.6) return best;
  }
  return clean;
}

export function radioLineIssues(text) {
  const issues = [];
  const line = (text || "").trim();
  if (!line) issues.push("empty");
  if (/\b(ai|assistant|sorry|confusion|format|console|model)\b/i.test(line))
    issues.push("assistant");
  if (/\b(houston|flight|captain|bridge|earth|sir|aye|roger|frequencies|unidentified vessels|miles|o'clock)\b/i.test(line))
    issues.push("offworld");
  if (/\b(remember|report|requesting|verification|confirmation|confirm\w*|acknowledg\w*|affirmative|successfully|reporting for duty)\b/i.test(line))
    issues.push("report");
  if (/\b(all right|okay|hello|received|understood|all clear|all stations|station crew|docking bay|reply accepted|hail accepted|hail received|local signal received|contact established|local pilot|local frequency|traffic's clear ahead|possible incoming|cargo status|cargo holds? empty|fueled and ready|this is|you'?re clear|clear to proceed|no hazards|detected|approximately|holds? (?:no|full|empty).*cargo|holds? full|onhold|on the move|heading out|hauling away|take a closer look|keep it brief|let's see what we've got|what we've got here)\b/i.test(line))
    issues.push("generic");
  if (/["“”*]/.test(line)) issues.push("truncated");
  if (/SIGNAL BELT COMMS|Signal belt radio|signal belt \/|\/\/|\s\/\s|=|\bknows?\b|\bhold empty\b|\bjob\b|\bone-(?:eight|time)\b/i.test(line))
    issues.push("copied_prompt");
  const words = line.split(/\s+/).filter(Boolean);
  if (words.length > 12) issues.push("long");
  if (/[,;]$|\.\.\.?$|\b[A-Z]$/.test(line) || /\b(with|for|to|from|and|the|a|an|any|of|is|when|depart|holding|awaiting|will|hold|hel|helios|prospect|kepler|range|bearing|in\s+\d+)$/i.test(line))
    issues.push("truncated");
  if (/\b(miner|hauler)\s+n$/i.test(line))
    issues.push("truncated");
  return issues;
}

export function speakerRadioLineIssues(text, speaker) {
  const issues = radioLineIssues(text);
  if (speaker.kind !== "npc" || issues.length) return issues;

  const line = text.toLowerCase();
  const ownSlot = Number.isFinite(Number(speaker.slot))
    ? String(Number(speaker.slot)).padStart(2, "0")
    : "";
  const mentionedSlots = [...line.matchAll(/\bn\s*(\d{1,2})\b/gi)]
    .map((match) => match[1].padStart(2, "0"));
  if (ownSlot && mentionedSlots.some((slot) => slot !== ownSlot)) {
    issues.push("wrong_speaker");
  }
  const memories = speaker.market_memories || [];
  const contracts = speaker.known_contracts || [];
  const cargo = speaker.cargo || [];
  const tokens = [
    speaker.home_station_name,
    speaker.dest_station_name,
    ...memories.flatMap((m) => [m.commodity_code, m.station_a_name, m.station_b_name]),
    ...contracts.flatMap((c) => [c.commodity_code, c.station_name]),
    ...cargo.map((c) => c.commodity_code),
  ]
    .filter((token) => token && token !== "no destination")
    .map((token) => String(token).toLowerCase());

  if (tokens.length && !tokens.some((token) => line.includes(token))) {
    issues.push("ungrounded");
  }
  if ((speaker.role || "") === "hauler") {
    const commodityTokens = [
      ...memories.map((m) => m.commodity_code),
      ...contracts.map((c) => c.commodity_code),
      ...cargo.map((c) => c.commodity_code),
    ].filter(Boolean).map((token) => String(token).toLowerCase());
    const dest = speaker.dest_station_name && speaker.dest_station_name !== "no destination"
      ? String(speaker.dest_station_name).toLowerCase()
      : "";
    const destCompact = dest ? compactStationName(speaker.dest_station_name).toLowerCase() : "";
    const mentionsCommodity = commodityTokens.some((token) => line.includes(token));
    const hasHaulFact = commodityTokens.length || dest;
    const mentionsHaulFact = commodityTokens.some((token) => line.includes(token))
      || (dest && line.includes(dest))
      || (destCompact && line.includes(destCompact));
    if (commodityTokens.length && !mentionsCommodity) issues.push("ungrounded");
    if (hasHaulFact && !mentionsHaulFact) issues.push("ungrounded");
  }
  return issues;
}

function firstUsefulMemory(speaker) {
  const memories = speaker.market_memories || [];
  return memories.find((m) => m.kind_name && m.commodity_code) || memories[0];
}

function pickFallback(speaker, choices) {
  const slot = Number.isFinite(Number(speaker.slot)) ? Number(speaker.slot) : 0;
  return choices[Math.abs(slot) % choices.length];
}

export function fallbackRadioLine(speaker) {
  const facts = speakerFacts(speaker);
  if (speaker.kind === "player") return "Open hail; local traffic check.";

  const memory = firstUsefulMemory(speaker);
  const contract = (speaker.known_contracts || [])[0];
  const commodity = memory?.commodity_code || contract?.commodity_code || "";
  const home = facts.home;
  const homeShort = compactStationName(home);
  const dest = facts.dest && facts.dest !== "no destination" ? facts.dest : "";
  const destShort = compactStationName(dest);
  const quantity = contract?.quantity || memory?.quantity_hint || "";
  const quantityPrefix = quantity ? `${quantity} ` : "";
  const role = speaker.role || "worker";
  if (role === "hauler") {
    if (dest && commodity) return pickFallback(speaker, [
      `${commodity} demand logged; moving ${destShort}-side.`,
      `${homeShort} has ${commodity} demand; ${destShort}-side.`,
      `${quantityPrefix}${commodity} tagged ${homeShort}>${destShort}.`,
      `${commodity} board at ${homeShort}; ${destShort} wants it.`,
    ]);
    if (dest) return pickFallback(speaker, [
      `${homeShort} load moving ${destShort}-side.`,
      `${destShort} run is active from ${homeShort}.`,
    ]);
    return `${homeShort} haul memory received.`;
  }
  if (role === "miner") {
    if (memory?.kind_name === "ore pressure" && commodity)
      return pickFallback(speaker, [
        `${commodity} pressure bright at ${homeShort}.`,
        `${commodity} pressure mark holding near ${homeShort}.`,
        `${homeShort} ${commodity} seam is talking.`,
      ]);
    if (commodity) return pickFallback(speaker, [
      `${commodity} mark awake near ${homeShort}.`,
      `${homeShort} wants ${commodity}; rock is speaking.`,
    ]);
    return pickFallback(speaker, [
      `${homeShort} rock mark acquired.`,
      `${homeShort} asteroid leg is quiet.`,
    ]);
  }
  return `${facts.id} holding local signal.`;
}

async function fetchJson(url, options) {
  const res = await fetch(url, options);
  const text = await res.text();
  if (!res.ok) throw new Error(`HTTP ${res.status} from ${url}: ${text.slice(0, 500)}`);
  return JSON.parse(text);
}

async function generateWithOllama(args, prompt) {
  if ((args.mode || "chat") === "chat") return generateWithOllamaChat(args, prompt);
  const data = await fetchJson(new URL("/api/generate", args.ollama), {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      model: args.model,
      prompt,
      stream: false,
      options: {
        temperature: args.temperature,
        num_predict: 12,
        stop: ["\n", "\" said", " said "],
      },
    }),
  });
  return cleanLine(data.response);
}

export function chatUserContentFromPrompt(prompt) {
  return prompt
    .split(/\r?\n/)
    .filter((line) => line.trim())
    .slice(-7)
    .join("\n");
}

async function generateWithOllamaChat(args, prompt) {
  const choicePrompt = /^1\s+/m.test(prompt) && /^2\s+/m.test(prompt);
  const data = await fetchJson(new URL("/api/chat", args.ollama), {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      model: args.model,
      stream: false,
      messages: [
        {
          role: "system",
          content: choicePrompt
            ? "Choose one listed radio line. Output only that line."
            : "Signal Space Miner radio. Speak as the listed ship. One terse line, 4-10 words. No report, advice, remember, confirm, roger, AI, or Earth.",
        },
        { role: "user", content: chatUserContentFromPrompt(prompt) },
      ],
      options: {
        temperature: args.temperature,
        num_predict: 14,
        stop: ["\n"],
      },
    }),
  });
  return cleanLine(data.message?.content || "");
}

async function generateChoiceBatchWithOllama(args, prompt) {
  const data = await fetchJson(new URL("/api/chat", args.ollama), {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      model: args.model,
      stream: false,
      messages: [
        {
          role: "system",
          content: "Output only comma-separated speaker=number choices. Use digits 1, 2, or 3 only.",
        },
        { role: "user", content: prompt },
      ],
      options: {
        temperature: args.temperature,
        num_predict: 40,
        stop: ["\n"],
      },
    }),
  });
  return cleanLine(data.message?.content || "");
}

function recordForSpeaker(context, args, speaker, index, prompt, raw, resolved, issues) {
  const usedFallback = !args.dryRun && issues.length > 0;
  const text = args.dryRun || !usedFallback ? resolved : fallbackRadioLine(speaker);
  const holograms = knowledgeHologramsForSpeaker(speaker);
  return {
    speaker: speakerId(speaker),
    kind: speaker.kind,
    slot: speaker.slot,
    accepted_hail: speaker.kind === "player" ? true : speaker.accepted_hail !== false,
    at_s: scheduledAtForSpeakerIndex(index, args),
    transmit_holograms: speaker.kind === "npc" && holograms.length > 0,
    holograms,
    prompt,
    prompt_chars: prompt.length,
    raw,
    resolved,
    text,
    issues,
    used_fallback: usedFallback,
  };
}

async function generateChoiceBatchConversation(context, args, plan) {
  const hailSalt = Number(context.hail_request_id ?? context.request_id ?? 0) || 0;
  const batchPrompt = buildChoiceBatchPrompt(plan, hailSalt);
  let batchRaw = "";
  let choices = new Map();
  let attempts = 0;
  const retries = Number.isFinite(Number(args.batchChoiceRetries))
    ? Math.max(0, Number(args.batchChoiceRetries))
    : 1;
  for (let attempt = 0; attempt <= retries; attempt++) {
    attempts++;
    batchRaw = await generateChoiceBatchWithOllama(args, batchPrompt);
    choices = parseChoiceBatchResponse(batchRaw, plan);
    const complete = plan.every((speaker) => {
      const choice = choices.get(choiceKeyForSpeaker(speaker));
      return choice >= 1 &&
        choice <= rotatedChoiceCandidatesForSpeaker(speaker, hailSalt).length;
    });
    if (complete) break;
  }
  const records = [];

  for (let index = 0; index < plan.length; index++) {
    const speaker = plan[index];
    const candidates = rotatedChoiceCandidatesForSpeaker(speaker, hailSalt);
    const choice = choices.get(choiceKeyForSpeaker(speaker));
    const prompt = buildSpeakerPrompt(context, speaker, records);
    let resolved = "";
    let issues = [];
    if (choice >= 1 && choice <= candidates.length) {
      resolved = candidates[choice - 1];
      issues = speakerRadioLineIssues(resolved, speaker);
    } else {
      resolved = fallbackRadioLine(speaker);
      issues = ["missing_choice"];
    }
    records.push(recordForSpeaker(
      context, args, speaker, index, prompt, batchRaw, resolved, issues));
  }
  records.generation = {
    mode: "choice_batch",
    llm_calls: attempts,
    prompt_chars: batchPrompt.length * attempts,
    batch_prompt_chars: batchPrompt.length,
    batch_attempts: attempts,
  };
  return records;
}

export async function generateConversation(context, args) {
  context.prompt_variant = args.variant || context.prompt_variant || "choice";
  const plan = buildConversationPlan(context, args.maxSpeakers);
  if (!args.dryRun && context.prompt_variant === "choice" &&
      args.batchChoice !== false && (args.mode || "chat") === "chat") {
    return generateChoiceBatchConversation(context, args, plan);
  }

  const records = [];
  for (let index = 0; index < plan.length; index++) {
    const speaker = plan[index];
    const prompt = buildSpeakerPrompt(context, speaker, records);
    const raw = args.dryRun ? "" : await generateWithOllama(args, prompt);
    const resolved = !args.dryRun && context.prompt_variant === "choice"
      ? resolveChoiceLine(raw, speaker)
      : raw;
    const issues = args.dryRun ? [] : speakerRadioLineIssues(resolved, speaker);
    records.push(recordForSpeaker(
      context, args, speaker, index, prompt, raw, resolved, issues));
  }
  records.generation = {
    mode: args.dryRun ? "dry_run" : "per_speaker",
    llm_calls: args.dryRun ? 0 : plan.length,
    prompt_chars: records.reduce((sum, record) => sum + (record.prompt_chars || 0), 0),
    batch_prompt_chars: 0,
    batch_attempts: 0,
  };
  return records;
}

export function conversationRuntimePayload(context, records) {
  const player = records.find((record) => record.kind === "player");
  const npcLines = records
    .filter((record) => record.kind === "npc" && record.accepted_hail)
    .map((record) => ({
      npc_index: Number.isFinite(Number(record.slot)) ? Number(record.slot) : -1,
      speaker: record.speaker,
      at_s: record.at_s,
      line: record.text || "",
      transmit_holograms: record.transmit_holograms,
      hologram_labels: (record.holograms || []).map((hologram) => hologram.label),
      holograms: record.transmit_holograms ? record.holograms : [],
      used_fallback: record.used_fallback,
      issues: record.issues || [],
    }));
  return {
    world: context.world || null,
    generation: records.generation || null,
    player_line: player?.text || "",
    npc_lines: npcLines,
  };
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const context = await readContext(args);
  const records = await generateConversation(context, args);
  if (args.runtimeJson) {
    console.log(JSON.stringify(conversationRuntimePayload(context, records), null, 2));
    return;
  }
  if (args.json) {
    console.log(JSON.stringify({
      world: context.world,
      generation: records.generation || null,
      records,
      runtime: conversationRuntimePayload(context, records),
    }, null, 2));
    return;
  }
  for (const record of records) {
    const at = Number.isFinite(record.at_s) ? `t+${record.at_s.toFixed(1)}s` : "t+?";
    console.log(`\n[${at}] ${record.speaker}:`);
    console.log(args.dryRun ? record.prompt : (record.text || "(empty response)"));
    if (!args.dryRun && record.transmit_holograms) {
      const labels = record.holograms.map((h) => h.label).slice(0, 3);
      const more = record.holograms.length > labels.length
        ? ` +${record.holograms.length - labels.length} more`
        : "";
      console.log(`holograms: ${labels.join(" | ")}${more}`);
    }
  }
}

if (import.meta.url === `file://${process.argv[1]}`) {
  main().catch((err) => {
    console.error(err.message);
    process.exit(1);
  });
}
