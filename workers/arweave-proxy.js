export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const path = url.pathname.slice(1) || "index.html";

    try {
      const manifestTx = await env.SIGNAL_MANIFEST.get("tx");
      if (!manifestTx) return new Response("Not Configured", { status: 503 });

      const rawRes = await fetch(`https://arweave.net/raw/${manifestTx}`);
      if (!rawRes.ok) return new Response("Manifest offline", { status: 503 });
      const manifest = await rawRes.json();

      const entry = manifest.paths?.[path];
      const txId = entry?.id || manifest.paths?.["index.html"]?.id;
      if (!txId) return new Response("Not Found", { status: 404 });

      // Large files: redirect to Arweave directly
      const ext = path.split(".").pop().toLowerCase();
      if (ext === "wasm" || ext === "js") {
        return Response.redirect(`https://arweave.net/raw/${txId}`, 302);
      }

      const fileRes = await fetch(`https://arweave.net/raw/${txId}`);
      if (!fileRes.ok) return new Response("File offline", { status: 503 });

      const ct = { html: "text/html; charset=utf-8" }[ext] || "application/octet-stream";
      return new Response(fileRes.body, {
        headers: { "content-type": ct, "cache-control": "public, max-age=3600", "access-control-allow-origin": "*" },
      });
    } catch (e) {
      return new Response("Unavailable", { status: 503 });
    }
  },
};
