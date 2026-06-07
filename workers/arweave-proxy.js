// This worker only handles HTML routing. JS/WASM assets are served
// by a separate mechanism (direct Arweave URLs embedded in the HTML).
export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const path = url.pathname.slice(1) || "index.html";
    
    // Only handle HTML files
    if (!path.endsWith(".html") && path !== "" && !path.includes(".")) {
      // Non-HTML requests fall through to the default origin
      return fetch(request);
    }
    if (path.endsWith(".js") || path.endsWith(".wasm")) {
      return fetch(request);
    }

    try {
      const manifestTx = await env.SIGNAL_MANIFEST.get("tx");
      if (!manifestTx) return new Response("Not Configured", { status: 503 });

      const rawRes = await fetch(`https://arweave.net/raw/${manifestTx}`);
      if (!rawRes.ok) return new Response("Manifest offline", { status: 503 });
      const manifest = await rawRes.json();

      const entry = manifest.paths?.[path];
      const txId = entry?.id || manifest.paths?.["index.html"]?.id;
      if (!txId) return new Response("Not Found", { status: 404 });

      const fileRes = await fetch(`https://arweave.net/raw/${txId}`);
      if (!fileRes.ok) return new Response("File offline", { status: 503 });

      return new Response(fileRes.body, {
        headers: {
          "content-type": "text/html; charset=utf-8",
          "cache-control": "public, max-age=3600",
          "access-control-allow-origin": "*",
        },
      });
    } catch (e) {
      return new Response("Unavailable", { status: 503 });
    }
  },
};
