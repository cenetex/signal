#!/usr/bin/env node

import assert from "node:assert/strict";
import test from "node:test";

import {
  SAMPLE_HAIL_CONTEXT,
  buildConversationPlan,
  buildChoiceBatchPrompt,
  buildSpeakerPrompt,
  candidateRadioLinesForSpeaker,
  chatUserContentFromPrompt,
  choiceKeyForSpeaker,
  conversationRuntimePayload,
  fallbackRadioLine,
  generateConversation,
  knowledgeHologramsForSpeaker,
  parseChoiceBatchResponse,
  radioLineIssues,
  realHailContextFromNpcSnapshot,
  resolveChoiceLine,
  rotatedChoiceCandidatesForSpeaker,
  scheduledAtForSpeakerIndex,
  speakerRadioLineIssues,
} from "./hail-conversation-smollm2.mjs";

test("sample hail plan starts with player then accepted NPCs by proximity", () => {
  const plan = buildConversationPlan(SAMPLE_HAIL_CONTEXT, 3);
  assert.equal(plan.length, 4);
  assert.equal(plan[0].kind, "player");
  assert.equal(plan[1].slot, 0);
  assert.equal(plan[2].slot, 1);
  assert.equal(plan[3].slot, 2);
  assert.ok(plan[1].distance_sq < plan[2].distance_sq);
  assert.ok(plan[2].distance_sq < plan[3].distance_sq);
});

test("NPC prompts include recent radio context and local memories", () => {
  const context = { ...SAMPLE_HAIL_CONTEXT, prompt_variant: "ledger" };
  const npc = buildConversationPlan(context, 4).find((speaker) => speaker.slot === 2);
  const prompt = buildSpeakerPrompt(context, npc, [
    { speaker: "YOU", text: "Prospect traffic, checking the belt." },
  ]);
  assert.match(prompt, /HAULER N02/);
  assert.match(prompt, /heard YOU: "Prospect traffic, checking the belt\."/);
  assert.match(prompt, /memory demand: haul FR around Kepler Yard/);
  assert.match(prompt, /contract haul 108 FR for Kepler Yard/);
});

test("terse NPC prompt is compact but still grounded in Signal facts", () => {
  const context = { ...SAMPLE_HAIL_CONTEXT, prompt_variant: "terse" };
  const npc = buildConversationPlan(context, 4).find((speaker) => speaker.slot === 2);
  const prompt = buildSpeakerPrompt(context, npc, [
    { speaker: "YOU", text: "Prospect traffic, checking the belt and looking for long-haul work." },
  ]);
  assert.match(prompt, /HAULER N02 outbound Kepler Yard>Helios Works/);
  assert.match(prompt, /knows FR demand Kepler Yard/);
  assert.match(prompt, /job 108 FR haul Kepler Yard/);
  assert.match(prompt, /heard YOU: Prospect traffic, checking the belt/);
  assert.ok(prompt.length < 210);
});

test("default prompt variant is grounded choice style", async () => {
  const records = await generateConversation(SAMPLE_HAIL_CONTEXT, {
    dryRun: true,
    maxSpeakers: 1,
  });
  assert.match(records[0].prompt, /^local signal\nYOU\n1 Open hail; local traffic check\./);
  assert.match(records[1].prompt, /^local signal/);
  assert.match(records[1].prompt, /1 FE pressure bright at Prospect Ref\./);
  assert.match(records[1].prompt, /2 Prospect Ref FE seam is talking\./);
  assert.match(records[1].prompt, /MINER N00:$/);
});

test("choice prompt gives SmolLM2 grounded candidate lines", () => {
  const context = { ...SAMPLE_HAIL_CONTEXT, prompt_variant: "choice" };
  const npc = buildConversationPlan(context, 4).find((speaker) => speaker.slot === 2);
  const prompt = buildSpeakerPrompt(context, npc, [
    { speaker: "YOU", text: "Open hail; local traffic check." },
  ]);
  assert.match(prompt, /^local signal/);
  assert.match(prompt, /YOU: Open hail; local traffic check\./);
  assert.match(prompt, /HAULER N02 outbound/);
  assert.match(prompt, /1 FR lane Kepler Yard>Helios Works lit\./);
  assert.match(prompt, /2 108 FR tagged Kepler Yard>Helios Works\./);
  assert.match(prompt, /3 FR board at Kepler Yard; Helios Works wants it\./);
  assert.match(prompt, /HAULER N02:$/);
  assert.ok(prompt.length < 230);
});

test("choice output resolves back to polished grounded candidates", () => {
  const context = { ...SAMPLE_HAIL_CONTEXT, prompt_variant: "choice" };
  const npc = buildConversationPlan(context, 4).find((speaker) => speaker.slot === 2);
  assert.equal(
    resolveChoiceLine("2 108 FR tagged Kepler Yard>Helios Works.", npc),
    "108 FR tagged Kepler Yard>Helios Works.",
  );
  assert.equal(
    resolveChoiceLine("FR board at Kepler Yard; Helios Works wants it.", npc),
    "FR board at Kepler Yard; Helios Works wants it.",
  );
  assert.equal(
    resolveChoiceLine("FR lane Kepler Yard>Helios Works lit", npc),
    "FR lane Kepler Yard>Helios Works lit.",
  );
  const miner = buildConversationPlan(SAMPLE_HAIL_CONTEXT, 3)[1];
  assert.equal(
    resolveChoiceLine("Prospect Ref FE pressure mark holding near.", miner),
    "FE pressure mark holding near Prospect Ref.",
  );
});

test("candidate lines cover supply, route risk, and scaffold memories", () => {
  const context = realHailContextFromNpcSnapshot({
    world: { tick: 12 },
    npcs: [{
      slot: 0,
      role: "hauler",
      state: "travel_to_destination",
      home_station_name: "Prospect Refinery",
      dest_station_name: "Helios Works",
      position: { x: 10, y: 20 },
      market_memories: [{
        kind_name: "route risk",
        commodity_code: "FR",
        station_a_name: "Helios Works",
        station_b_name: "Kepler Yard",
        quantity_hint: 4,
      }],
    }, {
      slot: 1,
      role: "hauler",
      state: "travel_to_destination",
      home_station_name: "Kepler Yard",
      dest_station_name: "Helios Works",
      position: { x: 20, y: 20 },
      market_memories: [{
        kind_name: "supply",
        commodity_code: "FR",
        station_a_name: "Kepler Yard",
        quantity_hint: 12,
      }],
    }, {
      slot: 2,
      role: "miner",
      state: "return_to_station",
      home_station_name: "Prospect Refinery",
      dest_station_name: "no destination",
      position: { x: 30, y: 20 },
      market_memories: [{
        kind_name: "scaffold pressure",
        module_name: "Frame Press",
        station_a_name: "Helios Works",
        station_b_name: "Kepler Yard",
        quantity_hint: 5,
      }],
    }],
  });
  const [, risk, supply, scaffold] = buildConversationPlan(context, 3);
  assert.deepEqual(candidateRadioLinesForSpeaker(risk), [
    "FR risk Kepler Yard>Helios Works.",
    "Kepler Yard>Helios Works reads rough.",
    "FR lane hazard near Helios Works.",
  ]);
  assert.deepEqual(candidateRadioLinesForSpeaker(supply), [
    "FR stack warm at Kepler Yard.",
    "Kepler Yard FR stock is moving.",
    "FR supply glows at Kepler Yard.",
  ]);
  assert.deepEqual(candidateRadioLinesForSpeaker(scaffold), [
    "Frame Press scaffold awake at Helios Works.",
    "Frame Press kit path Kepler Yard>Helios Works.",
    "Frame Press build signal holds at Helios Works.",
  ]);
});

test("batch choice prompt and parser handle compact SmolLM2 replies", () => {
  const plan = buildConversationPlan(SAMPLE_HAIL_CONTEXT, 2);
  const prompt = buildChoiceBatchPrompt(plan);
  assert.match(prompt, /^local hail choices/);
  assert.match(prompt, /YOU:\n1 Open hail; local traffic check\./);
  assert.match(prompt, /MINER N00:\n1 FE pressure bright at Prospect Ref\./);
  assert.match(prompt, /MINER N01:\n1 Prospect Ref FE seam is talking\./);
  assert.match(prompt, /Return all speaker keys\. No words\./);
  assert.match(prompt, /Example: YOU=1,N00=3,N01=3/);
  assert.match(prompt, /ANSWER:$/);

  const compact = parseChoiceBatchResponse("YOU=1,N00=3,N01=2", plan);
  assert.equal(compact.get("YOU"), 1);
  assert.equal(compact.get("N00"), 3);
  assert.equal(compact.get("N01"), 2);

  const roleNames = parseChoiceBatchResponse(
    "YOU=1, MINER N00=2, MINER N01=3", plan);
  assert.equal(roleNames.get("N00"), 2);
  assert.equal(roleNames.get("N01"), 3);

  const saltedPrompt = buildChoiceBatchPrompt(plan, 1);
  assert.match(saltedPrompt, /YOU:\n1 Local traffic, sound off\./);
  assert.match(saltedPrompt, /MINER N00:\n1 Prospect Ref FE seam is talking\./);
  assert.match(saltedPrompt, /Example: YOU=2,N00=3,N01=3/);

  const bare = parseChoiceBatchResponse("1,2,3", plan);
  assert.equal(bare.get("YOU"), 1);
  assert.equal(bare.get("N00"), 2);
  assert.equal(bare.get("N01"), 3);

  const invalid = parseChoiceBatchResponse("YOU=1,N00=2,N01=4", plan);
  assert.equal(invalid.get("N01"), undefined);
});

test("sample choice example resolves to compact distinct grounded lines", () => {
  const hailSalt = 1;
  const plan = buildConversationPlan(SAMPLE_HAIL_CONTEXT, 4);
  const prompt = buildChoiceBatchPrompt(plan, hailSalt);
  assert.ok(prompt.length <= 900);
  const example = prompt.match(/\bExample:\s*([^\n]+)/)?.[1] || "";
  const choices = new Map(example.split(",").map((part) => {
    const [key, value] = part.trim().split("=");
    return [key, Number(value)];
  }));
  const npcLines = [];
  for (const speaker of plan) {
    const key = choiceKeyForSpeaker(speaker);
    const candidates = rotatedChoiceCandidatesForSpeaker(speaker, hailSalt);
    const choice = choices.get(key);
    const line = candidates[choice - 1];
    assert.ok(line);
    if (speaker.kind === "npc") {
      assert.deepEqual(speakerRadioLineIssues(line, speaker), []);
      npcLines.push(line);
    }
  }
  assert.equal(new Set(npcLines).size, 4);
});

test("sample choice examples vary NPC lines across repeated hails", () => {
  const plan = buildConversationPlan(SAMPLE_HAIL_CONTEXT, 4);
  const transcripts = [];
  for (const hailSalt of [1, 2, 3]) {
    const prompt = buildChoiceBatchPrompt(plan, hailSalt);
    assert.ok(prompt.length <= 900);
    const example = prompt.match(/\bExample:\s*([^\n]+)/)?.[1] || "";
    const choices = new Map(example.split(",").map((part) => {
      const [key, value] = part.trim().split("=");
      return [key, Number(value)];
    }));
    const npcLines = [];
    for (const speaker of plan) {
      if (speaker.kind !== "npc") continue;
      const key = choiceKeyForSpeaker(speaker);
      const candidates = rotatedChoiceCandidatesForSpeaker(speaker, hailSalt);
      const line = candidates[choices.get(key) - 1];
      assert.ok(line);
      assert.deepEqual(speakerRadioLineIssues(line, speaker), []);
      npcLines.push(line);
    }
    assert.equal(new Set(npcLines).size, 4);
    transcripts.push(npcLines.join(" | "));
  }
  assert.equal(new Set(transcripts).size, 3);
});

test("transcript prompt drops the ship into local radio context", () => {
  const context = { ...SAMPLE_HAIL_CONTEXT, prompt_variant: "transcript" };
  const npc = buildConversationPlan(context, 4).find((speaker) => speaker.slot === 2);
  const prompt = buildSpeakerPrompt(context, npc, [
    { speaker: "YOU", text: "Open hail; local traffic check." },
  ]);
  assert.match(prompt, /^local signal/);
  assert.match(prompt, /YOU: Open hail; local traffic check\./);
  assert.match(prompt, /HAULER N02 outbound Kepler Yard>Helios Works/);
  assert.match(prompt, /FR demand Kepler Yard/);
  assert.match(prompt, /108 FR haul Kepler Yard/);
  assert.match(prompt, /HAULER N02:$/);
  assert.ok(prompt.length < 190);
});

test("chat prompt packing preserves transcript line breaks and speaker cue", () => {
  const context = { ...SAMPLE_HAIL_CONTEXT, prompt_variant: "transcript" };
  const npc = buildConversationPlan(context, 4).find((speaker) => speaker.slot === 2);
  const prompt = buildSpeakerPrompt(context, npc, [
    { speaker: "YOU", text: "Open hail; local traffic check." },
  ]);
  const content = chatUserContentFromPrompt(prompt);
  assert.match(content, /^local signal\n/);
  assert.match(content, /\nHAULER N02:$/);
  assert.ok(!content.includes("; HAULER N02"));
});

test("dry-run generation returns prompts for player and NPC speakers", async () => {
  const records = await generateConversation(SAMPLE_HAIL_CONTEXT, {
    dryRun: true,
    maxSpeakers: 2,
    firstDelay: 1.5,
    lineGap: 2.25,
  });
  assert.equal(records.length, 3);
  assert.equal(records[0].speaker, "YOU");
  assert.equal(records[1].speaker, "MINER N00");
  assert.equal(records[2].speaker, "MINER N01");
  assert.equal(records[0].at_s, 0);
  assert.equal(records[1].at_s, 1.5);
  assert.equal(records[2].at_s, 3.75);
  assert.equal(records[0].transmit_holograms, false);
  assert.equal(records[1].transmit_holograms, true);
  assert.equal(records[1].holograms[0].type, "market_memory");
  assert.match(records[0].prompt, /^local signal\nYOU\n1 Open hail/);
  assert.match(records[1].prompt, /MINER N00 returning/);
  assert.equal(records[1].prompt_chars, records[1].prompt.length);
  assert.equal(records[1].used_fallback, false);
  assert.equal(records.generation.mode, "dry_run");
  assert.equal(records.generation.llm_calls, 0);
});

test("runtime payload is keyed for the async client hail queue", async () => {
  const records = await generateConversation(SAMPLE_HAIL_CONTEXT, {
    dryRun: true,
    maxSpeakers: 2,
    firstDelay: 1.5,
    lineGap: 2.25,
  });
  records[0].text = "Open hail; local traffic check.";
  records[1].text = "FE pressure bright at Prospect Ref.";
  records[2].text = "FE pressure mark holding near Prospect Ref.";
  const payload = conversationRuntimePayload(SAMPLE_HAIL_CONTEXT, records);

  assert.equal(payload.player_line, "Open hail; local traffic check.");
  assert.equal(payload.generation.mode, "dry_run");
  assert.equal(payload.npc_lines.length, 2);
  assert.equal(payload.npc_lines[0].npc_index, 0);
  assert.equal(payload.npc_lines[0].speaker, "MINER N00");
  assert.equal(payload.npc_lines[0].at_s, 1.5);
  assert.equal(payload.npc_lines[0].line, "FE pressure bright at Prospect Ref.");
  assert.equal(payload.npc_lines[0].transmit_holograms, true);
  assert.deepEqual(payload.npc_lines[0].hologram_labels, [
    "ore pressure: FE @ Prospect Refinery",
    "demand: haul FE @ Prospect Refinery",
    "demand: haul RK @ Prospect Refinery",
    "demand: haul LM @ Prospect Refinery",
    "haul 1000 RK @ Prospect Refinery",
    "haul 8 LM @ Prospect Refinery",
    "delivery 4 FR @ Kepler Yard",
  ]);
  assert.deepEqual(payload.npc_lines[0].issues, []);
});

test("hail timing helper spaces async conversation turns", () => {
  assert.equal(scheduledAtForSpeakerIndex(0), 0);
  assert.equal(scheduledAtForSpeakerIndex(1), 1.2);
  assert.equal(scheduledAtForSpeakerIndex(3), 6);
  assert.equal(scheduledAtForSpeakerIndex(2, { firstDelay: 0.75, lineGap: 1.5 }), 2.25);
});

test("knowledge holograms expose compact memory and contract payloads", () => {
  const npc = buildConversationPlan(SAMPLE_HAIL_CONTEXT, 1)[1];
  const holograms = knowledgeHologramsForSpeaker(npc);
  assert.equal(holograms.length, 7);
  assert.equal(holograms[0].type, "market_memory");
  assert.equal(holograms[0].label, "ore pressure: FE @ Prospect Refinery");
  assert.equal(holograms[3].label, "demand: haul LM @ Prospect Refinery");
  assert.equal(holograms[4].type, "contract");
  assert.equal(holograms[4].label, "haul 1000 RK @ Prospect Refinery");
});

test("real NPC snapshot adapter preserves Signal memory fields", () => {
  const context = realHailContextFromNpcSnapshot({
    world: { tick: 12 },
    npcs: [{
      slot: 4,
      role: "hauler",
      state: "travel_to_destination",
      home_station_name: "Kepler Yard",
      dest_station_name: "Helios Works",
      position: { x: 10, y: 20 },
      hull: 150,
      cargo: [],
      market_memories: [{
        kind_name: "demand",
        action_name: "haul",
        commodity_code: "LM",
        station_a_name: "Kepler Yard",
        station_b_name: "Helios Works",
      }],
      known_contracts: [{
        action_name: "haul",
        quantity: 12,
        commodity_code: "LM",
        station_name: "Kepler Yard",
      }],
    }],
  }, { x: 1, y: 2 });
  const prompt = buildSpeakerPrompt(context, buildConversationPlan(context, 1)[1], []);
  assert.match(prompt, /knows LM demand Kepler Yard>Helios Works/);
  assert.match(prompt, /job 12 LM haul Kepler Yard/);
});

test("radio guardrail rejects assistant and offworld lines", () => {
  assert.deepEqual(radioLineIssues("Hauler N02, Kepler Yard to Helios Works."), []);
  assert.ok(radioLineIssues("As an AI assistant, I cannot hail.").includes("assistant"));
  assert.ok(radioLineIssues("Houston, this is Flight 12.").includes("offworld"));
  assert.ok(radioLineIssues("Miner N01 acknowledges.").includes("report"));
  assert.ok(radioLineIssues("All right, let's see what we've got here.").includes("generic"));
  assert.ok(radioLineIssues("Hauler N02 departing for").includes("truncated"));
  assert.ok(radioLineIssues("\"Prospect Refinery, this is").includes("truncated"));
  assert.ok(radioLineIssues("Signal Belt Hauler N03, from Kepler Yard to Hel").includes("truncated"));
  assert.ok(radioLineIssues("Hail received, requesting confirmation of traffic.").includes("report"));
  assert.ok(radioLineIssues("Hauler N03 confirms Kepler Yard FR demand, depart").includes("report"));
  assert.ok(radioLineIssues("Hauler N02, this is Helios Works.").includes("generic"));
  assert.ok(radioLineIssues("Miner, you're clear to proceed with the RK haul.").includes("generic"));
  assert.ok(radioLineIssues("All clear, no hazards detected.").includes("generic"));
  assert.ok(radioLineIssues("Hail Local Pilot.").includes("generic"));
  assert.ok(radioLineIssues("Prospect Ref: Hold empty, knows Fe ore pressure.").includes("copied_prompt"));
  assert.ok(radioLineIssues("Hauler N02 inbound Kepler Yard; holds full;").includes("generic"));
  assert.ok(radioLineIssues("Hauler N03 en route to Kepler Yard, awaiting").includes("truncated"));
  assert.ok(radioLineIssues("Hauler N03, Kepler Yard, one-time pickup").includes("copied_prompt"));
  assert.ok(radioLineIssues("Prospect Ref; job 1000 RK haul.").includes("copied_prompt"));
  assert.ok(radioLineIssues("Hail incoming on local frequency.").includes("generic"));
  assert.ok(radioLineIssues("Hail coming in on six...").includes("truncated"));
  assert.ok(radioLineIssues("Hail received.").includes("generic"));
  assert.ok(radioLineIssues("Return to Prospect Ref; know FE ore pressure; hauling R").includes("copied_prompt"));
  assert.ok(radioLineIssues("Return to Prospect Ref; know FE ore pressure; hauling R").includes("truncated"));
  assert.ok(radioLineIssues("Docking bay secured.").includes("generic"));
  assert.ok(radioLineIssues("Hauler n03 to Helios Works, cargo hold").includes("truncated"));
  assert.ok(radioLineIssues("Hauler N02 to Helios Works; cargo holds empty").includes("generic"));
  assert.ok(radioLineIssues("Fe ore in Prospect Ref; haul required for job").includes("copied_prompt"));
  assert.ok(radioLineIssues("Prospecting for Fe-bearing ores, ready to move when").includes("truncated"));
  assert.ok(radioLineIssues("Prospect miner / FE ore pressure: Ferrite dust inbound").includes("copied_prompt"));
  assert.ok(radioLineIssues("Prospect Refinery, this is Miner N01.").includes("generic"));
  assert.ok(radioLineIssues("Local signal received.").includes("generic"));
  assert.ok(radioLineIssues("Pressure reading normal, hauling to Helios Works in 3").includes("truncated"));
  assert.ok(radioLineIssues("Kepler Yard has FR demand; Helios").includes("truncated"));
  assert.ok(radioLineIssues("*Transmission crackles on comms device*").includes("truncated"));
  assert.ok(radioLineIssues("Received.").includes("generic"));
  assert.ok(radioLineIssues("Coming in at 3 o'clock and 5 miles").includes("offworld"));
});

test("fallback lines use authentic NPC memory facts", () => {
  const context = realHailContextFromNpcSnapshot({
    world: { tick: 12 },
    npcs: [{
      slot: 4,
      role: "hauler",
      state: "travel_to_destination",
      home_station_name: "Kepler Yard",
      dest_station_name: "Helios Works",
      position: { x: 10, y: 20 },
      hull: 150,
      market_memories: [{
        kind_name: "demand",
        action_name: "haul",
        commodity_code: "LM",
        station_a_name: "Kepler Yard",
        station_b_name: "Helios Works",
      }],
      known_contracts: [],
    }],
  });
  const npc = buildConversationPlan(context, 1)[1];
  assert.equal(fallbackRadioLine(npc), "LM demand logged; moving Helios Works-side.");
});

test("speaker guardrail rejects NPC lines without station or memory grounding", () => {
  const context = realHailContextFromNpcSnapshot({
    world: { tick: 12 },
    npcs: [{
      slot: 2,
      role: "hauler",
      state: "travel_to_destination",
      home_station_name: "Kepler Yard",
      dest_station_name: "Helios Works",
      position: { x: 10, y: 20 },
      hull: 150,
      market_memories: [{
        kind_name: "demand",
        action_name: "haul",
        commodity_code: "FR",
        station_a_name: "Kepler Yard",
      }],
      known_contracts: [],
    }],
  });
  const npc = buildConversationPlan(context, 1)[1];
  assert.ok(speakerRadioLineIssues("Hauling away.", npc).includes("generic"));
  assert.ok(speakerRadioLineIssues("Clear run tonight.", npc).includes("ungrounded"));
  assert.ok(speakerRadioLineIssues("Hauler N02 outbound Kepler Yard.", npc).includes("ungrounded"));
  assert.ok(speakerRadioLineIssues("N01, hauling FR to Helios Works.", npc).includes("wrong_speaker"));
  assert.ok(speakerRadioLineIssues("Helios Works-side.", npc).includes("ungrounded"));
  assert.deepEqual(speakerRadioLineIssues("Hauler N02 departing FR from Kepler Yard for Helios Works.", npc), []);
  assert.deepEqual(speakerRadioLineIssues("FR open at Kepler Yard.", npc), []);
});

test("fallback lines vary for separate ships with matching authentic memories", () => {
  const context = realHailContextFromNpcSnapshot({
    world: { tick: 12 },
    npcs: [2, 3].map((slot) => ({
      slot,
      role: "hauler",
      state: "travel_to_destination",
      home_station_name: "Kepler Yard",
      dest_station_name: "Helios Works",
      position: { x: slot * 10, y: 20 },
      hull: 150,
      market_memories: [{
        kind_name: "demand",
        action_name: "haul",
        commodity_code: "FR",
        station_a_name: "Kepler Yard",
        quantity_hint: 108,
      }],
      known_contracts: [{
        action_name: "haul",
        quantity: 108,
        commodity_code: "FR",
        station_name: "Kepler Yard",
      }],
    })),
  });
  const [, first, second] = buildConversationPlan(context, 2);
  assert.notEqual(fallbackRadioLine(first), fallbackRadioLine(second));
  assert.equal(fallbackRadioLine(first), "108 FR tagged Kepler Yard>Helios Works.");
  assert.equal(fallbackRadioLine(second), "FR board at Kepler Yard; Helios Works wants it.");
});
