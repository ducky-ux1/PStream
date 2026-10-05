/**
 * PStream Cloud Edge Stream Proxy (Cloudflare Worker / Serverless Edge)
 * Zero-PC Required - 24/7/365 Autonomous Streaming Engine
 * 
 * Features:
 * - VixSrc direct HLS master playlist extractor (.m3u8)
 * - Dynamic parameter and token resolver
 * - In-flight playlist rewriting with origin and referer bypass
 * - AES-128 key proxying (/storage/enc.key)
 * - Multi-language audio (English, Italian, Spanish, German, French)
 * - Full CORS headers (Access-Control-Allow-Origin: *)
 */

const VIX_HEADERS = {
  "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36",
  "Referer": "https://vixsrc.to/",
  "Accept-Language": "en-US,en;q=0.9",
  "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"
};

const CORS_HEADERS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, HEAD, OPTIONS",
  "Access-Control-Allow-Headers": "*",
  "Cache-Control": "no-cache, no-store, must-revalidate"
};

async function fetchJson(url, headers) {
  const resp = await fetch(url, { headers });
  if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
  return await resp.json();
}

async function fetchText(url, headers) {
  const resp = await fetch(url, { headers });
  if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
  return await resp.text();
}

async function extractVixMaster(mid, mtype, season, episode, lang) {
  const apiUrl = (mtype === "movie")
    ? `https://vixsrc.to/api/movie/${mid}?lang=${lang}`
    : `https://vixsrc.to/api/tv/${mid}/${season}/${episode}?lang=${lang}`;

  const data = await fetchJson(apiUrl, VIX_HEADERS);
  if (!data || !data.src) return null;

  const embedUrl = data.src.startsWith("/") ? `https://vixsrc.to${data.src}` : data.src;
  const html = await fetchText(embedUrl, VIX_HEADERS);

  const matchParams = html.match(/masterPlaylist\s*=\s*\{[^}]+params:\s*\{([^}]+)\}[^}]+url:\s*'([^']+)'/);
  if (!matchParams) {
    // Fallback regex
    const t = html.match(/'token':\s*'([^']+)'/);
    const ex = html.match(/'expires':\s*'([^']+)'/);
    const p = html.match(/\/playlist\/(\d+)/) || html.match(/id:\s*'(\d+)'/);
    if (!t || !ex || !p) return null;
    const base = `https://vixsrc.to/playlist/${p[1]}`;
    const mUrl = `${base}?token=${t[1]}&expires=${ex[1]}&h=1&lang=${lang}`;
    const reqHeaders = { ...VIX_HEADERS, "Referer": embedUrl, "Origin": "https://vixsrc.to" };
    return await fetchText(mUrl, reqHeaders);
  }

  const paramsStr = matchParams[1];
  const baseUrl = matchParams[2];
  const tokenM = paramsStr.match(/'token':\s*'([^']+)'/);
  const expiresM = paramsStr.match(/'expires':\s*'([^']+)'/);
  if (!tokenM || !expiresM) return null;

  const masterUrl = `${baseUrl}?token=${tokenM[1]}&expires=${expiresM[1]}&h=1&lang=${lang}`;
  const reqHeaders = {
    ...VIX_HEADERS,
    "Referer": embedUrl,
    "Origin": "https://vixsrc.to",
    "Accept": "*/*"
  };

  return await fetchText(masterUrl, reqHeaders);
}

function rewriteMasterPlaylist(content, hostPrefix) {
  const lines = content.split("\n");
  const out = [];
  const uriRegex = /URI="([^"]+)"/g;

  for (const line of lines) {
    const trimmed = line.trim();
    if (trimmed.startsWith("http")) {
      out.push(`${hostPrefix}/api/stream/sub.m3u8?url=${encodeURIComponent(trimmed)}`);
    } else if (trimmed.startsWith("#EXT-X-MEDIA")) {
      out.push(trimmed.replace(uriRegex, (match, u) => {
        return `URI="${hostPrefix}/api/stream/sub.m3u8?url=${encodeURIComponent(u)}"`;
      }));
    } else {
      out.push(line);
    }
  }
  return out.join("\n");
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);

    if (request.method === "OPTIONS") {
      return new Response(null, { headers: CORS_HEADERS });
    }

    const hostPrefix = `${url.protocol}//${url.host}`;

    // 1. Stream Resolver Endpoint
    if (url.pathname === "/api/stream/resolve") {
      const mid = url.searchParams.get("id");
      const mtype = url.searchParams.get("type") || "movie";
      const season = url.searchParams.get("season") || "1";
      const episode = url.searchParams.get("episode") || "1";
      const lang = url.searchParams.get("lang") || "en";
      const provider = url.searchParams.get("provider") || "eng";

      try {
        const streamUrl = `${hostPrefix}/api/stream/master.m3u8?id=${mid}&type=${mtype}&season=${season}&episode=${episode}&lang=${lang}&provider=${provider}`;
        return new Response(JSON.stringify({
          status: "ok",
          streamUrl: streamUrl,
          type: "hls",
          provider: provider
        }), {
          headers: { ...CORS_HEADERS, "Content-Type": "application/json" }
        });
      } catch (err) {
        return new Response(JSON.stringify({ status: "error", error: err.message }), {
          status: 500,
          headers: { ...CORS_HEADERS, "Content-Type": "application/json" }
        });
      }
    }

    // 2. Master Playlist Proxy
    if (url.pathname === "/api/stream/master.m3u8") {
      const mid = url.searchParams.get("id");
      const mtype = url.searchParams.get("type") || "movie";
      const season = url.searchParams.get("season") || "1";
      const episode = url.searchParams.get("episode") || "1";
      const lang = url.searchParams.get("lang") || "en";

      try {
        let masterContent = await extractVixMaster(mid, mtype, season, episode, lang);
        if (!masterContent && lang !== "en") {
          masterContent = await extractVixMaster(mid, mtype, season, episode, "en");
        }
        if (!masterContent) {
          return new Response("#EXTM3U\n# Stream unavailable", { status: 404, headers: CORS_HEADERS });
        }
        const rewritten = rewriteMasterPlaylist(masterContent, hostPrefix);
        return new Response(rewritten, {
          headers: {
            ...CORS_HEADERS,
            "Content-Type": "application/vnd.apple.mpegurl"
          }
        });
      } catch (err) {
        return new Response(err.message, { status: 500, headers: CORS_HEADERS });
      }
    }

    // 3. Sub-Playlist Proxy & AES Key Rewriter
    if (url.pathname === "/api/stream/sub.m3u8") {
      const targetUrl = url.searchParams.get("url");
      if (!targetUrl) return new Response("Missing target url", { status: 400 });

      try {
        const text = await fetchText(targetUrl, {
          ...VIX_HEADERS,
          "Referer": "https://vixsrc.to/"
        });

        const keyUrl = `${hostPrefix}/api/stream/key`;
        let rewritten = text.replaceAll('URI="/storage/enc.key"', `URI="${keyUrl}"`);
        rewritten = rewritten.replaceAll("URI='/storage/enc.key'", `URI="${keyUrl}"`);

        const parsed = new URL(targetUrl);
        const baseOrigin = `${parsed.protocol}//${parsed.host}`;
        const basePath = targetUrl.substring(0, targetUrl.lastIndexOf("/"));

        const lines = rewritten.split("\n");
        const out = [];
        for (const line of lines) {
          const trimmed = line.trim();
          if (trimmed && !trimmed.startsWith("#") && !trimmed.startsWith("http")) {
            if (trimmed.startsWith("/")) {
              out.push(baseOrigin + trimmed);
            } else {
              out.push(basePath + "/" + trimmed);
            }
          } else {
            out.push(line);
          }
        }

        return new Response(out.join("\n"), {
          headers: {
            ...CORS_HEADERS,
            "Content-Type": "application/vnd.apple.mpegurl"
          }
        });
      } catch (err) {
        return new Response(err.message, { status: 500, headers: CORS_HEADERS });
      }
    }

    // 4. AES-128 Key Proxy
    if (url.pathname === "/api/stream/key") {
      try {
        const resp = await fetch("https://vixsrc.to/storage/enc.key", {
          headers: {
            ...VIX_HEADERS,
            "Referer": "https://vixsrc.to/"
          }
        });
        const keyData = await resp.arrayBuffer();
        return new Response(keyData, {
          headers: {
            ...CORS_HEADERS,
            "Content-Type": "application/octet-stream"
          }
        });
      } catch (err) {
        return new Response(err.message, { status: 500, headers: CORS_HEADERS });
      }
    }

    return new Response(JSON.stringify({ status: "ok", message: "PStream Cloud Edge Running" }), {
      headers: { ...CORS_HEADERS, "Content-Type": "application/json" }
    });
  }
};
