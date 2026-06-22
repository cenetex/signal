#!/usr/bin/env node

import { readFile } from 'node:fs/promises';

const DEFAULT_SCHEMA = 'crlp.signal_client_flight_shadow.v1';

function usage() {
  console.log(`usage: node scripts/analyze-signal-client-brain-shadow.mjs [options] [FILE...]

Analyze Signal client brain shadow JSONL from client stdout or extracted logs.

Options:
  --input FILE                  Add an input file. Positional files also work.
  --schema NAME                 Schema to analyze (default: ${DEFAULT_SCHEMA})
  --min-rows N                  Fail below N matching rows (default: 1)
  --min-teacher-rows N          Fail below N rows with teacher labels (default: 1)
  --min-teacher-match-rate N    Fail below teacher top-1 match rate, 0..1
  --min-p50-margin N            Fail below median best-allowed margin
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
  const bestAllowedCounts = new Map();
  const teacherCounts = new Map();
  const confusion = new Map();
  const rankCounts = new Map();
  const margins = [];
  const teacherMargins = [];
  const modelVersions = new Map();

  let teacherRows = 0;
  let teacherMatches = 0;
  let teacherScoreWins = 0;
  let teacherScoreKnown = 0;
  let bestRawDisallowed = 0;
  let nullTeacherRows = 0;

  for (const row of rows) {
    increment(bestAllowedCounts, actionKey(row.best_allowed));
    if (typeof row.feature_set === 'string') {
      const version = `${row.feature_set}@${row.feature_encoder_version ?? '?'}`;
      increment(modelVersions, version);
    }
    if (Number.isFinite(row.allowed_margin)) margins.push(row.allowed_margin);

    const bestRawKey = actionKey(row.best_raw);
    const bestAllowedKey = actionKey(row.best_allowed);
    if (bestRawKey !== 'missing' && bestRawKey !== bestAllowedKey) {
      bestRawDisallowed++;
    }

    if (!row.teacher) {
      nullTeacherRows++;
      continue;
    }

    teacherRows++;
    const teacherKey = actionKey(row.teacher);
    increment(teacherCounts, teacherKey);
    increment(confusion, `${teacherKey} -> ${bestAllowedKey}`);
    if (row.teacher.matches_best_allowed === true) teacherMatches++;

    const rank = teacherRank(row);
    increment(rankCounts, rank == null ? 'missing' : String(rank));

    if (Number.isFinite(row.teacher.score) && Number.isFinite(row.best_allowed?.score)) {
      teacherScoreKnown++;
      const delta = row.best_allowed.score - row.teacher.score;
      teacherMargins.push(delta);
      if (delta <= 0) teacherScoreWins++;
    }
  }

  const teacherMatchRate = teacherRows > 0 ? teacherMatches / teacherRows : null;
  const teacherScoreWinRate = teacherScoreKnown > 0 ? teacherScoreWins / teacherScoreKnown : null;

  return {
    schema: rows[0]?.schema || DEFAULT_SCHEMA,
    rows: rows.length,
    teacherRows,
    nullTeacherRows,
    teacherMatches,
    teacherMatchRate: round(teacherMatchRate),
    teacherScoreKnown,
    teacherScoreWins,
    teacherScoreWinRate: round(teacherScoreWinRate),
    bestRawDisallowed,
    allowedMargin: summarizeValues(margins),
    bestMinusTeacherScore: summarizeValues(teacherMargins),
    teacherRankCounts: Object.fromEntries([...rankCounts.entries()].sort((a, b) => {
      if (a[0] === 'missing') return 1;
      if (b[0] === 'missing') return -1;
      return Number(a[0]) - Number(b[0]);
    })),
    bestAllowedCounts: Object.fromEntries([...bestAllowedCounts.entries()].sort()),
    teacherCounts: Object.fromEntries([...teacherCounts.entries()].sort()),
    confusion: Object.fromEntries([...confusion.entries()].sort()),
    modelVersions: Object.fromEntries([...modelVersions.entries()].sort()),
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
    const rate = summary.teacherMatchRate == null ? 0 : summary.teacherMatchRate;
    if (rate < args.minTeacherMatchRate) {
      failures.push(`teacher_match_rate ${rate} < ${args.minTeacherMatchRate}`);
    }
  }
  if (args.minP50Margin != null) {
    const margin = summary.allowedMargin.p50 == null ? -Infinity : summary.allowedMargin.p50;
    if (margin < args.minP50Margin) {
      failures.push(`allowed_margin.p50 ${margin} < ${args.minP50Margin}`);
    }
  }
  return failures;
}

function printHuman(summary, parseStats, failures) {
  console.log(`schema=${summary.schema}`);
  console.log(`rows=${summary.rows} teacher_rows=${summary.teacherRows} null_teacher_rows=${summary.nullTeacherRows}`);
  console.log(`teacher_match_rate=${summary.teacherMatchRate ?? 'n/a'} teacher_score_win_rate=${summary.teacherScoreWinRate ?? 'n/a'}`);
  console.log(`allowed_margin=${JSON.stringify(summary.allowedMargin)}`);
  console.log(`best_minus_teacher_score=${JSON.stringify(summary.bestMinusTeacherScore)}`);
  console.log(`teacher_rank_counts=${JSON.stringify(summary.teacherRankCounts)}`);
  console.log(`best_raw_disallowed=${summary.bestRawDisallowed}`);
  console.log(`best_allowed_counts=${JSON.stringify(summary.bestAllowedCounts)}`);
  console.log(`teacher_counts=${JSON.stringify(summary.teacherCounts)}`);
  console.log(`confusion=${JSON.stringify(summary.confusion)}`);
  console.log(`model_versions=${JSON.stringify(summary.modelVersions)}`);
  console.log(`parsed_lines=${parseStats.lines} json_lines=${parseStats.jsonLines} matching_rows=${parseStats.matchingRows}`);
  if (failures.length > 0) {
    console.error(`signal-client-brain-shadow: threshold failure: ${failures.join('; ')}`);
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
  console.error(`signal-client-brain-shadow: ${err.message}`);
  process.exit(1);
});
