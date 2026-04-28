// auth.js — OpenRouter OAuth (PKCE S256) for browser apps.
//
// Flow:
//   1. generate code_verifier + code_challenge, redirect to /auth
//   2. on return, exchange ?code= for a long-lived user API key via /api/v1/auth/keys
//   3. cache key in localStorage; subsequent visits go straight to chat.

const KEY_STORAGE   = "voicebox.openrouter.key";
const VERIFIER_KEY  = "voicebox.openrouter.code_verifier";

function b64url(bytes) {
  let s = btoa(String.fromCharCode(...bytes));
  return s.replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

async function makePkcePair() {
  const buf = new Uint8Array(32);
  crypto.getRandomValues(buf);
  const verifier  = b64url(buf);
  const digest    = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(verifier));
  const challenge = b64url(new Uint8Array(digest));
  return { verifier, challenge };
}

export function getStoredKey() {
  return localStorage.getItem(KEY_STORAGE);
}

export function clearStoredKey() {
  localStorage.removeItem(KEY_STORAGE);
}

export async function startLogin() {
  const { verifier, challenge } = await makePkcePair();
  sessionStorage.setItem(VERIFIER_KEY, verifier);
  // callback_url must be an exact match to the URL OpenRouter will redirect to.
  // For dev we just use the page itself; the ?code=… arrives at the same path.
  const cb = location.origin + location.pathname;
  const url = new URL("https://openrouter.ai/auth");
  url.searchParams.set("callback_url",         cb);
  url.searchParams.set("code_challenge",       challenge);
  url.searchParams.set("code_challenge_method", "S256");
  location.href = url.toString();
}

// Run on page load: if the URL contains ?code=…, exchange it for a key,
// strip the param from the URL, and return the key.
export async function maybeCompleteLogin() {
  const params = new URLSearchParams(location.search);
  const code   = params.get("code");
  if (!code) return null;

  const verifier = sessionStorage.getItem(VERIFIER_KEY);
  sessionStorage.removeItem(VERIFIER_KEY);

  // Strip ?code= from URL so refreshes don't re-run the exchange.
  history.replaceState({}, "", location.pathname);

  const r = await fetch("https://openrouter.ai/api/v1/auth/keys", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      code,
      code_verifier: verifier,
      code_challenge_method: "S256",
    }),
  });
  if (!r.ok) {
    throw new Error(`OAuth exchange failed: ${r.status} ${await r.text()}`);
  }
  const { key } = await r.json();
  if (!key) throw new Error("OAuth exchange: no key in response");
  localStorage.setItem(KEY_STORAGE, key);
  return key;
}
