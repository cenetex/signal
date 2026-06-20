export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const path = url.pathname.slice(1) || "index.html";

    try {
      // Try KV-stored paths map first (fastest, no network hop)
      let pathsRaw = await env.SIGNAL_MANIFEST.get("manifest");
      if (!pathsRaw) {
        // Fallback: fetch manifest TX from Arweave
        const manifestTx = await env.SIGNAL_MANIFEST.get("tx");
        if (!manifestTx) return new Response("Not Configured", { status: 503 });
        // Try multiple gateways
        for (const gw of ["https://arweave.net/raw/", "https://gateway.irys.xyz/", "https://node2.irys.xyz/"]) {
          const res = await fetch(gw + manifestTx);
          if (res.ok) { pathsRaw = await res.text(); break; }
        }
        if (!pathsRaw) return new Response("Manifest offline", { status: 503 });
      }

      const paths = JSON.parse(pathsRaw);
      let txId = paths[path];
      if (!txId && !path.includes(".")) txId = paths[path + ".html"];
      if (!txId) return new Response("Not Found", { status: 404 });

      // Irys uploads can be visible on Irys gateways before arweave.net/raw
      // catches up. Try all known gateways before reporting the file offline.
      let fileRes = null;
      for (const gw of ["https://arweave.net/raw/", "https://gateway.irys.xyz/", "https://node2.irys.xyz/"]) {
        const res = await fetch(gw + txId);
        if (res.ok) { fileRes = res; break; }
      }
      if (!fileRes) return new Response("File offline", { status: 503 });

      let effectiveExt = path.split(".").pop().toLowerCase();
      if (!path.includes(".") && paths[path + ".html"]) effectiveExt = "html";
      const ct = { html: "text/html; charset=utf-8", js: "application/javascript", wasm: "application/wasm", mp3: "audio/mpeg" }[effectiveExt] || "application/octet-stream";

      return new Response(fileRes.body, {
        headers: {
          "content-type": ct,
          "cache-control": effectiveExt === "wasm" ? "public, max-age=86400, immutable" : "public, max-age=3600",
          "access-control-allow-origin": "*",
        },
      });
    } catch (e) {
      return new Response("Unavailable", { status: 503 });
    }
  },
};
