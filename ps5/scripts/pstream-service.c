#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <ps5/klog.h>

#define SERVER_PORT 8086
#define APP_DIR "/user/app/PSTR00001"
#define HOST_PREFIX "http://127.0.0.1:8086"

#define MAX_LOG_LINES 250
#define LOG_LINE_LEN 256

// Sony Networking & SSL Prototypes
int sceNetInit(void);
int sceNetPoolCreate(const char*, int, int);
int sceNetPoolDestroy(int);
int sceSslInit(size_t);
int sceSslTerm(int);
int sceHttp2Init(int, int, size_t, int);
int sceHttp2Term(int);
int sceHttp2CreateTemplate(int, const char*, int, int);
int sceHttp2DeleteTemplate(int);
int sceHttp2CreateRequestWithURL(int, const char*, const char*, uint64_t);
int sceHttp2DeleteRequest(int);
int sceHttp2SendRequest(int, const void*, size_t);
int sceHttp2GetStatusCode(int, int*);
int sceHttp2ReadData(int, void *, size_t);
int sceHttp2SetSslCallback(int, void*, void*);
int sceHttp2SslDisableOption(int, unsigned int);
int sceHttp2AddRequestHeader(int, const char*, const char*, int);

static int g_netMemId = -1;
static int g_sslCtxId = -1;
static int g_httpCtxId = -1;
static pthread_mutex_t g_http_mutex = PTHREAD_MUTEX_INITIALIZER;

// In-Memory Key Cache (Instant 0ms key response)
static uint8_t g_enc_key[64];
static int g_enc_key_len = 0;
static pthread_mutex_t g_key_mutex = PTHREAD_MUTEX_INITIALIZER;

// Circular In-Memory Log Buffer (Dumpable via /api/logs)
static char g_log_lines[MAX_LOG_LINES][LOG_LINE_LEN];
static int g_log_count = 0;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void add_log(const char* msg) {
    if (!msg || !msg[0]) return;
    pthread_mutex_lock(&g_log_mutex);
    snprintf(g_log_lines[g_log_count % MAX_LOG_LINES], LOG_LINE_LEN, "%s", msg);
    g_log_count++;
    pthread_mutex_unlock(&g_log_mutex);
    printf("[PStream WebKit Log] %s\n", msg);
}

static int ssl_cb(int sslId, unsigned int verifyErr, void* const cert[], int certNum, void* userArg) {
    return 1; // Accept all certificates
}

static int init_net_subsystem(void) {
    if (sceNetInit()) {
        printf("[PStream Service] sceNetInit failed\n");
        return -1;
    }
    g_netMemId = sceNetPoolCreate("pstream_net", 4 * 1024 * 1024, 0);
    g_sslCtxId = sceSslInit(2 * 1024 * 1024);
    g_httpCtxId = sceHttp2Init(g_netMemId, g_sslCtxId, 2 * 1024 * 1024, 8);
    printf("[PStream Service] Net & SSL Subsystems Initialized (8 concurrent conns)\n");
    return 0;
}

static int http_fetch(const char* url, const char* referer, char* out_buf, size_t out_max) {
    pthread_mutex_lock(&g_http_mutex);

    int tmpl = sceHttp2CreateTemplate(g_httpCtxId, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36", 3, 1);
    if (tmpl < 0) {
        pthread_mutex_unlock(&g_http_mutex);
        return -1;
    }
    sceHttp2SetSslCallback(tmpl, (void*)ssl_cb, 0);
    sceHttp2SslDisableOption(tmpl, 0xFFFFFFFF);

    int req = sceHttp2CreateRequestWithURL(tmpl, "GET", url, 0);
    if (req < 0) {
        sceHttp2DeleteTemplate(tmpl);
        pthread_mutex_unlock(&g_http_mutex);
        return -1;
    }
    sceHttp2SetSslCallback(req, (void*)ssl_cb, 0);
    sceHttp2SslDisableOption(req, 0xFFFFFFFF);

    if (referer && referer[0]) {
        sceHttp2AddRequestHeader(req, "Referer", referer, 0);
    }
    if (strstr(url, "vixsrc") || (referer && strstr(referer, "vixsrc"))) {
        sceHttp2AddRequestHeader(req, "Origin", "https://vixsrc.to", 0);
    }
    sceHttp2AddRequestHeader(req, "Accept", "*/*", 0);
    sceHttp2AddRequestHeader(req, "Accept-Language", "en-US,en;q=0.9", 0);

    int ret = sceHttp2SendRequest(req, NULL, 0);
    if (ret < 0) {
        printf("[http_fetch] send error: 0x%08X for %s\n", ret, url);
        sceHttp2DeleteRequest(req);
        sceHttp2DeleteTemplate(tmpl);
        pthread_mutex_unlock(&g_http_mutex);
        return -1;
    }

    int status = 0;
    sceHttp2GetStatusCode(req, &status);
    if (status != 200) {
        printf("[http_fetch] HTTP status %d for %s\n", status, url);
        sceHttp2DeleteRequest(req);
        sceHttp2DeleteTemplate(tmpl);
        pthread_mutex_unlock(&g_http_mutex);
        return -1;
    }

    size_t total = 0;
    while (total < out_max - 1) {
        int n = sceHttp2ReadData(req, out_buf + total, out_max - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    out_buf[total] = 0;

    sceHttp2DeleteRequest(req);
    sceHttp2DeleteTemplate(tmpl);
    pthread_mutex_unlock(&g_http_mutex);
    return (int)total;
}

static void urlencode(const char* src, char* dst, size_t dst_max) {
    static const char* hex = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_max - 4; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            dst[j++] = c;
        } else {
            dst[j++] = '%';
            dst[j++] = hex[c >> 4];
            dst[j++] = hex[c & 0x0F];
        }
    }
    dst[j] = 0;
}

static void urldecode(const char* src, char* dst, size_t dst_max) {
    size_t i = 0, j = 0;
    while (src[i] && j < dst_max - 1) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = {src[i+1], src[i+2], 0};
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else if (src[i] == '+') {
            dst[j++] = ' ';
            i++;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = 0;
}

static const char* map_lang(const char* in) {
    if (!in || !in[0]) return "en";
    if (strcmp(in, "eng") == 0 || strcmp(in, "en") == 0) return "en";
    if (strcmp(in, "ita") == 0 || strcmp(in, "it") == 0) return "it";
    if (strcmp(in, "esp") == 0 || strcmp(in, "es") == 0) return "es";
    if (strcmp(in, "ger") == 0 || strcmp(in, "de") == 0) return "de";
    if (strcmp(in, "fra") == 0 || strcmp(in, "fr") == 0) return "fr";
    if (strcmp(in, "por") == 0 || strcmp(in, "pt") == 0) return "pt";
    if (strcmp(in, "anime") == 0) return "en";
    return in;
}

static int ensure_enc_key(void) {
    pthread_mutex_lock(&g_key_mutex);
    if (g_enc_key_len >= 16) {
        pthread_mutex_unlock(&g_key_mutex);
        return g_enc_key_len;
    }

    char tmp[128];
    int len = http_fetch("https://vixsrc.to/storage/enc.key", "https://vixsrc.to/", tmp, sizeof(tmp));
    if (len >= 16) {
        memcpy(g_enc_key, tmp, len);
        g_enc_key_len = len;
        printf("[PStream] Cached AES-128 key (%d bytes)\n", len);
    }
    int res = g_enc_key_len;
    pthread_mutex_unlock(&g_key_mutex);
    return res;
}

static int extract_vix_stream(const char* id, const char* type, int season, int episode, const char* lang_in, char* out_url, size_t out_url_max) {
    const char* lang = map_lang(lang_in);
    char* buf1 = malloc(8192);
    char* buf2 = malloc(65536);
    if (!buf1 || !buf2) {
        if (buf1) free(buf1);
        if (buf2) free(buf2);
        return -1;
    }

    char api_url[512];
    if (strcmp(type, "tv") == 0) {
        snprintf(api_url, sizeof(api_url), "https://vixsrc.to/api/tv/%s/%d/%d?lang=%s", id, season, episode, lang);
    } else {
        snprintf(api_url, sizeof(api_url), "https://vixsrc.to/api/movie/%s?lang=%s", id, lang);
    }

    printf("[PStream] Resolving VixSrc: %s\n", api_url);
    int len1 = http_fetch(api_url, "https://vixsrc.to/", buf1, 8192);
    if (len1 <= 0) {
        if (strcmp(lang, "en") != 0) {
            printf("[PStream] Localized %s failed, retrying with English...\n", lang);
            if (strcmp(type, "tv") == 0) {
                snprintf(api_url, sizeof(api_url), "https://vixsrc.to/api/tv/%s/%d/%d?lang=en", id, season, episode);
            } else {
                snprintf(api_url, sizeof(api_url), "https://vixsrc.to/api/movie/%s?lang=en", id);
            }
            len1 = http_fetch(api_url, "https://vixsrc.to/", buf1, 8192);
        }
        if (len1 <= 0) {
            free(buf1);
            free(buf2);
            return -1;
        }
    }

    char* src_pos = strstr(buf1, "\"src\":\"");
    if (!src_pos) src_pos = strstr(buf1, "\"src\": \"");
    if (!src_pos) {
        free(buf1);
        free(buf2);
        return -1;
    }
    src_pos += 7;
    char* src_end = strchr(src_pos, '"');
    if (!src_end) {
        free(buf1);
        free(buf2);
        return -1;
    }
    *src_end = 0;

    char embed_url[2048];
    char clean_src[2048];
    int j = 0;
    for (int i = 0; src_pos[i]; i++) {
        if (src_pos[i] == '\\' && src_pos[i+1] == '/') continue;
        clean_src[j++] = src_pos[i];
    }
    clean_src[j] = 0;

    if (clean_src[0] == '/') {
        snprintf(embed_url, sizeof(embed_url), "https://vixsrc.to%s", clean_src);
    } else {
        snprintf(embed_url, sizeof(embed_url), "%s", clean_src);
    }

    int len2 = http_fetch(embed_url, "https://vixsrc.to/", buf2, 65536);
    if (len2 <= 0) {
        free(buf1);
        free(buf2);
        return -1;
    }

    char token[256] = {0};
    char expires[256] = {0};
    char base_url[512] = {0};

    char* t_pos = strstr(buf2, "'token': '");
    if (!t_pos) t_pos = strstr(buf2, "\"token\": \"");
    if (t_pos) {
        t_pos += 10;
        char* t_end = strchr(t_pos, '\'');
        if (!t_end) t_end = strchr(t_pos, '"');
        if (t_end) {
            size_t t_len = t_end - t_pos;
            if (t_len < sizeof(token)) {
                strncpy(token, t_pos, t_len);
                token[t_len] = 0;
            }
        }
    }

    char* exp_pos = strstr(buf2, "'expires': '");
    if (!exp_pos) exp_pos = strstr(buf2, "\"expires\": \"");
    if (exp_pos) {
        exp_pos += 12;
        char* exp_end = strchr(exp_pos, '\'');
        if (!exp_end) exp_end = strchr(exp_pos, '"');
        if (exp_end) {
            size_t e_len = exp_end - exp_pos;
            if (e_len < sizeof(expires)) {
                strncpy(expires, exp_pos, e_len);
                expires[e_len] = 0;
            }
        }
    }

    char* url_pos = strstr(buf2, "url: '");
    if (!url_pos) url_pos = strstr(buf2, "url: \"");
    if (url_pos) {
        url_pos += 6;
        char* u_end = strchr(url_pos, '\'');
        if (!u_end) u_end = strchr(url_pos, '"');
        if (u_end) {
            size_t u_len = u_end - url_pos;
            if (u_len < sizeof(base_url)) {
                strncpy(base_url, url_pos, u_len);
                base_url[u_len] = 0;
            }
        }
    }

    if (!base_url[0]) {
        char* p_pos = strstr(buf2, "/playlist/");
        if (p_pos) {
            char* p_end = strchr(p_pos, '\'');
            if (!p_end) p_end = strchr(p_pos, '"');
            if (p_end) {
                size_t p_len = p_end - p_pos;
                snprintf(base_url, sizeof(base_url), "https://vixsrc.to%.*s", (int)p_len, p_pos);
            }
        }
    }

    free(buf1);
    free(buf2);

    if (token[0] && expires[0] && base_url[0]) {
        char sep = strchr(base_url, '?') ? '&' : '?';
        snprintf(out_url, out_url_max, "%s%ctoken=%s&expires=%s&h=1&lang=%s", base_url, sep, token, expires, lang);
        printf("[PStream] Successfully Resolved Stream:\n>>> %s\n", out_url);
        return 0;
    }

    return -1;
}

static void rewrite_master_playlist(const char* in, const char* master_url, char* out, size_t out_max) {
    char base_origin[512] = {0};
    char base_dir[1024] = {0};

    const char* colon = strstr(master_url, "://");
    if (colon) {
        const char* slash = strchr(colon + 3, '/');
        if (slash) {
            size_t o_len = slash - master_url;
            if (o_len >= sizeof(base_origin)) o_len = sizeof(base_origin) - 1;
            strncpy(base_origin, master_url, o_len);
            base_origin[o_len] = 0;
        } else {
            strncpy(base_origin, master_url, sizeof(base_origin) - 1);
        }
    }

    const char* last_slash = strrchr(master_url, '/');
    if (last_slash) {
        size_t d_len = last_slash - master_url;
        if (d_len >= sizeof(base_dir)) d_len = sizeof(base_dir) - 1;
        strncpy(base_dir, master_url, d_len);
        base_dir[d_len] = 0;
    }

    out[0] = 0;
    size_t out_len = 0;
    char line[4096];
    const char* p = in;

    while (*p) {
        size_t line_len = 0;
        while (*p && *p != '\n' && *p != '\r' && line_len < sizeof(line) - 1) {
            line[line_len++] = *p++;
        }
        line[line_len] = 0;
        while (*p == '\r' || *p == '\n') p++;

        if (line[0] != 0 && line[0] != '#') {
            // Variant stream URL (absolute or relative)
            char abs_sub[2048];
            if (strncmp(line, "http", 4) == 0) {
                snprintf(abs_sub, sizeof(abs_sub), "%s", line);
            } else if (line[0] == '/') {
                snprintf(abs_sub, sizeof(abs_sub), "%s%s", base_origin, line);
            } else {
                snprintf(abs_sub, sizeof(abs_sub), "%s/%s", base_dir, line);
            }

            char enc[4096];
            urlencode(abs_sub, enc, sizeof(enc));
            char rep[5120];
            snprintf(rep, sizeof(rep), "%s/api/stream/sub.m3u8?url=%s\n", HOST_PREFIX, enc);
            size_t rlen = strlen(rep);
            if (out_len + rlen < out_max) {
                memcpy(out + out_len, rep, rlen);
                out_len += rlen;
            }
        } else if (strstr(line, "URI=\"") || strstr(line, "URI='")) {
            // Audio or subtitle track URI
            char quote_char = strstr(line, "URI=\"") ? '"' : '\'';
            char* u_start = quote_char == '"' ? (strstr(line, "URI=\"") + 5) : (strstr(line, "URI='") + 5);
            char* u_end = strchr(u_start, quote_char);
            if (u_end) {
                char target[2048] = {0};
                size_t tlen = u_end - u_start;
                if (tlen < sizeof(target)) {
                    strncpy(target, u_start, tlen);
                    target[tlen] = 0;
                }

                char abs_target[2048];
                if (strncmp(target, "http", 4) == 0) {
                    snprintf(abs_target, sizeof(abs_target), "%s", target);
                } else if (target[0] == '/') {
                    snprintf(abs_target, sizeof(abs_target), "%s%s", base_origin, target);
                } else {
                    snprintf(abs_target, sizeof(abs_target), "%s/%s", base_dir, target);
                }

                char enc[4096];
                urlencode(abs_target, enc, sizeof(enc));
                char before[1024];
                size_t b_len = u_start - line;
                if (b_len >= sizeof(before)) b_len = sizeof(before) - 1;
                strncpy(before, line, b_len);
                before[b_len] = 0;

                char rep[6144];
                snprintf(rep, sizeof(rep), "%s%s/api/stream/sub.m3u8?url=%s%s\n",
                    before, HOST_PREFIX, enc, u_end);
                size_t rlen = strlen(rep);
                if (out_len + rlen < out_max) {
                    memcpy(out + out_len, rep, rlen);
                    out_len += rlen;
                }
            } else {
                size_t llen = strlen(line);
                if (out_len + llen + 1 < out_max) {
                    memcpy(out + out_len, line, llen);
                    out_len += llen;
                    out[out_len++] = '\n';
                }
            }
        } else {
            size_t llen = strlen(line);
            if (out_len + llen + 1 < out_max) {
                memcpy(out + out_len, line, llen);
                out_len += llen;
                out[out_len++] = '\n';
            }
        }
    }
    out[out_len] = 0;
}

static void rewrite_sub_playlist(const char* in, const char* sub_url, char* out, size_t out_max) {
    char base_origin[512] = {0};
    char base_dir[1024] = {0};

    const char* colon = strstr(sub_url, "://");
    if (colon) {
        const char* slash = strchr(colon + 3, '/');
        if (slash) {
            size_t o_len = slash - sub_url;
            if (o_len >= sizeof(base_origin)) o_len = sizeof(base_origin) - 1;
            strncpy(base_origin, sub_url, o_len);
            base_origin[o_len] = 0;
        } else {
            strncpy(base_origin, sub_url, sizeof(base_origin) - 1);
        }
    }

    const char* last_slash = strrchr(sub_url, '/');
    if (last_slash) {
        size_t d_len = last_slash - sub_url;
        if (d_len >= sizeof(base_dir)) d_len = sizeof(base_dir) - 1;
        strncpy(base_dir, sub_url, d_len);
        base_dir[d_len] = 0;
    }

    out[0] = 0;
    size_t out_len = 0;
    char line[4096];
    const char* p = in;

    while (*p) {
        size_t line_len = 0;
        while (*p && *p != '\n' && *p != '\r' && line_len < sizeof(line) - 1) {
            line[line_len++] = *p++;
        }
        line[line_len] = 0;
        while (*p == '\r' || *p == '\n') p++;

        // 1. Rewrite AES-128 Encryption Key URI
        if (strstr(line, "#EXT-X-KEY:") && strstr(line, "AES-128")) {
            char key_url[512];
            snprintf(key_url, sizeof(key_url), "%s/api/stream/key", HOST_PREFIX);
            char rep[2048] = {0};
            char* u1 = strstr(line, "URI=\"");
            char* u2 = strstr(line, "URI='");
            if (u1) {
                char* qend = strchr(u1 + 5, '"');
                if (qend) {
                    size_t b_len = (u1 + 5) - line;
                    char before[1024];
                    if (b_len >= sizeof(before)) b_len = sizeof(before) - 1;
                    strncpy(before, line, b_len);
                    before[b_len] = 0;
                    snprintf(rep, sizeof(rep), "%s%s%s\n", before, key_url, qend);
                }
            } else if (u2) {
                char* qend = strchr(u2 + 5, '\'');
                if (qend) {
                    size_t b_len = (u2 + 5) - line;
                    char before[1024];
                    if (b_len >= sizeof(before)) b_len = sizeof(before) - 1;
                    strncpy(before, line, b_len);
                    before[b_len] = 0;
                    snprintf(rep, sizeof(rep), "%s%s%s\n", before, key_url, qend);
                }
            }
            if (!rep[0]) {
                snprintf(rep, sizeof(rep), "%s\n", line);
            }
            size_t rlen = strlen(rep);
            if (out_len + rlen < out_max) {
                memcpy(out + out_len, rep, rlen);
                out_len += rlen;
            }
        }
        // 2. Rewrite #EXT-X-MAP (fMP4 init segment map)
        else if (strstr(line, "#EXT-X-MAP:") && (strstr(line, "URI=\"") || strstr(line, "URI='"))) {
            char quote_char = strstr(line, "URI=\"") ? '"' : '\'';
            char* u_start = quote_char == '"' ? (strstr(line, "URI=\"") + 5) : (strstr(line, "URI='") + 5);
            char* u_end = strchr(u_start, quote_char);
            if (u_end) {
                char map_path[1024] = {0};
                size_t mlen = u_end - u_start;
                if (mlen < sizeof(map_path)) {
                    strncpy(map_path, u_start, mlen);
                    map_path[mlen] = 0;
                }
                char abs_map[2048];
                if (strncmp(map_path, "http", 4) == 0) {
                    snprintf(abs_map, sizeof(abs_map), "%s", map_path);
                } else if (map_path[0] == '/') {
                    snprintf(abs_map, sizeof(abs_map), "%s%s", base_origin, map_path);
                } else {
                    snprintf(abs_map, sizeof(abs_map), "%s/%s", base_dir, map_path);
                }
                char rep[3072];
                size_t b_len = u_start - line;
                char before[1024];
                if (b_len >= sizeof(before)) b_len = sizeof(before) - 1;
                strncpy(before, line, b_len);
                before[b_len] = 0;
                snprintf(rep, sizeof(rep), "%s%s%s\n", before, abs_map, u_end);
                size_t rlen = strlen(rep);
                if (out_len + rlen < out_max) {
                    memcpy(out + out_len, rep, rlen);
                    out_len += rlen;
                }
            } else {
                size_t llen = strlen(line);
                if (out_len + llen + 1 < out_max) {
                    memcpy(out + out_len, line, llen);
                    out_len += llen;
                    out[out_len++] = '\n';
                }
            }
        }
        // 3. Rewrite relative media segment lines
        else if (line[0] != 0 && line[0] != '#' && strncmp(line, "http", 4) != 0) {
            char rep[4096];
            if (line[0] == '/') {
                snprintf(rep, sizeof(rep), "%s%s\n", base_origin, line);
            } else {
                snprintf(rep, sizeof(rep), "%s/%s\n", base_dir, line);
            }
            size_t rlen = strlen(rep);
            if (out_len + rlen < out_max) {
                memcpy(out + out_len, rep, rlen);
                out_len += rlen;
            }
        }
        // 4. Leave tags and absolute URLs unchanged
        else {
            size_t llen = strlen(line);
            if (out_len + llen + 1 < out_max) {
                memcpy(out + out_len, line, llen);
                out_len += llen;
                out[out_len++] = '\n';
            }
        }
    }
    out[out_len] = 0;
}

static void send_cors_response(int client_fd, int status_code, const char* content_type, const char* body) {
    char header[1024];
    size_t body_len = body ? strlen(body) : 0;
    const char* status_text = (status_code == 200) ? "OK" : (status_code == 404) ? "Not Found" : "Internal Server Error";

    snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: *\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        status_code, status_text, content_type, body_len
    );

    send(client_fd, header, strlen(header), 0);
    if (body_len > 0) {
        send(client_fd, body, body_len, 0);
    }
}

static const char* get_mime_type(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(dot, ".css") == 0) return "text/css; charset=utf-8";
    if (strcmp(dot, ".js") == 0) return "application/javascript; charset=utf-8";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".png") == 0) return "image/png";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, ".svg") == 0) return "image/svg+xml";
    return "application/octet-stream";
}

static void send_file_response(int client_fd, const char* filepath) {
    FILE* f = fopen(filepath, "rb");
    if (!f) {
        send_cors_response(client_fd, 404, "text/plain", "File Not Found");
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    const char* mime = get_mime_type(filepath);
    char header[512];
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n",
        mime, sz
    );
    send(client_fd, header, strlen(header), 0);

    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        send(client_fd, buf, n, 0);
    }
    fclose(f);
}

static void* client_worker(void* arg) {
    int client_fd = (int)(intptr_t)arg;
    char req_buf[4096];
    int n = recv(client_fd, req_buf, sizeof(req_buf) - 1, 0);
    if (n <= 0) {
        close(client_fd);
        return NULL;
    }
    req_buf[n] = 0;

    // Handle OPTIONS CORS preflight
    if (strncmp(req_buf, "OPTIONS ", 8) == 0) {
        send_cors_response(client_fd, 200, "text/plain", "");
        close(client_fd);
        return NULL;
    }

    // Parse GET request path
    if (strncmp(req_buf, "GET ", 4) != 0) {
        send_cors_response(client_fd, 405, "text/plain", "Method Not Allowed");
        close(client_fd);
        return NULL;
    }

    char* path_start = req_buf + 4;
    char* path_end = strchr(path_start, ' ');
    if (!path_end) {
        close(client_fd);
        return NULL;
    }
    *path_end = 0;

    char klog_buf[512];
    snprintf(klog_buf, sizeof(klog_buf), "[PStream Service] HTTP %s", path_start);
    klog_puts(klog_buf);

    // 1. Health check
    if (strcmp(path_start, "/health") == 0) {
        send_cors_response(client_fd, 200, "application/json", "{\"status\":\"ok\",\"service\":\"PStream Native PS5 Engine\"}");
        close(client_fd);
        return NULL;
    }

    // 1b. Shutdown endpoint
    if (strcmp(path_start, "/shutdown") == 0) {
        send_cors_response(client_fd, 200, "text/plain", "Server shutting down");
        close(client_fd);
        exit(0);
        return NULL;
    }

    // 2. WebKit Logging endpoint
    if (strncmp(path_start, "/log", 4) == 0) {
        char* q = strstr(path_start, "msg=");
        if (q) {
            char decoded[LOG_LINE_LEN];
            urldecode(q + 4, decoded, sizeof(decoded));
            add_log(decoded);
        }
        send_cors_response(client_fd, 200, "text/plain", "OK");
        close(client_fd);
        return NULL;
    }

    // 2b. View Logs API (/api/logs)
    if (strcmp(path_start, "/api/logs") == 0) {
        pthread_mutex_lock(&g_log_mutex);
        char* big_logs = malloc(65536);
        big_logs[0] = 0;
        int count = g_log_count < MAX_LOG_LINES ? g_log_count : MAX_LOG_LINES;
        int start_idx = g_log_count < MAX_LOG_LINES ? 0 : (g_log_count % MAX_LOG_LINES);
        for (int i = 0; i < count; i++) {
            int idx = (start_idx + i) % MAX_LOG_LINES;
            strncat(big_logs, g_log_lines[idx], 65536 - strlen(big_logs) - 2);
            strcat(big_logs, "\n");
        }
        pthread_mutex_unlock(&g_log_mutex);
        send_cors_response(client_fd, 200, "text/plain", big_logs);
        free(big_logs);
        close(client_fd);
        return NULL;
    }

    // 3. Stream Resolver Endpoint
    if (strncmp(path_start, "/api/stream/resolve", 19) == 0) {
        char id[64] = {0};
        char type[16] = "movie";
        int season = 1;
        int episode = 1;
        char lang[16] = "en";

        char* q = strchr(path_start, '?');
        if (q) {
            q++;
            char query_copy[1024];
            strncpy(query_copy, q, sizeof(query_copy) - 1);
            query_copy[sizeof(query_copy) - 1] = 0;

            char* token = strtok(query_copy, "&");
            while (token) {
                if (strncmp(token, "id=", 3) == 0) {
                    strncpy(id, token + 3, sizeof(id) - 1);
                } else if (strncmp(token, "type=", 5) == 0) {
                    strncpy(type, token + 5, sizeof(type) - 1);
                } else if (strncmp(token, "season=", 7) == 0) {
                    season = atoi(token + 7);
                } else if (strncmp(token, "episode=", 8) == 0) {
                    episode = atoi(token + 8);
                } else if (strncmp(token, "lang=", 5) == 0) {
                    strncpy(lang, token + 5, sizeof(lang) - 1);
                } else if (strncmp(token, "provider=", 9) == 0) {
                    strncpy(lang, token + 9, sizeof(lang) - 1);
                }
                token = strtok(NULL, "&");
            }
        }

        if (id[0]) {
            char raw_stream_url[2048] = {0};
            if (extract_vix_stream(id, type, season, episode, lang, raw_stream_url, sizeof(raw_stream_url)) == 0) {
                char enc_url[4096];
                urlencode(raw_stream_url, enc_url, sizeof(enc_url));
                char res_json[5120];
                snprintf(res_json, sizeof(res_json),
                    "{\"status\":\"ok\",\"streamUrl\":\"%s/api/stream/master.m3u8?url=%s\",\"type\":\"hls\"}",
                    HOST_PREFIX, enc_url
                );
                send_cors_response(client_fd, 200, "application/json", res_json);
                close(client_fd);
                return NULL;
            }
        }

        send_cors_response(client_fd, 500, "application/json", "{\"status\":\"error\",\"error\":\"Stream resolution failed\"}");
        close(client_fd);
        return NULL;
    }

    // 4. Master Playlist Proxy (/api/stream/master.m3u8?url=...)
    if (strncmp(path_start, "/api/stream/master.m3u8", 23) == 0) {
        char* q = strstr(path_start, "url=");
        if (!q) {
            send_cors_response(client_fd, 400, "text/plain", "Missing url parameter");
            close(client_fd);
            return NULL;
        }
        char target_url[2048];
        urldecode(q + 4, target_url, sizeof(target_url));

        char* raw_master = malloc(512 * 1024);
        char* rewritten_master = malloc(1024 * 1024);
        if (!raw_master || !rewritten_master) {
            if (raw_master) free(raw_master);
            if (rewritten_master) free(rewritten_master);
            send_cors_response(client_fd, 500, "text/plain", "Memory allocation failed");
            close(client_fd);
            return NULL;
        }

        int m_len = http_fetch(target_url, "https://vixsrc.to/", raw_master, 512 * 1024);
        if (m_len <= 0) {
            free(raw_master);
            free(rewritten_master);
            send_cors_response(client_fd, 502, "text/plain", "Failed to fetch master playlist");
            close(client_fd);
            return NULL;
        }

        rewrite_master_playlist(raw_master, target_url, rewritten_master, 1024 * 1024);
        free(raw_master);

        send_cors_response(client_fd, 200, "application/vnd.apple.mpegurl; charset=utf-8", rewritten_master);
        free(rewritten_master);
        close(client_fd);
        return NULL;
    }

    // 5. Sub-Playlist Proxy & Key Rewriter (/api/stream/sub.m3u8?url=...)
    if (strncmp(path_start, "/api/stream/sub.m3u8", 20) == 0) {
        char* q = strstr(path_start, "url=");
        if (!q) {
            send_cors_response(client_fd, 400, "text/plain", "Missing url parameter");
            close(client_fd);
            return NULL;
        }
        char target_url[2048];
        urldecode(q + 4, target_url, sizeof(target_url));

        char* raw_sub = malloc(2 * 1024 * 1024);
        char* rewritten_sub = malloc(4 * 1024 * 1024);
        if (!raw_sub || !rewritten_sub) {
            if (raw_sub) free(raw_sub);
            if (rewritten_sub) free(rewritten_sub);
            send_cors_response(client_fd, 500, "text/plain", "Memory allocation failed");
            close(client_fd);
            return NULL;
        }

        int s_len = http_fetch(target_url, "https://vixsrc.to/", raw_sub, 2 * 1024 * 1024);
        if (s_len <= 0) {
            free(raw_sub);
            free(rewritten_sub);
            send_cors_response(client_fd, 502, "text/plain", "Failed to fetch sub playlist");
            close(client_fd);
            return NULL;
        }

        rewrite_sub_playlist(raw_sub, target_url, rewritten_sub, 4 * 1024 * 1024);
        free(raw_sub);

        send_cors_response(client_fd, 200, "application/vnd.apple.mpegurl; charset=utf-8", rewritten_sub);
        free(rewritten_sub);
        close(client_fd);
        return NULL;
    }

    // 6. AES-128 Key Proxy (/api/stream/key) - Instant In-Memory Cache Response
    if (strncmp(path_start, "/api/stream/key", 15) == 0) {
        int k_len = ensure_enc_key();
        if (k_len >= 16) {
            char header[512];
            snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
                "Access-Control-Allow-Headers: *\r\n"
                "Content-Type: application/octet-stream\r\n"
                "Content-Length: %d\r\n"
                "Connection: close\r\n\r\n",
                k_len
            );
            send(client_fd, header, strlen(header), 0);
            send(client_fd, (char*)g_enc_key, k_len, 0);
        } else {
            send_cors_response(client_fd, 502, "text/plain", "Failed to fetch enc.key");
        }
        close(client_fd);
        return NULL;
    }

    // 7. Check Update Endpoint (/api/check_update)
    if (strncmp(path_start, "/api/check_update", 17) == 0) {
        char remote_ver_buf[2048];
        memset(remote_ver_buf, 0, sizeof(remote_ver_buf));
        int r_len = http_fetch("https://raw.githubusercontent.com/ducky-ux1/PStream/main/ps5/www/version.json", "https://github.com", remote_ver_buf, sizeof(remote_ver_buf) - 1);
        if (r_len <= 0) {
            r_len = http_fetch("https://raw.githubusercontent.com/ducky-ux1/PStream/main/github/ps5/www/version.json", "https://github.com", remote_ver_buf, sizeof(remote_ver_buf) - 1);
        }
        
        char local_ver[64] = "1.0.0";
        FILE* vf = fopen("/user/app/PSTR00001/version.json", "r");
        if (vf) {
            char l_buf[2048];
            size_t n = fread(l_buf, 1, sizeof(l_buf) - 1, vf);
            l_buf[n] = 0;
            fclose(vf);
            char* v_pos = strstr(l_buf, "\"version\":");
            if (v_pos) {
                char* q1 = strchr(v_pos + 10, '\"');
                if (q1) {
                    char* q2 = strchr(q1 + 1, '\"');
                    if (q2 && (q2 - q1 - 1) < sizeof(local_ver)) {
                        strncpy(local_ver, q1 + 1, q2 - q1 - 1);
                        local_ver[q2 - q1 - 1] = 0;
                    }
                }
            }
        }

        if (r_len > 0) {
            char remote_ver[64] = "";
            char* v_pos = strstr(remote_ver_buf, "\"version\":");
            if (v_pos) {
                char* q1 = strchr(v_pos + 10, '\"');
                if (q1) {
                    char* q2 = strchr(q1 + 1, '\"');
                    if (q2 && (q2 - q1 - 1) < sizeof(remote_ver)) {
                        strncpy(remote_ver, q1 + 1, q2 - q1 - 1);
                        remote_ver[q2 - q1 - 1] = 0;
                    }
                }
            }

            int has_update = (remote_ver[0] && strcmp(remote_ver, local_ver) != 0);
            char resp[1024];
            snprintf(resp, sizeof(resp),
                "{\"update_available\": %s, \"current_version\": \"%s\", \"latest_version\": \"%s\"}",
                has_update ? "true" : "false", local_ver, remote_ver[0] ? remote_ver : local_ver);
            send_cors_response(client_fd, 200, "application/json", resp);
        } else {
            char resp[512];
            snprintf(resp, sizeof(resp),
                "{\"update_available\": false, \"current_version\": \"%s\", \"status\": \"offline\"}", local_ver);
            send_cors_response(client_fd, 200, "application/json", resp);
        }
        close(client_fd);
        return NULL;
    }

    // 8. Apply Update Endpoint (/api/apply_update)
    if (strncmp(path_start, "/api/apply_update", 17) == 0) {
        printf("[PStream Updater] Downloading latest updates from GitHub...\n");
        const char* files_to_update[] = { "app.js", "app.css", "index.html", "input.js", "version.json" };
        int num_files = sizeof(files_to_update) / sizeof(files_to_update[0]);
        int updated_count = 0;

        for (int i = 0; i < num_files; i++) {
            char gh_url[512];
            snprintf(gh_url, sizeof(gh_url), "https://raw.githubusercontent.com/ducky-ux1/PStream/main/ps5/www/%s", files_to_update[i]);
            
            char* file_buf = malloc(512 * 1024);
            if (!file_buf) continue;

            int dl_len = http_fetch(gh_url, "https://github.com", file_buf, (512 * 1024) - 1);
            if (dl_len <= 0) {
                // Fallback to github/ps5/www/ path
                snprintf(gh_url, sizeof(gh_url), "https://raw.githubusercontent.com/ducky-ux1/PStream/main/github/ps5/www/%s", files_to_update[i]);
                dl_len = http_fetch(gh_url, "https://github.com", file_buf, (512 * 1024) - 1);
            }

            if (dl_len > 0) {
                char dst_path[512];
                snprintf(dst_path, sizeof(dst_path), "/user/app/PSTR00001/%s", files_to_update[i]);
                FILE* df = fopen(dst_path, "wb");
                if (df) {
                    fwrite(file_buf, 1, dl_len, df);
                    fclose(df);
                    updated_count++;
                    printf("[PStream Updater] Updated %s (%d bytes)\n", files_to_update[i], dl_len);
                }
                // Mirror to system_ex if accessible
                snprintf(dst_path, sizeof(dst_path), "/system_ex/app/PSTR00001/%s", files_to_update[i]);
                FILE* sf = fopen(dst_path, "wb");
                if (sf) {
                    fwrite(file_buf, 1, dl_len, sf);
                    fclose(sf);
                }
            }
            free(file_buf);
        }

        char resp[512];
        snprintf(resp, sizeof(resp),
            "{\"success\": %s, \"updated_files\": %d, \"message\": \"%s\"}",
            (updated_count > 0) ? "true" : "false", updated_count,
            (updated_count > 0) ? "Update applied successfully from GitHub!" : "Failed to download updates from GitHub.");
        send_cors_response(client_fd, 200, "application/json", resp);
        close(client_fd);
        return NULL;
    }

    // 9. Static File Serving from /user/app/PSTR00001
    char full_filepath[1024];
    if (strcmp(path_start, "/") == 0 || strcmp(path_start, "/index.html") == 0) {
        snprintf(full_filepath, sizeof(full_filepath), "%s/index.html", APP_DIR);
    } else {
        const char* rel = (path_start[0] == '/') ? path_start + 1 : path_start;
        char clean_rel[512];
        strncpy(clean_rel, rel, sizeof(clean_rel) - 1);
        clean_rel[sizeof(clean_rel) - 1] = 0;
        char* qmark = strchr(clean_rel, '?');
        if (qmark) *qmark = 0;
        char* ampersand = strchr(clean_rel, '&');
        if (ampersand) *ampersand = 0;

        snprintf(full_filepath, sizeof(full_filepath), "%s/%s", APP_DIR, clean_rel);
    }

    send_file_response(client_fd, full_filepath);
    close(client_fd);
    return NULL;
}

static void* precache_key_thread(void* arg) {
    ensure_enc_key();
    return NULL;
}

int main(int argc, char* argv[]) {
    printf("[PStream Native Engine] Initializing standalone background server on port %d...\n", SERVER_PORT);

    if (init_net_subsystem() < 0) {
        printf("[PStream Native Engine] Failed to initialize networking.\n");
        return -1;
    }

    // Pre-cache AES encryption key immediately in background
    pthread_t k_th;
    pthread_create(&k_th, NULL, precache_key_thread, NULL);
    pthread_detach(k_th);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(SERVER_PORT);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        printf("[PStream Native Engine] Port %d already in use. Service is already active!\n", SERVER_PORT);
        close(server_fd);
        return 0;
    }

    if (listen(server_fd, 64) < 0) {
        perror("listen");
        close(server_fd);
        return -1;
    }

    printf("[PStream Native Engine] Listening on 0.0.0.0:%d - Standalone mode ACTIVE!\n", SERVER_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd >= 0) {
            pthread_t thread;
            pthread_create(&thread, NULL, client_worker, (void*)(intptr_t)client_fd);
            pthread_detach(thread);
        }
    }

    close(server_fd);
    return 0;
}
