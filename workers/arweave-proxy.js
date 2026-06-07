export default {
  async fetch(request) {
    const url = new URL(request.url);
    const path = url.pathname.slice(1) || "index.html";
    
    // Irys tx IDs for Signal files
    const files = {
      "index.html":  "3LZEte9Zjnom32AierB6oE9S5ybaKqCZMwmTPwzFqW2B",
      "mine.html":   "4r5E9C3VvfXERrv9KAy83vwWiyiY3483vQ5iAhvPMqUM",
      "play.html":   "C9RGdaEHUNRx5exRewGsXiZoaHCFBLxiw1iFw9hET58h",
      "signal.html": "5JgXGeswjeXb14fnYLnSqMM3Qv7g64eQ28YWh4327q8S",
    };
    const txId = files[path] || files["index.html"];
    if (!txId) return new Response("Not Found", { status: 404 });

    // Try Arweave raw first, then Irys gateway
    const urls = [
      `https://arweave.net/raw/${txId}`,
      `https://gateway.irys.dev/${txId}`,
    ];

    for (const u of urls) {
      try {
        const res = await fetch(u);
        if (res.ok && res.status === 200) {
          const ct = path.endsWith(".html") ? "text/html; charset=utf-8" : 
                     path.endsWith(".js") ? "application/javascript" : "application/octet-stream";
          return new Response(res.body, {
            headers: { "content-type": ct, "cache-control": "public, max-age=3600", "access-control-allow-origin": "*" },
          });
        }
      } catch {}
    }
    return new Response("Content not yet available on gateways. Try again soon.", { status: 503 });
  },
};
