import http.server
import socketserver
import urllib.parse
import urllib.request
import json
import time
import os

PORT = 8080
DIRECTORY = os.path.dirname(os.path.abspath(__file__))
API_KEY = "adc5047f27e588c9347087931a696cf4"
TMDB_BASE = "https://api.themoviedb.org/3"

cache = {
    "feeds": {},
    "details": {},
    "tv_seasons": {},
    "episodes": {},
    "streams": {},
    "enc_key": None
}

LANGUAGE_MAP = {
    "eng": {"tmdb": "en-US", "stream": "en", "name": "English (ENG)"},
    "ita": {"tmdb": "it-IT", "stream": "it", "name": "Italiano (ITA)"},
    "esp": {"tmdb": "es-ES", "stream": "es", "name": "Español (ESP)"},
    "ger": {"tmdb": "de-DE", "stream": "de", "name": "Deutsch (GER)"},
    "fra": {"tmdb": "fr-FR", "stream": "fr", "name": "Français (FRA)"},
    "anime": {"tmdb": "ja-JP", "stream": "en", "name": "Anime Hub (JPN/SUB)"},
    "por": {"tmdb": "pt-BR", "stream": "pt", "name": "Português (POR)"},
}

def resolve_lang(lang_key):
    if not lang_key:
        return "en-US", "en"
    key = str(lang_key).lower().strip()
    if key in LANGUAGE_MAP:
        return LANGUAGE_MAP[key]["tmdb"], LANGUAGE_MAP[key]["stream"]
    if "-" in key:
        return key, key.split("-")[0]
    if len(key) == 2:
        return f"{key}-{key.upper()}", key
    return "en-US", "en"

def tmdb_get(endpoint, lang="en-US"):
    sep = '&' if '?' in endpoint else '?'
    url = f"{TMDB_BASE}{endpoint}{sep}api_key={API_KEY}&language={lang}"
    req = urllib.request.Request(url, headers={"User-Agent": "PStream-Netflix/2.0"})
    try:
        with urllib.request.urlopen(req, timeout=8) as resp:
            return json.loads(resp.read().decode('utf-8'))
    except Exception as e:
        print(f"[TMDb Error] {endpoint} ({lang}): {e}")
        return None

def format_item(it, def_type="movie"):
    mtype = it.get("media_type", def_type)
    title = it.get("title") or it.get("name") or it.get("original_title") or "Unknown"
    date = it.get("release_date") or it.get("first_air_date") or ""
    year = date[:4] if date else ""
    poster = f"https://image.tmdb.org/t/p/w500{it['poster_path']}" if it.get("poster_path") else ""
    backdrop = f"https://image.tmdb.org/t/p/w1280{it['backdrop_path']}" if it.get("backdrop_path") else ""
    rating = round(it.get("vote_average", 0.0), 1)
    match_pct = min(99, max(75, int(rating * 10 + 15))) if rating > 0 else 92

    return {
        "id": str(it.get("id")),
        "title": title,
        "type": mtype,
        "year": year,
        "overview": it.get("overview", ""),
        "rating": str(rating),
        "match": f"{match_pct}% Match",
        "poster": poster,
        "backdrop": backdrop
    }

def get_tab_feed(tab="home", lang_param="en-US"):
    tmdb_lang, _ = resolve_lang(lang_param)
    now = time.time()
    cache_key = f"feed_{tab}_{tmdb_lang}"
    if cache_key in cache["feeds"]:
        cached_entry = cache["feeds"][cache_key]
        if now - cached_entry["time"] < 3600:
            return cached_entry["data"]

    print(f"[PStream Server] Fetching fresh catalog for tab: {tab} (lang: {tmdb_lang})...")
    feed = {
        "tab": tab,
        "featured": None,
        "categories": []
    }

    if tab == "movies":
        # Movies Tab
        pop = tmdb_get("/movie/popular", lang=tmdb_lang)
        trend = tmdb_get("/trending/movie/week", lang=tmdb_lang)
        top = tmdb_get("/movie/top_rated", lang=tmdb_lang)
        act = tmdb_get("/discover/movie?with_genres=28&sort_by=popularity.desc", lang=tmdb_lang)
        scifi = tmdb_get("/discover/movie?with_genres=878&sort_by=popularity.desc", lang=tmdb_lang)
        horror = tmdb_get("/discover/movie?with_genres=27&sort_by=popularity.desc", lang=tmdb_lang)
        comedy = tmdb_get("/discover/movie?with_genres=35&sort_by=popularity.desc", lang=tmdb_lang)

        if trend and "results" in trend:
            items = [format_item(x, "movie") for x in trend["results"] if x.get("poster_path")]
            if items:
                feed["featured"] = items[0]
                feed["categories"].append({"title": "Top 10 Movies Today", "isTop10": True, "items": items[:10]})

        if pop and "results" in pop:
            items = [format_item(x, "movie") for x in pop["results"] if x.get("poster_path")]
            if items:
                feed["categories"].append({"title": "Popular on PStream", "items": items})

        if top and "results" in top:
            items = [format_item(x, "movie") for x in top["results"] if x.get("poster_path")]
            if items:
                feed["categories"].append({"title": "Critically Acclaimed Films", "items": items})

        if act and "results" in act:
            items = [format_item(x, "movie") for x in act["results"] if x.get("poster_path")]
            if items:
                feed["categories"].append({"title": "Action & Adrenaline", "items": items})

        if scifi and "results" in scifi:
            items = [format_item(x, "movie") for x in scifi["results"] if x.get("poster_path")]
            if items:
                feed["categories"].append({"title": "Sci-Fi & Cyberpunk", "items": items})

        if horror and "results" in horror:
            items = [format_item(x, "movie") for x in horror["results"] if x.get("poster_path")]
            if items:
                feed["categories"].append({"title": "Chilling Horror & Thrillers", "items": items})

        if comedy and "results" in comedy:
            items = [format_item(x, "movie") for x in comedy["results"] if x.get("poster_path")]
            if items:
                feed["categories"].append({"title": "Laugh-Out-Loud Comedies", "items": items})

    elif tab == "tv":
        # TV Shows Tab
        trend = tmdb_get("/trending/tv/week", lang=tmdb_lang)
        pop = tmdb_get("/tv/popular", lang=tmdb_lang)
        top = tmdb_get("/tv/top_rated", lang=tmdb_lang)
        drama = tmdb_get("/discover/tv?with_genres=18&sort_by=popularity.desc", lang=tmdb_lang)
        scifi = tmdb_get("/discover/tv?with_genres=10765&sort_by=popularity.desc", lang=tmdb_lang)
        comedy = tmdb_get("/discover/tv?with_genres=35&sort_by=popularity.desc", lang=tmdb_lang)

        if trend and "results" in trend:
            items = [format_item(x, "tv") for x in trend["results"] if x.get("poster_path")]
            if items:
                feed["featured"] = items[0]
                feed["categories"].append({"title": "Top 10 Series Today", "isTop10": True, "items": items[:10]})

        if pop and "results" in pop:
            items = [format_item(x, "tv") for x in pop["results"] if x.get("poster_path")]
            if items:
                feed["categories"].append({"title": "Binge-Worthy TV Shows", "items": items})

        if top and "results" in top:
            items = [format_item(x, "tv") for x in top["results"] if x.get("poster_path")]
            if items:
                feed["categories"].append({"title": "Highest Rated TV Dramas", "items": items})

        if drama and "results" in drama:
            items = [format_item(x, "tv") for x in drama["results"] if x.get("poster_path")]
            if items:
                feed["categories"].append({"title": "Gripping Dramas", "items": items})

        if scifi and "results" in scifi:
            items = [format_item(x, "tv") for x in scifi["results"] if x.get("poster_path")]
            if items:
                feed["categories"].append({"title": "Sci-Fi & Fantasy Series", "items": items})

        if comedy and "results" in comedy:
            items = [format_item(x, "tv") for x in comedy["results"] if x.get("poster_path")]
            if items:
                feed["categories"].append({"title": "TV Comedies", "items": items})

    elif tab == "anime":
        # Anime Tab
        trend_anime = tmdb_get("/discover/tv?with_genres=16&with_original_language=ja&sort_by=popularity.desc", lang=tmdb_lang)
        top_anime = tmdb_get("/discover/tv?with_genres=16&with_original_language=ja&sort_by=vote_average.desc&vote_count.gte=100", lang=tmdb_lang)
        anime_movies = tmdb_get("/discover/movie?with_genres=16&with_original_language=ja&sort_by=popularity.desc", lang=tmdb_lang)
        action_anime = tmdb_get("/discover/tv?with_genres=16,10759&with_original_language=ja&sort_by=popularity.desc", lang=tmdb_lang)

        if trend_anime and "results" in trend_anime:
            items = [format_item(x, "tv") for x in trend_anime["results"] if x.get("poster_path")]
            if items:
                feed["featured"] = items[0]
                feed["categories"].append({"title": "Top 10 Anime Today", "isTop10": True, "items": items[:10]})

        if top_anime and "results" in top_anime:
            items = [format_item(x, "tv") for x in top_anime["results"] if x.get("poster_path")]
            if items:
                feed["categories"].append({"title": "Critically Acclaimed Anime", "items": items})

        if anime_movies and "results" in anime_movies:
            items = [format_item(x, "movie") for x in anime_movies["results"] if x.get("poster_path")]
            if items:
                feed["categories"].append({"title": "Feature Anime Films", "items": items})

        if action_anime and "results" in action_anime:
            items = [format_item(x, "tv") for x in action_anime["results"] if x.get("poster_path")]
            if items:
                feed["categories"].append({"title": "Shonen & Action Anime", "items": items})

    else:
        # Default Home Tab (Mixed blockbuster experience)
        trend = tmdb_get("/trending/all/week", lang=tmdb_lang)
        pop_m = tmdb_get("/movie/popular", lang=tmdb_lang)
        pop_tv = tmdb_get("/tv/popular", lang=tmdb_lang)
        top_m = tmdb_get("/movie/top_rated", lang=tmdb_lang)
        top_tv = tmdb_get("/tv/top_rated", lang=tmdb_lang)
        act = tmdb_get("/discover/movie?with_genres=28&sort_by=popularity.desc", lang=tmdb_lang)
        anime = tmdb_get("/discover/tv?with_genres=16&with_original_language=ja&sort_by=popularity.desc", lang=tmdb_lang)
        scifi = tmdb_get("/discover/movie?with_genres=878&sort_by=popularity.desc", lang=tmdb_lang)

        if trend and "results" in trend:
            items = [format_item(x) for x in trend["results"] if x.get("poster_path")]
            if items:
                feed["featured"] = items[0]
                feed["categories"].append({"title": "Top 10 Today", "isTop10": True, "items": items[:10]})

        if pop_m and "results" in pop_m:
            items = [format_item(x, "movie") for x in pop_m["results"] if x.get("poster_path")]
            if items:
                feed["categories"].append({"title": "Blockbuster Movies", "items": items})

        if pop_tv and "results" in pop_tv:
            items = [format_item(x, "tv") for x in pop_tv["results"] if x.get("poster_path")]
            if items:
                feed["categories"].append({"title": "Hit TV Series", "items": items})

        if top_m and "results" in top_m:
            items = [format_item(x, "movie") for x in top_m["results"] if x.get("poster_path")]
            if items:
                feed["categories"].append({"title": "Critically Acclaimed Movies", "items": items})

        if top_tv and "results" in top_tv:
            items = [format_item(x, "tv") for x in top_tv["results"] if x.get("poster_path")]
            if items:
                feed["categories"].append({"title": "Award-Winning TV Dramas", "items": items})

        if act and "results" in act:
            items = [format_item(x, "movie") for x in act["results"] if x.get("poster_path")]
            if items:
                feed["categories"].append({"title": "Action & Adventure", "items": items})

        if anime and "results" in anime:
            items = [format_item(x, "tv") for x in anime["results"] if x.get("poster_path")]
            if items:
                feed["categories"].append({"title": "Japanese Anime & Animation", "items": items})

        if scifi and "results" in scifi:
            items = [format_item(x, "movie") for x in scifi["results"] if x.get("poster_path")]
            if items:
                feed["categories"].append({"title": "Sci-Fi & Cyberpunk", "items": items})

    cache["feeds"][cache_key] = {"data": feed, "time": now}
    return feed

def get_media_details(media_id, mtype="movie", lang_param="en-US"):
    tmdb_lang, _ = resolve_lang(lang_param)
    cache_key = f"{mtype}_{media_id}_{tmdb_lang}"
    if cache_key in cache["details"]:
        return cache["details"][cache_key]

    endpoint = f"/{mtype}/{media_id}?append_to_response=credits,recommendations,release_dates,content_ratings,videos"
    data = tmdb_get(endpoint, lang=tmdb_lang)
    if not data:
        return None

    title = data.get("title") or data.get("name") or "Unknown"
    date = data.get("release_date") or data.get("first_air_date") or ""
    year = date[:4] if date else ""
    rating = round(data.get("vote_average", 0.0), 1)
    match_pct = min(99, max(75, int(rating * 10 + 15))) if rating > 0 else 94

    # Runtime or Seasons text
    if mtype == "tv":
        num_seasons = len([s for s in data.get("seasons", []) if s.get("season_number", 0) > 0])
        duration_text = f"{num_seasons} Season{'s' if num_seasons != 1 else ''}"
    else:
        rt = data.get("runtime", 0)
        hours = rt // 60
        mins = rt % 60
        duration_text = f"{hours}h {mins}m" if hours > 0 else f"{mins}m"

    # Certification / Age rating
    cert = "16+"
    if mtype == "movie" and "release_dates" in data:
        for country in data["release_dates"].get("results", []):
            if country.get("iso_3166_1") == "US":
                for rel in country.get("release_dates", []):
                    c = rel.get("certification")
                    if c:
                        cert = c
                        break
    elif mtype == "tv" and "content_ratings" in data:
        for cr in data["content_ratings"].get("results", []):
            if cr.get("iso_3166_1") == "US":
                c = cr.get("rating")
                if c:
                    cert = c
                    break

    # Genres
    genres = [g["name"] for g in data.get("genres", [])]

    # Cast & Director
    cast = []
    director = "Unknown"
    if "credits" in data:
        cast = [c["name"] for c in data["credits"].get("cast", [])[:6]]
        for crew in data["credits"].get("crew", []):
            if crew.get("job") == "Director":
                director = crew.get("name")
                break

    # Similar recommendations
    recommendations = []
    if "recommendations" in data and "results" in data["recommendations"]:
        recommendations = [format_item(x, mtype) for x in data["recommendations"]["results"] if x.get("poster_path")][:12]

    # Seasons for TV
    seasons = []
    if mtype == "tv" and "seasons" in data:
        seasons = [{
            "season_number": s["season_number"],
            "name": s.get("name", f"Season {s['season_number']}"),
            "episode_count": s.get("episode_count", 0)
        } for s in data["seasons"] if s.get("season_number", 0) > 0]

    # YouTube Trailer key
    trailer_key = ""
    if "videos" in data and "results" in data["videos"]:
        for vid in data["videos"]["results"]:
            if vid.get("site") == "YouTube" and vid.get("type") in ("Trailer", "Teaser"):
                trailer_key = vid.get("key", "")
                break

    details = {
        "id": str(media_id),
        "title": title,
        "type": mtype,
        "year": year,
        "overview": data.get("overview", ""),
        "rating": str(rating),
        "match": f"{match_pct}% Match",
        "duration": duration_text,
        "certification": cert,
        "genres": genres,
        "cast": cast,
        "director": director,
        "poster": f"https://image.tmdb.org/t/p/w500{data['poster_path']}" if data.get("poster_path") else "",
        "backdrop": f"https://image.tmdb.org/t/p/w1280{data['backdrop_path']}" if data.get("backdrop_path") else "",
        "seasons": seasons,
        "recommendations": recommendations,
        "trailer_key": trailer_key
    }

    cache["details"][cache_key] = details
    return details

VIX_HEADERS = {
    "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36",
    "Accept": "*/*",
    "Accept-Language": "en-US,en;q=0.9",
    "Referer": "https://vixsrc.to/",
    "Origin": "https://vixsrc.to"
}

def _do_extract_vix(media_id, mtype="movie", season=1, episode=1, lang="en"):
    import re
    for attempt in range(2):
        try:
            if mtype == "movie":
                api_url = f"https://vixsrc.to/api/movie/{media_id}?lang={lang}"
            else:
                api_url = f"https://vixsrc.to/api/tv/{media_id}/{season}/{episode}?lang={lang}"

            req = urllib.request.Request(api_url, headers=VIX_HEADERS)
            with urllib.request.urlopen(req, timeout=12) as resp:
                data = json.loads(resp.read().decode('utf-8'))
                src = data.get("src", "")
                if not src:
                    return None, "No embed source in API"

            embed_url = "https://vixsrc.to" + src if src.startswith("/") else src
            req2 = urllib.request.Request(embed_url, headers=VIX_HEADERS)
            with urllib.request.urlopen(req2, timeout=12) as resp2:
                html = resp2.read().decode('utf-8', errors='ignore')

            # Primary: Extract window.masterPlaylist params
            match_params = re.search(r"masterPlaylist\s*=\s*\{[^}]+params:\s*\{([^}]+)\}[^}]+url:\s*'([^']+)'", html)
            if match_params:
                params_str = match_params.group(1)
                base_url = match_params.group(2)
                token_m = re.search(r"'token':\s*'([^']+)'", params_str)
                expires_m = re.search(r"'expires':\s*'([^']+)'", params_str)
                if token_m and expires_m:
                    token = token_m.group(1)
                    expires = expires_m.group(1)
                    master_url = f"{base_url}?token={token}&expires={expires}&h=1&lang={lang}"
                else:
                    return None, "Missing token/expires in masterPlaylist params"
            else:
                # Fallback: Extract from script variables
                token = re.findall(r"'token':\s*'([^']+)'", html)
                expires = re.findall(r"'expires':\s*'([^']+)'", html)
                vid = re.findall(r"/playlist/(\d+)", html) or re.findall(r"id:\s*'(\d+)'", html)
                if not (token and expires and vid):
                    return None, "Failed to parse stream metadata"
                base_url = f"https://vixsrc.to/playlist/{vid[0]}"
                master_url = f"{base_url}?token={token[0]}&expires={expires[0]}&h=1&lang={lang}"

            req_headers = dict(VIX_HEADERS)
            req_headers["Referer"] = embed_url
            req_headers["Origin"] = "https://vixsrc.to"
            req_headers["Accept"] = "*/*"

            req3 = urllib.request.Request(master_url, headers=req_headers)
            with urllib.request.urlopen(req3, timeout=12) as resp3:
                content = resp3.read().decode('utf-8', errors='ignore')

            if content and "#EXTM3U" in content:
                return content, master_url
        except Exception as e:
            print(f"[VixSrc Extract Error attempt {attempt+1}] {media_id} ({mtype}, lang={lang}): {e}")
            if attempt == 1:
                return None, str(e)
            time.sleep(0.4)

    return None, "Extraction timed out"

def extract_vix_master(media_id, mtype="movie", season=1, episode=1, lang="en", provider="server1"):
    _, stream_lang = resolve_lang(lang or provider)
    cache_key = f"{media_id}_{mtype}_{season}_{episode}_{stream_lang}"
    now = time.time()
    if cache_key in cache["streams"]:
        entry = cache["streams"][cache_key]
        if now - entry["time"] < 7200:
            return entry["content"], entry["master_url"]

    content, master_url = _do_extract_vix(media_id, mtype, season, episode, stream_lang)
    if not content and stream_lang != "en":
        print(f"[VixSrc] Localized {stream_lang} stream not found, falling back to 'en'...")
        content, master_url = _do_extract_vix(media_id, mtype, season, episode, "en")

    if content and "#EXTM3U" in content:
        cache["streams"][cache_key] = {
            "time": now,
            "content": content,
            "master_url": master_url
        }
        return content, master_url

    return None, master_url

def rewrite_master_playlist(content, host_prefix):
    import re
    lines = content.splitlines()
    out = []
    uri_regex = re.compile(r'URI="([^"]+)"')
    for line in lines:
        if line.startswith("http"):
            sub_url = f"{host_prefix}/api/stream/sub.m3u8?url=" + urllib.parse.quote(line)
            out.append(sub_url)
        elif line.startswith("#EXT-X-MEDIA"):
            def replace_uri(m):
                u = m.group(1)
                return f'URI="{host_prefix}/api/stream/sub.m3u8?url={urllib.parse.quote(u)}"'
            out.append(uri_regex.sub(replace_uri, line))
        else:
            out.append(line)
    return "\n".join(out)

def fetch_and_rewrite_sub(sub_url, host_prefix):
    headers = dict(VIX_HEADERS)
    headers["Referer"] = "https://vixsrc.to/"
    req = urllib.request.Request(sub_url, headers=headers)
    with urllib.request.urlopen(req, timeout=12) as resp:
        text = resp.read().decode('utf-8', errors='ignore')

    key_proxy_url = f"{host_prefix}/api/stream/key"
    text = text.replace('URI="/storage/enc.key"', f'URI="{key_proxy_url}"')
    text = text.replace("URI='/storage/enc.key'", f'URI="{key_proxy_url}"')

    parsed = urllib.parse.urlparse(sub_url)
    base_origin = f"{parsed.scheme}://{parsed.netloc}"
    lines = text.splitlines()
    out = []
    for line in lines:
        if line and not line.startswith("#") and not line.startswith("http"):
            if line.startswith("/"):
                out.append(base_origin + line)
            else:
                out.append(sub_url.rsplit("/", 1)[0] + "/" + line)
        else:
            out.append(line)
    return "\n".join(out)

def fetch_enc_key():
    now = time.time()
    if cache["enc_key"] and now - cache["enc_key"]["time"] < 3600:
        return cache["enc_key"]["bytes"]

    headers = dict(VIX_HEADERS)
    headers["Referer"] = "https://vixsrc.to/"
    req = urllib.request.Request("https://vixsrc.to/storage/enc.key", headers=headers)
    with urllib.request.urlopen(req, timeout=12) as resp:
        b = resp.read()
        cache["enc_key"] = {"time": now, "bytes": b}
        return b

class PStreamHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=DIRECTORY, **kwargs)

    def end_headers(self):
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Cache-Control', 'no-cache, no-store, must-revalidate')
        super().end_headers()

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        params = urllib.parse.parse_qs(parsed.query)

        if parsed.path == '/log':
            msg = params.get('msg', [''])[0]
            print(f"\n>>> [PS5 LOG] {msg}")
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain')
            self.end_headers()
            self.wfile.write(b"OK")
            return

        if parsed.path == '/api/feed':
            tab = params.get('tab', ['home'])[0]
            lang = params.get('lang', ['en-US'])[0]
            feed = get_tab_feed(tab, lang)
            payload = json.dumps(feed).encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return

        if parsed.path == '/api/details':
            mid = params.get('id', [''])[0]
            mtype = params.get('type', ['movie'])[0]
            lang = params.get('lang', ['en-US'])[0]
            if mid:
                details = get_media_details(mid, mtype, lang)
                if details:
                    payload = json.dumps(details).encode('utf-8')
                    self.send_response(200)
                    self.send_header('Content-Type', 'application/json')
                    self.send_header('Content-Length', str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)
                    return
            self.send_error(404, "Title not found")
            return

        if parsed.path == '/api/tv':
            tv_id = params.get('id', [''])[0]
            lang = params.get('lang', ['en-US'])[0]
            if tv_id:
                details = get_media_details(tv_id, "tv", lang)
                if details:
                    payload = json.dumps(details).encode('utf-8')
                    self.send_response(200)
                    self.send_header('Content-Type', 'application/json')
                    self.send_header('Content-Length', str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)
                    return
            self.send_error(404, "TV Show not found")
            return

        if parsed.path == '/api/season':
            tv_id = params.get('id', [''])[0]
            season_num = params.get('season', ['1'])[0]
            lang = params.get('lang', ['en-US'])[0]
            tmdb_lang, _ = resolve_lang(lang)
            cache_key = f"{tv_id}_s{season_num}_{tmdb_lang}"
            if cache_key in cache["episodes"]:
                episodes = cache["episodes"][cache_key]
            else:
                data = tmdb_get(f"/tv/{tv_id}/season/{season_num}", lang=tmdb_lang)
                episodes = []
                if data and "episodes" in data:
                    for ep in data["episodes"]:
                        still = f"https://image.tmdb.org/t/p/w300{ep['still_path']}" if ep.get("still_path") else ""
                        runtime = ep.get("runtime", 0)
                        r_str = f"{runtime}m" if runtime else ""
                        episodes.append({
                            "episode_number": ep["episode_number"],
                            "name": ep.get("name", f"Episode {ep['episode_number']}"),
                            "overview": ep.get("overview", ""),
                            "still": still,
                            "runtime": r_str
                        })
                    cache["episodes"][cache_key] = episodes
            payload = json.dumps({"episodes": episodes}).encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(payload)
            return

        if parsed.path == '/api/search':
            query = params.get('q', [''])[0]
            lang = params.get('lang', ['en-US'])[0]
            tmdb_lang, _ = resolve_lang(lang)
            results = []
            if query:
                data = tmdb_get(f"/search/multi?query={urllib.parse.quote(query)}", lang=tmdb_lang)
                if data and "results" in data:
                    for x in data["results"]:
                        if x.get("media_type") in ("movie", "tv") and x.get("poster_path"):
                            results.append(format_item(x, x.get("media_type")))
            payload = json.dumps({"results": results}).encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return

        if parsed.path == '/api/stream/resolve':
            mid = params.get('id', [''])[0]
            mtype = params.get('type', ['movie'])[0]
            season = int(params.get('season', ['1'])[0])
            episode = int(params.get('episode', ['1'])[0])
            lang = params.get('lang', ['en'])[0]
            provider = params.get('provider', ['server1'])[0]
            _, stream_lang = resolve_lang(provider if provider in LANGUAGE_MAP else lang)

            master_content, err_msg = extract_vix_master(mid, mtype, season, episode, stream_lang, provider)
            host = self.headers.get('Host', f'192.168.1.197:{PORT}')
            if master_content:
                stream_url = f"http://{host}/api/stream/master.m3u8?id={mid}&type={mtype}&season={season}&episode={episode}&lang={stream_lang}&provider={provider}"
                payload = json.dumps({"status": "ok", "streamUrl": stream_url, "type": "hls", "provider": provider}).encode('utf-8')
            else:
                # ZERO ADS GUARANTEE: Never, ever return ad-infested iframe embeds to the user!
                payload = json.dumps({
                    "status": "error",
                    "error": f"Direct stream temporarily unavailable from this server ({err_msg}). Please switch provider in Settings or try another title.",
                    "type": "error"
                }).encode('utf-8')

            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return

        if parsed.path == '/api/stream/master.m3u8':
            mid = params.get('id', [''])[0]
            mtype = params.get('type', ['movie'])[0]
            season = int(params.get('season', ['1'])[0])
            episode = int(params.get('episode', ['1'])[0])
            lang = params.get('lang', ['en'])[0]
            provider = params.get('provider', ['server1'])[0]
            _, stream_lang = resolve_lang(provider if provider in LANGUAGE_MAP else lang)

            master_content, _ = extract_vix_master(mid, mtype, season, episode, stream_lang, provider)
            if master_content:
                host = self.headers.get('Host', f'192.168.1.197:{PORT}')
                host_prefix = f"http://{host}"
                rewritten = rewrite_master_playlist(master_content, host_prefix).encode('utf-8')
                self.send_response(200)
                self.send_header('Content-Type', 'application/vnd.apple.mpegurl; charset=utf-8')
                self.send_header('Content-Length', str(len(rewritten)))
                self.end_headers()
                self.wfile.write(rewritten)
                return
            else:
                self.send_error(404, "Stream not found")
                return

        if parsed.path == '/api/stream/sub.m3u8':
            sub_url = params.get('url', [''])[0]
            if sub_url:
                try:
                    host = self.headers.get('Host', f'192.168.1.197:{PORT}')
                    host_prefix = f"http://{host}"
                    rewritten = fetch_and_rewrite_sub(sub_url, host_prefix).encode('utf-8')
                    self.send_response(200)
                    self.send_header('Content-Type', 'application/vnd.apple.mpegurl; charset=utf-8')
                    self.send_header('Content-Length', str(len(rewritten)))
                    self.end_headers()
                    self.wfile.write(rewritten)
                    return
                except Exception as e:
                    print(f"[Sub Playlist Error]: {e}")
                    self.send_error(500, f"Error fetching sub playlist: {e}")
                    return
            self.send_error(400, "Missing url parameter")
            return

        if parsed.path == '/api/stream/key':
            try:
                key_bytes = fetch_enc_key()
                self.send_response(200)
                self.send_header('Content-Type', 'application/octet-stream')
                self.send_header('Content-Length', str(len(key_bytes)))
                self.end_headers()
                self.wfile.write(key_bytes)
                return
            except Exception as e:
                self.send_error(500, f"Error fetching key: {e}")
                return

        return super().do_GET()

if __name__ == '__main__':
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("0.0.0.0", PORT), PStreamHandler) as httpd:
        print(f"[PStream] Netflix-Grade Server running on 0.0.0.0:{PORT} from {DIRECTORY}")
        httpd.serve_forever()
