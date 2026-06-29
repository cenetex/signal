#!/usr/bin/env node

import { readFile } from 'node:fs/promises';

const DEFAULT_SCHEMA = 'crlp.signal_hnn_shadow.v1';

function usage() {
  console.log(`usage: node scripts/analyze-signal-hnn-shadow.mjs [options] [FILE...]

Analyze Signal HNN shadow JSONL from client stdout or extracted logs.

Options:
  --input FILE                  Add an input file. Positional files also work.
  --schema NAME                 Schema to analyze (default: ${DEFAULT_SCHEMA})
  --min-rows N                  Fail below N matching rows (default: 1)
  --min-teacher-rows N          Fail below N rows with teacher labels (default: 1)
  --min-teacher-match-rate N    Fail below teacher top-1 legal match rate, 0..1
  --min-p50-margin N            Fail below median HNN legal margin
  --min-p50-fidelity N          Fail below median trace fidelity, 0..1
  --max-p90-capacity-load N     Fail above p90 trace capacity load
  --json                        Emit JSON summary only
  --help                        Show this help

If no files are provided, stdin is read.`);
}

function parseArgs(argv) {
  const args = {
    files: [],
    schema: DEFAULT_SCHEMA,
    minRows: 1,
    minTeacherRows: 1,
    minTeacherMatchRate: null,
    minP50Margin: null,
    minP50Fidelity: null,
    maxP90CapacityLoad: null,
    json: false,
  };

  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--help' || arg === '-h') {
      usage();
      process.exit(0);
    }
    if (arg === '--json') {
      args.json = true;
      continue;
    }
    if (arg === '--input') {
      const value = argv[++i];
      if (!value) throw new Error('--input requires a file');
      args.files.push(value);
      continue;
    }
    if (arg.startsWith('--input=')) {
      args.files.push(arg.slice('--input='.length));
      continue;
    }
    if (arg.startsWith('--') && arg.includes('=')) {
      const splitAt = arg.indexOf('=');
      const rawKey = arg.slice(2, splitAt);
      const key = rawKey.replace(/-([a-z])/g, (_, c) => c.toUpperCase());
      const value = arg.slice(splitAt + 1);
      if (!(key in args)) throw new Error(`unknown option: --${rawKey}`);
      args[key] = value;
      continue;
    }
    if (arg.startsWith('--')) {
      const key = arg.slice(2).replace(/-([a-z])/g, (_, c) => c.toUpperCase());
      const value = argv[++i];
      if (value == null) throw new Error(`${arg} requires a value`);
      if (!(key in args)) throw new Error(`unknown option: ${arg}`);
      args[key] = value;
      continue;
    }
    args.files.push(arg);
  }

  args.minRows = parseNonNegativeInt(args.minRows, '--min-rows');
  args.minTeacherRows = parseNonNegativeInt(args.minTeacherRows, '--min-teacher-rows');
  args.minTeacherMatchRate =
    args.minTeacherMatchRate == null
      ? null
      : parseUnitFloat(args.minTeacherMatchRate, '--min-teacher-match-rate');
  args.minP50Margin =
    args.minP50Margin == null
      ? null
      : parseFiniteFloat(args.minP50Margin, '--min-p50-margin');
  args.minP50Fidelity =
    args.minP50Fidelity == null
      ? null
      : parseUnitFloat(args.minP50Fidelity, '--min-p50-fidelity');
  args.maxP90CapacityLoad =
    args.maxP90CapacityLoad == null
      ? null
      : parseFiniteFloat(args.maxP90CapacityLoad, '--max-p90-capacity-load');
  return args;
}

function parseNonNegativeInt(value, label) {
  const parsed = Number(value);
  if (!Number.isInteger(parsed) || parsed < 0) {
    throw new Error(`${label} must be a non-negative integer`);
  }
  return parsed;
}

function parseUnitFloat(value, label) {
  const parsed = Number(value);
  if (!Number.isFinite(parsed) || parsed < 0 || parsed > 1) {
    throw new Error(`${label} must be a number in [0, 1]`);
  }
  return parsed;
}

function parseFiniteFloat(value, label) {
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) throw new Error(`${label} must be finite`);
  return parsed;
}

async function readStdin() {
  const chunks = [];
  for await (const chunk of process.stdin) chunks.push(chunk);
  return Buffer.concat(chunks).toString('utf8');
}

async function readInputs(files) {
  if (files.length === 0) return [{ source: '<stdin>', text: await readStdin() }];
  return Promise.all(files.map(async (file) => ({
    source: file,
    text: await readFile(file, 'utf8'),
  })));
}

function tryParseJsonLine(line) {
  const trimmed = line.trim();
  if (!trimmed.startsWith('{')) return null;
  try {
    return JSON.parse(trimmed);
  } catch {
    return null;
  }
}

function actionKey(action) {
  if (!action || typeof action !== 'object') return 'missing';
  const index = Number.isInteger(action.index) ? action.index : '?';
  const name = typeof action.name === 'string' && action.name.length > 0
    ? action.name
    : '?';
  return `${index}:${name}`;
}

function increment(map, key, amount = 1) {
  map.set(key, (map.get(key) || 0) + amount);
}

function round(value, digits = 6) {
  if (!Number.isFinite(value)) return null;
  const scale = 10 ** digits;
  return Math.round(value * scale) / scale;
}

function percentile(values, p) {
  if (values.length === 0) return null;
  const sorted = [...values].sort((a, b) => a - b);
  const idx = Math.min(sorted.length - 1, Math.max(0, Math.floor(p * (sorted.length - 1))));
  return sorted[idx];
}

function summarizeValues(values) {
  if (values.length === 0) {
    return { count: 0, min: null, p10: null, p50: null, p90: null, max: null, avg: null };
  }
  const sum = values.reduce((acc, value) => acc + value, 0);
  return {
    count: values.length,
    min: round(Math.min(...values)),
    p10: round(percentile(values, 0.10)),
    p50: round(percentile(values, 0.50)),
    p90: round(percentile(values, 0.90)),
    max: round(Math.max(...values)),
    avg: round(sum / values.length),
  };
}

function rowBestAllowed(row) {
  return row.hnn_top_allowed || row.best_allowed || null;
}

function rowMargin(row) {
  if (Number.isFinite(row.margin)) return row.margin;
  if (Number.isFinite(row.allowed_margin)) return row.allowed_margin;
  if (Number.isFinite(row.contract?.last_margin)) return row.contract.last_margin;
  return null;
}

function rowFidelity(row) {
  if (Number.isFinite(row.trace_fidelity)) return row.trace_fidelity;
  if (Number.isFinite(row.contract?.fidelity_estimate)) return row.contract.fidelity_estimate;
  return null;
}

function rowCapacityLoad(row) {
  if (Number.isFinite(row.capacity_load)) return row.capacity_load;
  if (Number.isFinite(row.contract?.capacity_load)) return row.contract.capacity_load;
  return null;
}

function contractKey(contract) {
  if (!contract || typeof contract !== 'object') return 'missing';
  return [
    `dim=${contract.dim ?? '?'}`,
    `seed=${contract.seed ?? '?'}`,
    `keygen=${contract.keygen_version ?? '?'}`,
    `encoder=${contract.encoder_version ?? '?'}`,
    `trace=${contract.trace_format_version ?? '?'}`,
    `vocab=${contract.action_vocabulary_hash ?? '?'}`,
  ].join('/');
}

function teacherRank(row) {
  const teacher = row.teacher;
  if (!teacher || !Number.isInteger(teacher.index) || !Array.isArray(row.actions)) {
    return null;
  }
  const allowed = row.actions
    .filter((action) => action && action.allowed === true && Number.isFinite(action.score))
    .sort((a, b) => b.score - a.score);
  const rank = allowed.findIndex((action) => action.index === teacher.index);
  return rank >= 0 ? rank + 1 : null;
}

function analyzeRows(rows) {
  const hnnTopCounts = new Map();
  const hnnAllowedCounts = new Map();
  const teacherCounts = new Map();
  const confusion = new Map();
  const rankCounts = new Map();
  const contracts = new Map();
  const holonetContracts = new Map();
  const holonetRouteCounts = new Map();
  const margins = [];
  const fidelities = [];
  const capacityLoads = [];
  const storedCounts = [];
  const holonetActiveCounts = [];
  const holonetRouteSimilarities = [];

  let teacherRows = 0;
  let teacherMatchesAllowed = 0;
  let teacherMatchesRaw = 0;
  let nullTeacherRows = 0;

  for (const row of rows) {
    const bestAllowed = rowBestAllowed(row);
    increment(hnnTopCounts, actionKey(row.hnn_top));
    increment(hnnAllowedCounts, actionKey(bestAllowed));
    increment(contracts, contractKey(row.contract));
    if (row.holonet && typeof row.holonet === 'object') {
      increment(holonetContracts, contractKey(row.holonet.contract));
      const route = Number.isInteger(row.holonet.last_route)
        ? String(row.holonet.last_route)
        : 'missing';
      increment(holonetRouteCounts, route);
      if (Number.isFinite(row.holonet.active_count)) {
        holonetActiveCounts.push(row.holonet.active_count);
      }
      if (Number.isFinite(row.holonet.route_similarity)) {
        holonetRouteSimilarities.push(row.holonet.route_similarity);
      }
    }

    const margin = rowMargin(row);
    if (Number.isFinite(margin)) margins.push(margin);
    const fidelity = rowFidelity(row);
    if (Number.isFinite(fidelity)) fidelities.push(fidelity);
    const load = rowCapacityLoad(row);
    if (Number.isFinite(load)) capacityLoads.push(load);
    const storedCount = Number.isFinite(row.stored_count)
      ? row.stored_count
      : row.contract?.stored_count;
    if (Number.isFinite(storedCount)) storedCounts.push(storedCount);

    if (!row.teacher) {
      nullTeacherRows++;
      continue;
    }

    teacherRows++;
    const teacherKey = actionKey(row.teacher);
    const bestAllowedKey = actionKey(bestAllowed);
    increment(teacherCounts, teacherKey);
    increment(confusion, `${teacherKey} -> ${bestAllowedKey}`);
    if (row.teacher.matches_best_allowed === true ||
        row.teacher.index === bestAllowed?.index) {
      teacherMatchesAllowed++;
    }
    if (row.teacher.matches_hnn_top === true ||
        row.teacher.index === row.hnn_top?.index) {
      teacherMatchesRaw++;
    }
    const rank = teacherRank(row);
    increment(rankCounts, rank == null ? 'missing' : String(rank));
  }

  const teacherAllowedMatchRate = teacherRows > 0
    ? teacherMatchesAllowed / teacherRows
    : null;
  const teacherRawMatchRate = teacherRows > 0
    ? teacherMatchesRaw / teacherRows
    : null;

  return {
    schema: rows[0]?.schema || DEFAULT_SCHEMA,
    rows: rows.length,
    teacherRows,
    nullTeacherRows,
    teacherMatchesAllowed,
    teacherAllowedMatchRate: round(teacherAllowedMatchRate),
    teacherMatchesRaw,
    teacherRawMatchRate: round(teacherRawMatchRate),
    margin: summarizeValues(margins),
    traceFidelity: summarizeValues(fidelities),
    capacityLoad: summarizeValues(capacityLoads),
    storedCount: summarizeValues(storedCounts),
    holonetActiveCount: summarizeValues(holonetActiveCounts),
    holonetRouteSimilarity: summarizeValues(holonetRouteSimilarities),
    teacherRankCounts: Object.fromEntries([...rankCounts.entries()].sort((a, b) => {
      if (a[0] === 'missing') return 1;
      if (b[0] === 'missing') return -1;
      return Number(a[0]) - Number(b[0]);
    })),
    hnnTopCounts: Object.fromEntries([...hnnTopCounts.entries()].sort()),
    hnnAllowedCounts: Object.fromEntries([...hnnAllowedCounts.entries()].sort()),
    teacherCounts: Object.fromEntries([...teacherCounts.entries()].sort()),
    confusion: Object.fromEntries([...confusion.entries()].sort()),
    contracts: Object.fromEntries([...contracts.entries()].sort()),
    holonetRouteCounts: Object.fromEntries([...holonetRouteCounts.entries()].sort()),
    holonetContracts: Object.fromEntries([...holonetContracts.entries()].sort()),
  };
}

function collectRows(inputs, schema) {
  const rows = [];
  const parseStats = {
    sources: inputs.length,
    lines: 0,
    jsonLines: 0,
    matchingRows: 0,
  };

  for (const input of inputs) {
    const lines = input.text.split(/\r?\n/);
    for (let i = 0; i < lines.length; i++) {
      parseStats.lines++;
      const parsed = tryParseJsonLine(lines[i]);
      if (!parsed) continue;
      parseStats.jsonLines++;
      if (parsed.schema !== schema) continue;
      parsed._source = input.source;
      parsed._line = i + 1;
      rows.push(parsed);
      parseStats.matchingRows++;
    }
  }
  return { rows, parseStats };
}

function checkThresholds(summary, args) {
  const failures = [];
  if (summary.rows < args.minRows) {
    failures.push(`rows ${summary.rows} < ${args.minRows}`);
  }
  if (summary.teacherRows < args.minTeacherRows) {
    failures.push(`teacher_rows ${summary.teacherRows} < ${args.minTeacherRows}`);
  }
  if (args.minTeacherMatchRate != null) {
    const rate = summary.teacherAllowedMatchRate == null
      ? 0
      : summary.teacherAllowedMatchRate;
    if (rate < args.minTeacherMatchRate) {
      failures.push(`teacher_allowed_match_rate ${rate} < ${args.minTeacherMatchRate}`);
    }
  }
  if (args.minP50Margin != null) {
    const margin = summary.margin.p50 == null ? -Infinity : summary.margin.p50;
    if (margin < args.minP50Margin) {
      failures.push(`margin.p50 ${margin} < ${args.minP50Margin}`);
    }
  }
  if (args.minP50Fidelity != null) {
    const fidelity = summary.traceFidelity.p50 == null ? 0 : summary.traceFidelity.p50;
    if (fidelity < args.minP50Fidelity) {
      failures.push(`trace_fidelity.p50 ${fidelity} < ${args.minP50Fidelity}`);
    }
  }
  if (args.maxP90CapacityLoad != null) {
    const load = summary.capacityLoad.p90 == null ? 0 : summary.capacityLoad.p90;
    if (load > args.maxP90CapacityLoad) {
      failures.push(`capacity_load.p90 ${load} > ${args.maxP90CapacityLoad}`);
    }
  }
  return failures;
}

function printHuman(summary, parseStats, failures) {
  console.log(`schema=${summary.schema}`);
  console.log(`rows=${summary.rows} teacher_rows=${summary.teacherRows} null_teacher_rows=${summary.nullTeacherRows}`);
  console.log(`teacher_allowed_match_rate=${summary.teacherAllowedMatchRate ?? 'n/a'} teacher_raw_match_rate=${summary.teacherRawMatchRate ?? 'n/a'}`);
  console.log(`margin=${JSON.stringify(summary.margin)}`);
  console.log(`trace_fidelity=${JSON.stringify(summary.traceFidelity)}`);
  console.log(`capacity_load=${JSON.stringify(summary.capacityLoad)}`);
  console.log(`stored_count=${JSON.stringify(summary.storedCount)}`);
  console.log(`holonet_active_count=${JSON.stringify(summary.holonetActiveCount)}`);
  console.log(`holonet_route_similarity=${JSON.stringify(summary.holonetRouteSimilarity)}`);
  console.log(`teacher_rank_counts=${JSON.stringify(summary.teacherRankCounts)}`);
  console.log(`hnn_top_counts=${JSON.stringify(summary.hnnTopCounts)}`);
  console.log(`hnn_allowed_counts=${JSON.stringify(summary.hnnAllowedCounts)}`);
  console.log(`teacher_counts=${JSON.stringify(summary.teacherCounts)}`);
  console.log(`confusion=${JSON.stringify(summary.confusion)}`);
  console.log(`contracts=${JSON.stringify(summary.contracts)}`);
  console.log(`holonet_route_counts=${JSON.stringify(summary.holonetRouteCounts)}`);
  console.log(`holonet_contracts=${JSON.stringify(summary.holonetContracts)}`);
  console.log(`parsed_lines=${parseStats.lines} json_lines=${parseStats.jsonLines} matching_rows=${parseStats.matchingRows}`);
  if (failures.length > 0) {
    console.error(`signal-hnn-shadow: threshold failure: ${failures.join('; ')}`);
  }
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const inputs = await readInputs(args.files);
  const { rows, parseStats } = collectRows(inputs, args.schema);
  const summary = analyzeRows(rows);
  summary.schema = args.schema;
  const failures = checkThresholds(summary, args);

  if (args.json) {
    console.log(JSON.stringify({ ...summary, parseStats, failures }, null, 2));
  } else {
    printHuman(summary, parseStats, failures);
  }

  if (failures.length > 0) process.exit(1);
}

main().catch((err) => {
  console.error(`signal-hnn-shadow: ${err.message}`);
  process.exit(1);
});
