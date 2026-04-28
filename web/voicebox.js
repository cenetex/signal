/*
 * voicebox.js — WASM browser voice integration for Signal.
 *
 * Delegates to ES-module helpers in web/voice/:
 *   auth.js      OAuth-PKCE flow against openrouter.ai
 *   stt.js       Whisper-tiny.en via transformers.js (AudioWorklet mic capture)
 *   tts.js       Kokoro v1.0 via kokoro-js (per-persona voice + speed)
 *   personas.js  static catalog of voice/system-prompt config
 *
 * The C side (src/voice_web.c, EM_JS) calls window.voicebox.{init,event,ask,
 * setState,setMicEnabled,quit}. Those are exposed below.
 *
 * Voice never leaves the device — STT + TTS are local WASM. Only the
 * transcribed text + a [SHIP TELEMETRY] blob round-trip to OpenRouter for
 * ASK elaborations, paid for by the player's own key.
 */

import { getStoredKey, clearStoredKey, startLogin, maybeCompleteLogin }
    from "./voice/auth.js";
import { STT } from "./voice/stt.js";
import { TTS } from "./voice/tts.js";
import { PERSONAS, DEFAULT_PERSONA } from "./voice/personas.js";

const state = {
    apiKey:     null,
    sttReady:   false,
    ttsReady:   false,
    micEnabled: false,
    selectedModel: "openai/gpt-oss-20b:free",
    shipState:  "",
};

function log(...args) { console.log("[voicebox]", ...args); }

// ---------- TTS pipeline ----------

async function ensureTTS() {
    if (state.ttsReady) return;
    log("loading Kokoro v1.0 (~325 MB; cached after first load)…");
    await TTS.init({ voice: PERSONAS[DEFAULT_PERSONA].voice, device: "wasm" });
    state.ttsReady = true;
}

function speakWithPersona(personaName, text) {
    if (!text || !text.trim()) return;
    const persona = PERSONAS[personaName] || PERSONAS[DEFAULT_PERSONA];
    if (!state.ttsReady) {
        ensureTTS().then(() => TTS.speak(text, persona))
                   .catch(err => log("TTS load failed:", err.message));
        return;
    }
    TTS.speak(text, persona);
}

// ---------- LLM (OpenRouter) ----------

async function askLLM(personaName, directive) {
    if (!state.apiKey) {
        log("ASK skipped — not connected to OpenRouter");
        return;
    }
    const persona = PERSONAS[personaName] || PERSONAS[DEFAULT_PERSONA];
    const messages = [
        { role: "system", content: persona.system },
        { role: "user",   content:
            `[SHIP TELEMETRY] ${state.shipState}\n` +
            `[STAGE DIRECTION — speak in your own voice. Paraphrase in 1 short ` +
            `sentence; do not quote this directive verbatim.] ${directive}` },
    ];

    let r;
    try {
        r = await fetch("https://openrouter.ai/api/v1/chat/completions", {
            method: "POST",
            headers: {
                "Content-Type":  "application/json",
                "Authorization": `Bearer ${state.apiKey}`,
                "HTTP-Referer":  location.origin,
                "X-Title":       "signal-web-voice",
            },
            body: JSON.stringify({
                model: state.selectedModel,
                stream: true,
                messages,
            }),
        });
    } catch (err) { log("ASK fetch error:", err.message); return; }
    if (!r.ok) { log(`ASK HTTP ${r.status}`); return; }

    // Stream SSE; emit each complete sentence to TTS as it arrives.
    const reader   = r.body.getReader();
    const dec      = new TextDecoder();
    let leftover   = "";
    let sentBuf    = "";
    let inThink    = false;

    function drainSentences(force = false) {
        while (true) {
            // skip <think>…</think> qwen reasoning blocks
            if (!inThink && sentBuf.includes("<think>") &&
                            !sentBuf.includes("</think>")) {
                sentBuf = sentBuf.split("<think>")[0];
                inThink = true;
            }
            if (inThink) {
                const close = sentBuf.indexOf("</think>");
                if (close < 0) return;
                sentBuf = sentBuf.slice(close + "</think>".length);
                inThink = false;
            }
            const m = sentBuf.match(force ? /^.+/s : /^.*?[.!?\n]/s);
            if (!m) return;
            const candidate = m[0];
            const remainder = sentBuf.slice(candidate.length);
            const letters = (candidate.match(/[a-zA-Z]/g) || []).length;
            if (!force && letters < 4) return;
            const sentence = candidate.trim();
            if (sentence) speakWithPersona(personaName, sentence);
            sentBuf = remainder;
            if (!force) continue;
            return;
        }
    }

    while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        leftover += dec.decode(value, { stream: true });
        let idx;
        while ((idx = leftover.indexOf("\n\n")) >= 0) {
            const event = leftover.slice(0, idx);
            leftover    = leftover.slice(idx + 2);
            for (const line of event.split("\n")) {
                if (!line.startsWith("data: ")) continue;
                const payload = line.slice(6);
                if (payload === "[DONE]") continue;
                try {
                    const j = JSON.parse(payload);
                    const delta = j.choices?.[0]?.delta?.content ?? "";
                    if (delta) { sentBuf += delta; drainSentences(false); }
                } catch { /* keepalives */ }
            }
        }
    }
    drainSentences(true);
}

// ---------- STT pipeline (push-to-talk) ----------

STT.onTranscript = (text) => {
    if (!text || !state.apiKey) return;
    log("captain said:", text);
    // Pilot speech routes through nav7 as a free-form ASK with state.
    askLLM(DEFAULT_PERSONA,
        `The captain just said: "${text}". Respond as NAV-7 directly to them.`);
};

async function ensureSTT() {
    if (state.sttReady) return;
    log("loading Whisper tiny.en (~75 MB; cached after first load)…");
    await STT.init();
    await STT.startCapture();
    state.sttReady = true;
}

// ---------- public API surface called from voice_web.c ----------

window.voicebox = {
    /* called from voice_init() at signal startup */
    init() {
        log("init");
        // Try to complete an in-flight OAuth callback first; otherwise restore
        // a cached key. Mic + TTS load lazily on first use.
        maybeCompleteLogin().then(newKey => {
            if (newKey) { state.apiKey = newKey; log("OAuth complete"); return; }
            const cached = getStoredKey();
            if (cached) { state.apiKey = cached; log("using cached OpenRouter key"); }
            else        { log("no OpenRouter key — ASK disabled until /voice/connect is run"); }
        }).catch(err => log("auth error:", err.message));

        // Pre-warm Kokoro in the background once auth is ready, so the first
        // station hail doesn't wait on a 30-second model download.
        setTimeout(() => {
            if (state.apiKey || true) ensureTTS().catch(() => {});
        }, 500);
    },

    /* deterministic chatter — speak verbatim */
    event(persona, line) {
        log(`event ${persona}: ${line}`);
        speakWithPersona(persona, line);
    },

    /* LLM-mediated, state-aware paraphrase */
    ask(persona, directive) {
        log(`ask ${persona}: ${directive}`);
        askLLM(persona, directive).catch(err => log("ASK error:", err.message));
    },

    /* C side calls this when ship state changes (≤1 Hz) */
    setState(fields) {
        state.shipState = fields;
    },

    /* push-to-talk gate */
    async setMicEnabled(enabled) {
        if (enabled === state.micEnabled) return;
        state.micEnabled = enabled;
        if (enabled) {
            try { await ensureSTT(); STT.holdToTalk(true); }
            catch (err) { log("mic error:", err.message); state.micEnabled = false; }
        } else if (state.sttReady) {
            STT.holdToTalk(false);
        }
    },

    /* shutdown */
    quit() {
        log("quit");
        if (state.ttsReady) TTS.cancel();
    },

    // --- player-facing UX (called from a settings UI in the host page) ---

    /* kicks off the OAuth-PKCE redirect to openrouter.ai */
    connect: startLogin,

    /* clears cached key + signs out */
    disconnect() {
        clearStoredKey();
        state.apiKey = null;
        log("disconnected");
    },

    /* model picker */
    setModel(modelId) { if (modelId) state.selectedModel = modelId; },
    getModel() { return state.selectedModel; },

    /* introspection */
    isConnected() { return !!state.apiKey; },
    isTTSReady()  { return state.ttsReady; },
    isSTTReady()  { return state.sttReady; },
};
