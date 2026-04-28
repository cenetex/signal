// tts.js — local Kokoro v1.0 TTS in the browser via kokoro-js.
//
// First call lazily downloads the ONNX weights (~325 MB int8). Cached after.
// API:
//   await TTS.init({ voice: "am_michael", device: "wasm" });
//   TTS.speak("hello captain.");          // queued, returns immediately
//   await TTS.flush();                    // resolves when all queued audio has played
//   TTS.cancel();                         // drop pending + stop current playback (barge-in)
//
// Voices: af_alloy, af_bella, af_nicole, am_adam, am_eric, am_fenrir, am_michael,
//         bm_lewis, bf_emma, etc. Same catalog as the native voicebox Kokoro v1.0.

// kokoro-js is lazy-imported inside init() so an unsupported-browser parse
// error in the bundle doesn't crash voicebox at module-evaluation time. The
// caller falls back to browser speechSynthesis if init() throws.
let KokoroTTS = null;
async function loadKokoro() {
    if (KokoroTTS) return KokoroTTS;
    // esm.sh inlines deps so the browser doesn't try to fetch jsdelivr-relative
    // `/npm/...` paths off the current origin.
    const m = await import("https://esm.sh/kokoro-js@latest");
    KokoroTTS = m.KokoroTTS;
    return KokoroTTS;
}

const MODEL = "onnx-community/Kokoro-82M-v1.0-ONNX";

class _TTS {
  constructor() {
    this.kokoro    = null;
    this.audioCtx  = null;
    this.voice     = "am_michael";
    this.queue     = [];          // pending {text, persona} jobs
    this.draining  = false;
    this.curSrc    = null;        // current AudioBufferSourceNode
    this.cancelled = false;
    this.onStatus  = null;
    this.onSpeakingChange = null; // (bool) — for mic gating / UI
    this._idleResolvers = [];
  }

  _status(s) { if (this.onStatus) this.onStatus(s); }
  _setSpeaking(b) { if (this.onSpeakingChange) this.onSpeakingChange(b); }

  async init({ voice = "am_michael", device = "wasm" } = {}) {
    if (this.kokoro) return;
    this.voice = voice;
    this._status(`loading kokoro (${device})…`);
    const t0 = performance.now();
    const Kokoro = await loadKokoro();
    this.kokoro = await Kokoro.from_pretrained(MODEL, {
      dtype: "q8",
      device,
    });
    this.audioCtx = new AudioContext();
    this._status(`kokoro ready in ${((performance.now()-t0)/1000).toFixed(1)} s`);
  }

  setVoice(v)   { this.voice = v; }
  isIdle()      { return !this.draining && this.queue.length === 0 && !this.curSrc; }

  speak(text, persona = null) {
    if (!text || !text.trim()) return;
    if (this.cancelled) this.cancelled = false;
    this.queue.push({ text: text.trim(), persona });
    if (!this.draining) this._drain();
  }

  cancel() {
    this.cancelled = true;
    this.queue = [];
    if (this.curSrc) { try { this.curSrc.stop(); } catch {} this.curSrc = null; }
    this._setSpeaking(false);
    this._resolveIdle();
  }

  flush() {
    if (this.isIdle()) return Promise.resolve();
    return new Promise(res => this._idleResolvers.push(res));
  }

  _resolveIdle() {
    const r = this._idleResolvers; this._idleResolvers = [];
    r.forEach(fn => fn());
  }

  async _drain() {
    if (!this.kokoro || !this.audioCtx) return;
    if (this.audioCtx.state === "suspended") await this.audioCtx.resume();
    this.draining = true;
    this._setSpeaking(true);
    while (this.queue.length && !this.cancelled) {
      const job = this.queue.shift();
      const voice = job.persona?.voice ?? this.voice;
      let audio;
      try {
        const t0 = performance.now();
        audio = await this.kokoro.generate(job.text, { voice });
        this._status(`tts ${(performance.now()-t0).toFixed(0)} ms`);
      } catch (err) {
        this._status(`tts error: ${err.message}`);
        continue;
      }
      if (this.cancelled) break;
      await this._play(audio);
    }
    this.draining = false;
    if (this.queue.length === 0) this._setSpeaking(false);
    this._resolveIdle();
  }

  _play({ audio, sampling_rate }) {
    return new Promise((resolve) => {
      const buf = this.audioCtx.createBuffer(1, audio.length, sampling_rate);
      buf.copyToChannel(audio, 0);
      const src = this.audioCtx.createBufferSource();
      src.buffer = buf;
      src.connect(this.audioCtx.destination);
      src.onended = () => { if (this.curSrc === src) this.curSrc = null; resolve(); };
      this.curSrc = src;
      src.start();
    });
  }
}

export const TTS = new _TTS();
