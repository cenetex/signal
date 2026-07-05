const ORIGIN = "https://signal-relay-kind-pond-4338.fly.dev";
const ORIGIN_HOST = new URL(ORIGIN).host;

export default {
  async fetch(request) {
    const publicUrl = new URL(request.url);
    const target = new URL(publicUrl.pathname + publicUrl.search, ORIGIN);

    if (target.pathname === "/") {
      target.pathname = "/play";
    }

    const proxyRequest = new Request(target.toString(), request);
    proxyRequest.headers.set("Host", ORIGIN_HOST);
    proxyRequest.headers.set("X-Forwarded-Host", publicUrl.host);
    proxyRequest.headers.set("X-Forwarded-Proto", publicUrl.protocol.replace(":", ""));

    const response = await fetch(proxyRequest);
    const location = response.headers.get("Location");
    if (!location) return response;

    const rewrittenLocation = rewriteOriginLocation(location, publicUrl);
    if (!rewrittenLocation) return response;

    const headers = new Headers(response.headers);
    headers.set("Location", rewrittenLocation);

    return new Response(response.body, {
      status: response.status,
      statusText: response.statusText,
      headers,
    });
  },
};

function rewriteOriginLocation(location, publicUrl) {
  const target = new URL(location, ORIGIN);
  if (target.host !== ORIGIN_HOST) return null;

  target.protocol = publicUrl.protocol;
  target.host = publicUrl.host;
  return target.toString();
}
