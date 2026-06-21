const FLY_ORIGIN = "https://signal-relay-kind-pond-4338.fly.dev";
const FLY_HOST = new URL(FLY_ORIGIN).host;

export default {
  async fetch(request) {
    const publicUrl = new URL(request.url);
    const target = new URL(publicUrl.pathname + publicUrl.search, FLY_ORIGIN);

    if (target.pathname === "/") {
      target.pathname = "/play";
    }

    const proxyRequest = new Request(target.toString(), request);
    proxyRequest.headers.set("Host", FLY_HOST);
    proxyRequest.headers.set("X-Forwarded-Host", publicUrl.host);
    proxyRequest.headers.set("X-Forwarded-Proto", publicUrl.protocol.replace(":", ""));

    const response = await fetch(proxyRequest);
    const location = response.headers.get("Location");
    if (!location) return response;

    const rewrittenLocation = rewriteFlyLocation(location, publicUrl);
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

function rewriteFlyLocation(location, publicUrl) {
  const target = new URL(location, FLY_ORIGIN);
  if (target.host !== FLY_HOST) return null;

  target.protocol = publicUrl.protocol;
  target.host = publicUrl.host;
  return target.toString();
}
