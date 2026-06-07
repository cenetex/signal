let manifestCache = null;
let cacheTime = 0;

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const path = url.pathname.slice(1) || "index.html";

    try {
      if (!manifestCache || Date.now() - cacheTime > 600_000) {
        const manifestTx = await env.SIGNAL_MANIFEST.get("tx");
        if (!manifestTx) return new Response("Not Configured", { status: 503 });
        const rawRes = await fetch(`https://arweave.net/raw/${manifestTx}`);
        if (!rawRes.ok) return new Response("Manifest offline", { status: 503 });
        manifestCache = await rawRes.json();
        cacheTime = Date.now();
      }

      const paths = manifestCache.paths || manifestCache;
      const entry = paths[path];
      // Handle both formats: {id: "tx"} or just "tx"
      const txId = typeof entry === "object" ? entry.id : entry;
      if (!txId) return new Response("Not Found", { status: 404 });

      const fileRes = await fetch(`https://arweave.net/raw/${txId}`);
      if (!fileRes.ok) return new Response("File offline", { status: 503 });

      const ext = path.split(".").pop().toLowerCase();
      const ct = { html: "text/html; charset=utf-8", js: "application/javascript", wasm: "application/wasm" }[ext] || "application/octet-stream";

      return new Response(fileRes.body, {
        headers: {
          "content-type": ct,
          "cache-control": ext === "wasm" ? "public, max-age=86400, immutable" : "public, max-age=3600",
          "access-control-allow-origin": "*",
        },
      });
    } catch (e) {
      return new Response("Unavailable", { status: 503 });
    }
  },
};
