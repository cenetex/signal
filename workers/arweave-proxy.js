// Cloudflare Worker — serves Signal from Arweave at signal.ratimics.com
// Manifest TX ID is stored in KV (SIGNAL_MANIFEST) and updated by CI on each deploy.

const GATEWAY = 'https://arweave.net';

const CONTENT_TYPES = {
  html: 'text/html; charset=utf-8',
  js: 'application/javascript; charset=utf-8',
  wasm: 'application/wasm',
  css: 'text/css; charset=utf-8',
  svg: 'image/svg+xml',
  png: 'image/png',
  jpg: 'image/jpeg',
  json: 'application/json',
};

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const path = url.pathname.slice(1) || 'index.html';

    try {
      const manifestTx = await env.SIGNAL_MANIFEST.get('tx');
      if (!manifestTx) return new Response('Not Configured', { status: 503 });

      // Fetch the manifest (cached in KV for 10 minutes)
      let manifestJson = await env.SIGNAL_MANIFEST.get('manifest', 'json');
      if (!manifestJson) {
        const rawRes = await fetch(`${GATEWAY}/raw/${manifestTx}`);
        if (!rawRes.ok) throw new Error(`Manifest fetch failed: ${rawRes.status}`);
        manifestJson = await rawRes.json();
        await env.SIGNAL_MANIFEST.put('manifest', JSON.stringify(manifestJson), { expirationTtl: 600 });
      }

      const txId = manifestJson.paths[path];
      if (!txId) {
        const fallback = manifestJson.paths['index.html'];
        if (!fallback) return new Response('Not Found', { status: 404 });
        const fbRes = await fetch(`${GATEWAY}/raw/${fallback}`);
        return new Response(fbRes.body, {
          headers: { 'content-type': 'text/html; charset=utf-8', 'cache-control': 'public, max-age=60' },
        });
      }

      const ext = path.split('.').pop().toLowerCase();
      const ct = CONTENT_TYPES[ext] || 'application/octet-stream';

      const arRes = await fetch(`${GATEWAY}/raw/${txId}`);
      if (!arRes.ok) return new Response('Not Found', { status: 404 });

      return new Response(arRes.body, {
        headers: {
          'content-type': ct,
          'cache-control': 'public, max-age=86400',
          'access-control-allow-origin': '*',
        },
      });
    } catch (e) {
      return new Response('Service Unavailable', { status: 503 });
    }
  },
};
