function sanitizeChoiceText(text) {
  return String(text || "")
    .replace(/```[\s\S]*?```/g, "")
    .split(/\r?\n/)[0]
    .replace(/[^A-Za-z0-9=, _-]/g, "")
    .trim();
}

function normalizeRadioText(text) {
  return String(text || "")
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, " ")
    .replace(/\s+/g, " ")
    .trim();
}

function promptSpeakers(prompt) {
  const speakers = [];
  let current = null;
  String(prompt || "").split(/\r?\n/).forEach((line) => {
    const speaker = line.match(/^(YOU|(?:MINER|HAULER|WORKER)\s+N\d{2}):$/);
    if (speaker) {
      const id = speaker[1] === "YOU"
        ? "YOU"
        : speaker[1].match(/N\d{2}/)[0];
      current = { id, choices: [] };
      speakers.push(current);
      return;
    }
    const choice = line.match(/^([1-3])\s+(.+)$/);
    if (current && choice) {
      current.choices.push({
        number: Number(choice[1]),
        text: choice[2]
      });
    }
  });
  return speakers;
}

export function choiceGrammarForPrompt(prompt) {
  const speakers = promptSpeakers(prompt);
  if (speakers.length === 0) return "";
  const body = speakers.map((speaker, index) => {
    const prefix = index === 0 ? "" : "\",\" ";
    return `${prefix}"${speaker.id}=" choice`;
  }).join(" ");
  return [
    `root ::= ${body}`,
    `choice ::= "1" | "2" | "3"`,
  ].join("\n");
}

export function normalizeChoiceResponse(prompt, text) {
  const clean = sanitizeChoiceText(text);
  if (!clean) return "";

  const speakers = promptSpeakers(prompt);

  const assignments = [];
  const allowed = new Set(speakers.map((speaker) => speaker.id));
  const assignPattern = /\b(YOU|N\d{2})\s*=\s*([1-3])\b/gi;
  let match;
  while ((match = assignPattern.exec(clean)) !== null) {
    const id = match[1].toUpperCase();
    if (allowed.has(id)) assignments.push(`${id}=${match[2]}`);
  }
  if (assignments.length > 0) return assignments.join(",");

  const bareNumbers = [];
  const barePattern = /(^|[\s,;])([1-3])(?=$|[\s,;])/g;
  while ((match = barePattern.exec(clean)) !== null &&
         bareNumbers.length < speakers.length) {
    bareNumbers.push(match[2]);
  }
  if (bareNumbers.length > 0) {
    return bareNumbers.map((number, index) =>
      `${speakers[index].id}=${number}`).join(",");
  }

  const leading = clean.match(/^([1-3])(?:\s|$)/);
  if (leading && speakers.length === 1)
    return `${speakers[0].id}=${leading[1]}`;

  const cleanNorm = normalizeRadioText(clean);
  for (const speaker of speakers) {
    for (const choice of speaker.choices) {
      const choiceNorm = normalizeRadioText(choice.text);
      if (!choiceNorm) continue;
      if (cleanNorm === choiceNorm ||
          cleanNorm.includes(choiceNorm) ||
          choiceNorm.includes(cleanNorm)) {
        assignments.push(`${speaker.id}=${choice.number}`);
        break;
      }
    }
  }
  if (assignments.length > 0) return assignments.join(",");

  return clean;
}

function absoluteUrl(url) {
  return new URL(String(url), import.meta.url).href;
}

export const SignalHailGgufRuntime = {
  async create(config) {
    const modelUrl = config && config.modelUrl ? String(config.modelUrl) : "";
    if (!modelUrl) throw new Error("hailModel URL is required for GGUF hail runtime");

    const wasmUrl = config && config.wasmUrl
      ? String(config.wasmUrl)
      : "./wllama/esm/wasm/wllama.wasm";
    const { LoggerWithoutDebug, Wllama } =
      await import("./wllama/esm/index.js");
    const wllama = new Wllama({ default: absoluteUrl(wasmUrl) }, {
      allowOffline: true,
      logger: LoggerWithoutDebug,
      suppressNativeLog: true
    });

    const loadParams = {
      n_ctx: 1024,
      n_batch: 128,
      n_gpu_layers: Number.isFinite(config && config.nGpuLayers)
        ? config.nGpuLayers
        : 0
    };
    if (Number.isFinite(config && config.nThreads))
      loadParams.n_threads = config.nThreads;

    await wllama.loadModelFromUrl(absoluteUrl(modelUrl), loadParams);

    return {
      async generateChoice(request) {
        const prompt = String(request && request.prompt ? request.prompt : "");
        const response = await wllama.createChatCompletion({
          messages: [{
            role: "user",
            content: prompt
          }],
          max_tokens: Number(request && request.maxTokens) || 24,
          temperature: Number(request && request.temperature) || 0.25,
          top_k: 20,
          top_p: 0.9,
          grammar: choiceGrammarForPrompt(prompt) || undefined
        });
        const choice = response &&
          response.choices &&
          response.choices[0] &&
          response.choices[0].message &&
          response.choices[0].message.content;
        return normalizeChoiceResponse(prompt, choice);
      }
    };
  }
};
