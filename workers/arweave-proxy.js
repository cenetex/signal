export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const path = url.pathname.slice(1) || "index.html";

    try {
      const manifestTx = await env.SIGNAL_MANIFEST.get("tx");
      if (!manifestTx) return new Response("Not Configured", { status: 503 });

      // Fetch manifest from Arweave
      const rawRes = await fetch(`https://arweave.net/raw/${manifestTx}`);
      if (!rawRes.ok) return new Response("Manifest not found", { status: 503 });
      const manifest = await rawRes.json();

      const entry = manifest.paths?.[path];
      const txId = entry?.id || manifest.paths?.["index.html"]?.id;
      if (!txId) return new Response("Not Found", { status: 404 });

      const fileRes = await fetch(`https://arweave.net/raw/${txId}`);
      if (!fileRes.ok) return new Response("File not found on Arweave yet", { status: 503 });

      const ext = path.split(".").pop().toLowerCase();
      const ct = { html: "text/html; charset=utf-8", js: "application/javascript", wasm: "application/wasm" }[ext] || "text/html; charset=utf-8";
      return new Response(fileRes.body, { headers: { "content-type": ct, "cache-control": "public, max-age=86400", "access-control-allow-origin": "*" } });
    } catch (e) {
      return new Response("Service Unavailable", { status: 503 });
    }
  },
};
