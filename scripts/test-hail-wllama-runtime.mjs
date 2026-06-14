#!/usr/bin/env node

import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

const runtimeSource = readFileSync(
  new URL("../web/signal-hail-wllama-runtime.js", import.meta.url),
  "utf8",
);
const runtimeModule = await import(
  `data:text/javascript;base64,${Buffer.from(runtimeSource).toString("base64")}`
);
const { choiceGrammarForPrompt, normalizeChoiceResponse } = runtimeModule;

const SAMPLE_PROMPT = [
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
].join("\n");

test("normalizes compact model assignment text", () => {
  assert.equal(
    normalizeChoiceResponse(SAMPLE_PROMPT, "YOU=2, N00=3, N02=1"),
    "YOU=2,N00=3,N02=1",
  );
});

test("builds a constrained grammar for the active speaker keys", () => {
  assert.equal(choiceGrammarForPrompt(SAMPLE_PROMPT), [
    'root ::= "YOU=" choice "," "N00=" choice "," "N02=" choice',
    'choice ::= "1" | "2" | "3"',
  ].join("\n"));
});

test("filters assignment keys that are not present in the prompt", () => {
  const playerOnly = [
    "local hail choices",
    "YOU:",
    "1 Open hail; local traffic check.",
    "2 Local traffic, sound off.",
    "3 Open channel; nearby traffic check.",
    "Return all speaker keys. No words.",
    "Example: YOU=1",
    "ANSWER:",
  ].join("\n");
  assert.equal(
    normalizeChoiceResponse(playerOnly, "YOU=1,N01=2"),
    "YOU=1",
  );
});

test("normalizes bare number sequences in speaker order", () => {
  assert.equal(
    normalizeChoiceResponse(SAMPLE_PROMPT, "1, 2, 3"),
    "YOU=1,N00=2,N02=3",
  );
  assert.equal(
    normalizeChoiceResponse(SAMPLE_PROMPT, "1"),
    "YOU=1",
  );
});

test("normalizes SmolLM2 selected player line text to a key choice", () => {
  assert.equal(
    normalizeChoiceResponse(SAMPLE_PROMPT, "1 Open hail local traffic check"),
    "YOU=1",
  );
});

test("normalizes selected NPC line text to its speaker key choice", () => {
  assert.equal(
    normalizeChoiceResponse(SAMPLE_PROMPT, "Prospect Ref FE face is active."),
    "N00=2",
  );
  assert.equal(
    normalizeChoiceResponse(SAMPLE_PROMPT, "108 FR open at Kepler Yard; en route."),
    "N02=2",
  );
});

test("passes through unknown sanitized text for C parser fallback", () => {
  assert.equal(
    normalizeChoiceResponse(SAMPLE_PROMPT, "YOU maybe one"),
    "YOU maybe one",
  );
});
