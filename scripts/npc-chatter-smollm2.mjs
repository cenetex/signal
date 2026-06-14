#!/usr/bin/env node

const DEFAULT_SERVER = "http://127.0.0.1:8080";
const DEFAULT_OLLAMA = "http://127.0.0.1:11434";
const DEFAULT_MODEL = "smollm2";

function usage() {
  console.log(`Usage: node scripts/npc-chatter-smollm2.mjs [options]

Options:
  --server URL       Signal server API (default: ${DEFAULT_SERVER})
  --ollama URL       Ollama API base (default: ${DEFAULT_OLLAMA})
  --model NAME       Ollama model (default: ${DEFAULT_MODEL})
  --slot N           Specific NPC slot
  --role NAME        miner, hauler, tow, or any
  --station N        Match NPC home/dest/pickup station
  --limit N          Number of NPCs to sample (default: 3)
  --temperature N    Ollama temperature (default: 0.75)
  --dry-run          Print prompts without calling Ollama
  --json             Emit JSON records
`);
}

function parseArgs(argv) {
  const args = {
    server: DEFAULT_SERVER,
    ollama: DEFAULT_OLLAMA,
    model: DEFAULT_MODEL,
    limit: "3",
    temperature: 0.75,
    dryRun: false,
    json: false,
  };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === "--help" || arg === "-h") {
      usage();
      process.exit(0);
    } else if (arg === "--dry-run") {
      args.dryRun = true;
    } else if (arg === "--json") {
      args.json = true;
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
  return args;
}

async function fetchJson(url, options) {
  const res = await fetch(url, options);
  const text = await res.text();
  if (!res.ok) {
    throw new Error(`HTTP ${res.status} from ${url}: ${text.slice(0, 500)}`);
  }
  return JSON.parse(text);
}

function buildContextUrl(args) {
  const url = new URL("/api/npc_chatter_context", args.server);
  for (const key of ["slot", "role", "station", "limit"]) {
    if (args[key] != null) url.searchParams.set(key, String(args[key]));
  }
  return url;
}

function stationName(npc, key) {
  const idx = npc[key];
  if (idx == null || idx < 0 || idx === 255) return "none";
  return npc[`${key}_name`] || `station ${idx}`;
}

function memoryStationName(memory, key) {
  const idx = memory[key];
  if (idx == null || idx < 0 || idx === 255) return "somewhere";
  return memory[`${key}_name`] || `station ${idx}`;
}

function fmtMemory(memory) {
  const from = memoryStationName(memory, "station_a");
  const to = memoryStationName(memory, "station_b");
  const commodity = memory.commodity_code || "UNK";
  const action = memory.action_name || "work";
  const route = to === "somewhere" ? from : `${from} to ${to}`;
  return `${memory.kind_name}: ${action} ${commodity} around ${route}`;
}

function fmtJob(job) {
  const from = job.source_name || `station ${job.source}`;
  const to = job.dest_name || `station ${job.dest}`;
  const commodity = job.commodity_code || "UNK";
  const picked = job.selected ? "selected" : "considered";
  return `${picked}: ${from} to ${to}, ${commodity}, ${job.memory_kind_name}`;
}

function buildPrompt(world, npc) {
  const ident = `${npc.role.toUpperCase()} N${String(npc.slot).padStart(2, "0")}`;
  const home = stationName(npc, "home_station");
  const dest = stationName(npc, "dest_station");
  const pickup = stationName(npc, "pickup_station");
  const cargo = npc.cargo && npc.cargo.length
    ? npc.cargo.map((c) => `${c.amount} ${c.commodity_code}`).join(", ")
    : "empty hold";
  const selectedJobs = (npc.job_diagnostics || []).filter((j) => j.selected);
  const jobs = (selectedJobs.length ? selectedJobs : (npc.job_diagnostics || []).slice(0, 2))
    .map(fmtJob);
  const memories = (npc.market_memories || [])
    .slice()
    .sort((a, b) => (b.salience + b.confidence) - (a.salience + a.confidence))
    .slice(0, 6)
    .map(fmtMemory);
  const contracts = (npc.known_contracts || []).slice(0, 4).map((c) => {
    const station = c.station_name || `station ${c.station}`;
    return `${c.action_name} ${c.quantity} ${c.commodity_code} for ${station}, price ${c.price}, age ${c.age}`;
  });

  const status = [
    `${ident}`,
    npc.state,
    `home ${home}`,
    dest !== "none" ? `dest ${dest}` : "",
    pickup !== "none" ? `pickup ${pickup}` : "",
    `hull ${Math.round(npc.hull)}`,
    `cargo ${cargo}`,
  ].filter(Boolean).join(" // ");
  const lines = [`${status}`];
  if (jobs.length) lines.push(...jobs.map((line) => `job ${line}`));
  if (memories.length) lines.push(...memories.map((line) => `memory ${line}`));
  if (contracts.length) lines.push(...contracts.map((line) => `contract ${line}`));
  lines.push(`${ident} radio:`);
  return lines.join("\n");
}

function cleanLine(text) {
  let line = text
    .replace(/^[\s"'`]+|[\s"'`]+$/g, "")
    .split(/\r?\n/)
    .map((line) => line.trim())
    .find(Boolean) || "";
  line = line.replace(/^[A-Z]+ N\d+\s*(?:radio)?:\s*/i, "").trim();
  const sentence = line.match(/^(.{8,120}?[.!?])(?:\s|$)/);
  if (sentence) line = sentence[1];
  return line;
}

async function generateWithOllama(args, prompt) {
  const payload = {
    model: args.model,
    prompt,
    stream: false,
    options: {
      temperature: args.temperature,
      num_predict: 18,
      stop: ["\n", "Radio:"],
    },
  };
  const data = await fetchJson(new URL("/api/generate", args.ollama), {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload),
  });
  return cleanLine(data.response || "");
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const context = await fetchJson(buildContextUrl(args));
  const records = [];
  for (const npc of context.npcs || []) {
    const prompt = buildPrompt(context.world, npc);
    const line = args.dryRun ? "" : await generateWithOllama(args, prompt);
    records.push({ slot: npc.slot, role: npc.role, state: npc.state, prompt, line });
  }

  if (args.json) {
    console.log(JSON.stringify({ world: context.world, records }, null, 2));
    return;
  }
  if (records.length === 0) {
    console.log("No matching active NPCs.");
    return;
  }
  for (const record of records) {
    console.log(`\n[N${String(record.slot).padStart(2, "0")} ${record.role} ${record.state}]`);
    if (args.dryRun) {
      console.log(record.prompt);
    } else {
      console.log(record.line || "(empty response)");
    }
  }
}

main().catch((err) => {
  console.error(err.message);
  process.exit(1);
});
