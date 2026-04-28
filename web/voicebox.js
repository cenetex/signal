/*
 * voicebox.js -- WASM browser voice integration
 * Provides a JavaScript bridge for Signal's voice system.
 * Handles STT (Whisper), TTS (Kokoro), and LLM elaboration (OpenRouter).
 */

window.voicebox = (function() {
  const state = {
    initialized: false,
    micEnabled: false,
    openRouterKey: null,
    selectedModel: 'openrouter/auto', // Default or user-selected model
    personas: {},
    shipState: {},
  };

  async function init() {
    console.log('[voicebox] Initializing...');
    state.initialized = true;
    // Load persisted OpenRouter key if available
    const savedKey = localStorage.getItem('voicebox_openrouter_key');
    if (savedKey) {
      state.openRouterKey = savedKey;
      console.log('[voicebox] Loaded persisted OpenRouter key');
    }
    // Load persisted model selection
    const savedModel = localStorage.getItem('voicebox_selected_model');
    if (savedModel) {
      state.selectedModel = savedModel;
      console.log(`[voicebox] Loaded persisted model: ${savedModel}`);
    }
  }

  function event(persona, line) {
    console.log(`[voicebox] event: ${persona} says "${line}"`);
    // Line will be spoken via TTS (Kokoro)
    if (state.micEnabled && state.openRouterKey) {
      // Process elaboration if needed, then play TTS
      playTTS(persona, line);
    } else {
      playTTS(persona, line);
    }
  }

  function setState(fields) {
    console.log(`[voicebox] setState: ${fields}`);
    // Parse semicolon-separated fields: key1=value1;key2=value2
    const pairs = fields.split(';');
    pairs.forEach(pair => {
      const [key, value] = pair.split('=');
      if (key && value) {
        state.shipState[key.trim()] = value.trim();
      }
    });
  }

  async function ask(persona, directive) {
    console.log(`[voicebox] ask: ${persona} elaborates on "${directive}"`);
    if (!state.openRouterKey) {
      console.log('[voicebox] No OpenRouter key available; skipping elaboration');
      return;
    }
    // Query LLM for elaboration
    try {
      const contextStr = Object.entries(state.shipState)
        .map(([k, v]) => `${k}: ${v}`)
        .join(', ');
      const prompt = `As ${persona}, briefly elaborate on: ${directive}. Context: ${contextStr}`;
      const response = await queryLLM(prompt);
      if (response) {
        playTTS(persona, response);
      }
    } catch (err) {
      console.error(`[voicebox] LLM elaboration failed: ${err}`);
    }
  }

  async function queryLLM(prompt) {
    // Call OpenRouter API with user's key
    try {
      const response = await fetch('https://openrouter.io/api/v1/chat/completions', {
        method: 'POST',
        headers: {
          'Authorization': `Bearer ${state.openRouterKey}`,
          'Content-Type': 'application/json',
        },
        body: JSON.stringify({
          model: state.selectedModel,
          messages: [{
            role: 'user',
            content: prompt,
          }],
          max_tokens: 50,
        }),
      });
      if (!response.ok) {
        const err = await response.text();
        throw new Error(`OpenRouter request failed: ${err}`);
      }
      const data = await response.json();
      return data.choices?.[0]?.message?.content || null;
    } catch (err) {
      console.error(`[voicebox] LLM query error: ${err}`);
      return null;
    }
  }

  function playTTS(persona, text) {
    console.log(`[voicebox] playTTS: "${text}"`);
    // Placeholder: in a real implementation, use Kokoro WASM
    // For now, use browser's Web Speech API as fallback
    if ('speechSynthesis' in window) {
      const utterance = new SpeechSynthesisUtterance(text);
      utterance.rate = 0.9;
      speechSynthesis.speak(utterance);
    }
  }

  function setMicEnabled(enabled) {
    console.log(`[voicebox] setMicEnabled: ${enabled}`);
    state.micEnabled = enabled;
    // In a real implementation, initialize STT capture here
  }

  function quit() {
    console.log('[voicebox] Quitting...');
    state.initialized = false;
  }

  async function getAvailableModels() {
    // Fetch available models from OpenRouter
    // Free models are sufficient for the game
    if (!state.openRouterKey) {
      console.log('[voicebox] No OpenRouter key; returning default model');
      return [{ id: 'openrouter/auto', name: 'Auto (Free)' }];
    }
    try {
      const response = await fetch('https://openrouter.io/api/v1/models', {
        headers: { 'Authorization': `Bearer ${state.openRouterKey}` }
      });
      if (!response.ok) throw new Error(`Failed to fetch models: ${response.status}`);
      const data = await response.json();
      return (data.data || []).map(m => ({
        id: m.id,
        name: `${m.name || m.id}${m.pricing?.prompt ? ' (paid)' : ' (free)'}`
      }));
    } catch (err) {
      console.error(`[voicebox] Failed to fetch available models: ${err}`);
      return [{ id: 'openrouter/auto', name: 'Auto (Free)' }];
    }
  }

  function setModel(modelId) {
    if (!modelId) return;
    state.selectedModel = modelId;
    localStorage.setItem('voicebox_selected_model', modelId);
    console.log(`[voicebox] Model set to ${modelId}`);
  }

  // Public API
  return {
    init,
    event,
    setState,
    ask,
    setMicEnabled,
    quit,
    setOpenRouterKey: function(key) {
      state.openRouterKey = key;
      localStorage.setItem('voicebox_openrouter_key', key);
      console.log('[voicebox] OpenRouter key set');
    },
    setModel,
    getAvailableModels,
    getState: function() {
      return { ...state };
    },
  };
})();
