// stt.js — local Whisper STT in the browser via transformers.js.
//
// Loads on first use (~75 MB whisper-tiny.en cached by the browser after).
// Records mic audio with AudioWorklet, decodes on speech-end (button release
// for now; VAD-driven endpointing will come in Stage 1.5).
//
// API:
//   const stt = await STT.init({ model: "Xenova/whisper-tiny.en" });
//   stt.onTranscript = (text) => { ... };
//   stt.startCapture();   // open mic
//   stt.holdToTalk(true); // begin recording into the active buffer
//   stt.holdToTalk(false);// stop recording, transcribe, fire onTranscript

// transformers.js is lazy-imported inside init() so a CDN parse failure in
// an older browser doesn't crash voicebox at module-evaluation time.
let _transformers = null;
async function loadTransformers() {
    if (_transformers) return _transformers;
    const m = await import("https://esm.sh/@xenova/transformers@2.17.2");
    m.env.allowLocalModels = false;
    m.env.useBrowserCache  = true;
    _transformers = m;
    return m;
}

const TARGET_SR = 16000; // whisper expects 16k mono float32

class _STT {
  constructor() {
    this.transcriber = null;
    this.audioCtx    = null;
    this.workletNode = null;
    this.micStream   = null;
    this.recording   = false;
    this.buf         = []; // accumulated Float32Arrays at AudioContext.sampleRate
    this.onTranscript = null;
    this.onStatus     = null;
  }

  _status(s) { if (this.onStatus) this.onStatus(s); }

  async init({ model = "Xenova/whisper-tiny.en" } = {}) {
    this._status("loading whisper model…");
    const t = await loadTransformers();
    this.transcriber = await t.pipeline("automatic-speech-recognition", model);
    this._status("whisper ready");
  }

  async startCapture() {
    if (this.audioCtx) return; // already open
    this.micStream = await navigator.mediaDevices.getUserMedia({
      audio: { channelCount: 1, echoCancellation: true, noiseSuppression: true },
    });
    this.audioCtx  = new AudioContext({ sampleRate: TARGET_SR });
    // try setting target rate; browsers usually honor it but some don't —
    // we resample manually below as a backstop.
    const src      = this.audioCtx.createMediaStreamSource(this.micStream);

    // Inline AudioWorkletProcessor as a Blob URL so we don't need a separate file.
    const procSrc = `
      class CaptureProc extends AudioWorkletProcessor {
        process(inputs) {
          const ch = inputs[0][0];
          if (ch && ch.length) this.port.postMessage(ch.slice());
          return true;
        }
      }
      registerProcessor("capture-proc", CaptureProc);
    `;
    const url = URL.createObjectURL(new Blob([procSrc], { type: "application/javascript" }));
    await this.audioCtx.audioWorklet.addModule(url);
    this.workletNode = new AudioWorkletNode(this.audioCtx, "capture-proc");
    this.workletNode.port.onmessage = (ev) => {
      if (this.recording) this.buf.push(ev.data);
    };
    src.connect(this.workletNode);
    // Don't connect to destination — we don't want to monitor the mic to speakers.
    this._status("mic open");
  }

  holdToTalk(on) {
    if (!this.audioCtx || !this.transcriber) return;
    if (on) {
      this.buf = [];
      this.recording = true;
      this._status("recording");
    } else if (this.recording) {
      this.recording = false;
      const merged = this._mergeBuffers(this.buf);
      this.buf = [];
      this._status(`captured ${merged.length} samples (${(merged.length/this.audioCtx.sampleRate).toFixed(2)} s)`);
      // resample to 16k if AudioContext gave us something else
      const audio = this._resample(merged, this.audioCtx.sampleRate, TARGET_SR);
      if (audio.length < TARGET_SR * 0.2) {
        this._status("clip too short — ignored");
        return;
      }
      this._transcribe(audio);
    }
  }

  async _transcribe(audio) {
    this._status("transcribing…");
    const t0 = performance.now();
    const out = await this.transcriber(audio, {
      chunk_length_s: 30,
      stride_length_s: 5,
      language: "en",
    });
    const dt = (performance.now() - t0).toFixed(0);
    this._status(`stt ${dt} ms`);
    if (this.onTranscript) this.onTranscript(out.text.trim());
  }

  _mergeBuffers(chunks) {
    let total = 0; for (const c of chunks) total += c.length;
    const out = new Float32Array(total);
    let off = 0;
    for (const c of chunks) { out.set(c, off); off += c.length; }
    return out;
  }

  _resample(input, fromSr, toSr) {
    if (Math.abs(fromSr - toSr) < 1) return input;
    const ratio = fromSr / toSr;
    const outLen = Math.floor(input.length / ratio);
    const out = new Float32Array(outLen);
    for (let i = 0; i < outLen; i++) {
      const src = i * ratio;
      const i0  = Math.floor(src);
      const i1  = Math.min(i0 + 1, input.length - 1);
      const f   = src - i0;
      out[i] = input[i0] * (1 - f) + input[i1] * f;
    }
    return out;
  }
}

export const STT = new _STT();
