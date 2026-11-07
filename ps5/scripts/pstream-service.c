#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <ps5/klog.h>

#define SERVER_PORT 8086
#define APP_DIR "/user/app/PSTR00001"
#define HOST_PREFIX "http://127.0.0.1:8086"
#define KEY_TTL_SECONDS 600

#define MAX_LOG_LINES 250
#define LOG_LINE_LEN 256

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

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
int sceHttp2ReadData(int, void*, size_t);
int sceHttp2SetSslCallback(int, void*, void*);
int sceHttp2SslDisableOption(int, unsigned int);
int sceHttp2AddRequestHeader(int, const char*, const char*, int);
int sceHttp2SetResolveTimeOut(int, unsigned int);
int sceHttp2SetConnectTimeOut(int, unsigned int);
int sceHttp2SetSendTimeOut(int, unsigned int);
int sceHttp2SetRecvTimeOut(int, unsigned int);

static int g_netMemId = -1;
static int g_sslCtxId = -1;
static int g_httpCtxId = -1;
static uint64_t g_http_gen = 0;
static pthread_rwlock_t g_http_rwlock = PTHREAD_RWLOCK_INITIALIZER;

// Dynamic AES-128 Key Cache with 10-minute TTL & URL tracking
static uint8_t g_enc_key[64];
static int g_enc_key_len = 0;
static char g_enc_key_url[1024] = {0};
static time_t g_enc_key_time = 0;
static pthread_mutex_t g_key_mutex = PTHREAD_MUTEX_INITIALIZER;

// Circular In-Memory Log Buffer (Dumpable via /api/logs)
static char g_log_lines[MAX_LOG_LINES][LOG_LINE_LEN];
static int g_log_count = 0;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char* USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36";

static void add_log(const char* msg) {
    if (!msg || !msg[0]) return;
    pthread_mutex_lock(&g_log_mutex);
    snprintf(g_log_lines[g_log_count % MAX_LOG_LINES], LOG_LINE_LEN, "%s", msg);
    g_log_count++;
    pthread_mutex_unlock(&g_log_mutex);
    printf("[PStream WebKit Log] %s\n", msg);
}

static int send_all(int fd, const void* data, size_t len) {
    const uint8_t* ptr = (const uint8_t*)data;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, ptr + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static int ssl_cb(int sslId, unsigned int verifyErr, void* const cert[], int certNum, void* userArg) {
    (void)sslId;
    (void)verifyErr;
    (void)cert;
    (void)certNum;
    (void)userArg;
    return 1; // Accept upstream CDN certificates regardless of system CA age
}

static int init_net_subsystem(void) {
    int rc = sceNetInit();
    if (rc < 0 && rc != (int)0x80410102) { // ignore already-initialized
        printf("[PStream Service] sceNetInit returned 0x%08X\n", rc);
    }
    g_netMemId = sceNetPoolCreate("pstream_net", 8 * 1024 * 1024, 0);
    g_sslCtxId = sceSslInit(4 * 1024 * 1024);
    g_httpCtxId = sceHttp2Init(g_netMemId, g_sslCtxId, 4 * 1024 * 1024, 16);
    g_http_gen = 1;
    printf("[PStream Service] Net & SSL Subsystems Initialized (httpCtx=%d)\n", g_httpCtxId);
    return (g_httpCtxId >= 0) ? 0 : -1;
}

// Re-initialize sceHttp2 & sceSsl contexts after PS5 Rest Mode (suspend/resume)
static void reinit_http_subsystem(uint64_t failed_gen) {
    pthread_rwlock_wrlock(&g_http_rwlock);
    if (g_http_gen == failed_gen) {
        printf("[PStream Service] Re-initializing HTTP/SSL context after transport error (gen %lu)...\n",
               (unsigned long)failed_gen);
        if (g_httpCtxId >= 0) {
            sceHttp2Term(g_httpCtxId);
            g_httpCtxId = -1;
        }
        if (g_sslCtxId >= 0) {
            sceSslTerm(g_sslCtxId);
            g_sslCtxId = -1;
        }
        if (g_netMemId < 0) {
            g_netMemId = sceNetPoolCreate("pstream_net", 8 * 1024 * 1024, 0);
        }
        g_sslCtxId = sceSslInit(4 * 1024 * 1024);
        g_httpCtxId = sceHttp2Init(g_netMemId, g_sslCtxId, 4 * 1024 * 1024, 16);
        g_http_gen++;
        printf("[PStream Service] HTTP/SSL context restored (httpCtx=%d, gen=%lu)\n",
               g_httpCtxId, (unsigned long)g_http_gen);
    }
    pthread_rwlock_unlock(&g_http_rwlock);
}

static int configure_http_request(const char* url, const char* referer, int* out_tmpl, int* out_req) {
    int tmpl = sceHttp2CreateTemplate(g_httpCtxId, USER_AGENT, 3, 1);
    if (tmpl < 0) return -1;

    sceHttp2SetSslCallback(tmpl, (void*)ssl_cb, 0);
    sceHttp2SslDisableOption(tmpl, 0xFFFFFFFF);
    sceHttp2SetResolveTimeOut(tmpl, 8 * 1000 * 1000);
    sceHttp2SetConnectTimeOut(tmpl, 10 * 1000 * 1000);
    sceHttp2SetSendTimeOut(tmpl, 10 * 1000 * 1000);
    sceHttp2SetRecvTimeOut(tmpl, 20 * 1000 * 1000);

    int req = sceHttp2CreateRequestWithURL(tmpl, "GET", url, 0);
    if (req < 0) {
        sceHttp2DeleteTemplate(tmpl);
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

    *out_tmpl = tmpl;
    *out_req = req;
    return 0;
}

static int http_fetch(const char* url, const char* referer, char* out_buf, size_t out_max) {
    if (!url || !out_buf || out_max < 2) return -1;

    for (int attempt = 0; attempt < 2; attempt++) {
        pthread_rwlock_rdlock(&g_http_rwlock);
        uint64_t cur_gen = g_http_gen;

        int tmpl = -1, req = -1;
        if (configure_http_request(url, referer, &tmpl, &req) < 0) {
            pthread_rwlock_unlock(&g_http_rwlock);
            if (attempt == 0) {
                reinit_http_subsystem(cur_gen);
                continue;
            }
            return -1;
        }

        int ret = sceHttp2SendRequest(req, NULL, 0);
        if (ret < 0) {
            printf("[http_fetch] send error 0x%08X for %s (attempt %d)\n", ret, url, attempt + 1);
            sceHttp2DeleteRequest(req);
            sceHttp2DeleteTemplate(tmpl);
            pthread_rwlock_unlock(&g_http_rwlock);
            if (attempt == 0) {
                reinit_http_subsystem(cur_gen);
                continue;
            }
            return -1;
        }

        int status = 0;
        sceHttp2GetStatusCode(req, &status);
        if (status < 200 || status >= 300) {
            printf("[http_fetch] HTTP status %d for %s\n", status, url);
            sceHttp2DeleteRequest(req);
            sceHttp2DeleteTemplate(tmpl);
            pthread_rwlock_unlock(&g_http_rwlock);
            return -1;
        }

        size_t total = 0;
        while (total < out_max - 1) {
            int n = sceHttp2ReadData(req, out_buf + total, out_max - 1 - total);
            if (n <= 0) break;
            total += (size_t)n;
        }
        out_buf[total] = 0;

        sceHttp2DeleteRequest(req);
        sceHttp2DeleteTemplate(tmpl);
        pthread_rwlock_unlock(&g_http_rwlock);
        return (int)total;
    }
    return -1;
}

static int http_stream_to_client(int client_fd, const char* url, const char* referer,
                                 const char* content_type, const char* cache_ctrl) {
    for (int attempt = 0; attempt < 2; attempt++) {
        pthread_rwlock_rdlock(&g_http_rwlock);
        uint64_t cur_gen = g_http_gen;

        int tmpl = -1, req = -1;
        if (configure_http_request(url, referer, &tmpl, &req) < 0) {
            pthread_rwlock_unlock(&g_http_rwlock);
            if (attempt == 0) {
                reinit_http_subsystem(cur_gen);
                continue;
            }
            return -1;
        }

        int ret = sceHttp2SendRequest(req, NULL, 0);
        if (ret < 0) {
            sceHttp2DeleteRequest(req);
            sceHttp2DeleteTemplate(tmpl);
            pthread_rwlock_unlock(&g_http_rwlock);
            if (attempt == 0) {
                reinit_http_subsystem(cur_gen);
                continue;
            }
            return -1;
        }

        int status = 0;
        sceHttp2GetStatusCode(req, &status);
        if (status < 200 || status >= 300) {
            sceHttp2DeleteRequest(req);
            sceHttp2DeleteTemplate(tmpl);
            pthread_rwlock_unlock(&g_http_rwlock);
            return -1;
        }

        char header[512];
        snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
            "Access-Control-Allow-Headers: *\r\n"
            "Content-Type: %s\r\n"
            "Cache-Control: %s\r\n"
            "Connection: close\r\n\r\n",
            content_type, cache_ctrl ? cache_ctrl : "no-cache"
        );

        if (send_all(client_fd, header, strlen(header)) < 0) {
            sceHttp2DeleteRequest(req);
            sceHttp2DeleteTemplate(tmpl);
            pthread_rwlock_unlock(&g_http_rwlock);
            return 0;
        }

        char* chunk = malloc(65536);
        if (chunk) {
            while (1) {
                int n = sceHttp2ReadData(req, chunk, 65536);
                if (n <= 0) break;
                if (send_all(client_fd, chunk, (size_t)n) < 0) break;
            }
            free(chunk);
        }

        sceHttp2DeleteRequest(req);
        sceHttp2DeleteTemplate(tmpl);
        pthread_rwlock_unlock(&g_http_rwlock);
        return 0;
    }
    return -1;
}

static void urlencode(const char* src, char* dst, size_t dst_max) {
    static const char* hex = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 4 < dst_max; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            dst[j++] = (char)c;
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
    while (src[i] && j + 1 < dst_max) {
        if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            char hex[3] = {src[i + 1], src[i + 2], 0};
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

static int get_query_param(const char* path, const char* key, char* out, size_t out_max) {
    const char* q = strchr(path, '?');
    if (!q) return 0;
    q++;
    size_t klen = strlen(key);
    while (*q) {
        const char* amp = strchr(q, '&');
        size_t seg_len = amp ? (size_t)(amp - q) : strlen(q);
        if (seg_len > klen + 1 && strncmp(q, key, klen) == 0 && q[klen] == '=') {
            char raw[4096];
            size_t vlen = seg_len - (klen + 1);
            if (vlen >= sizeof(raw)) vlen = sizeof(raw) - 1;
            memcpy(raw, q + klen + 1, vlen);
            raw[vlen] = 0;
            urldecode(raw, out, out_max);
            return 1;
        }
        if (!amp) break;
        q = amp + 1;
    }
    return 0;
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

static int fetch_enc_key(const char* requested_url, uint8_t* out_key, size_t out_max) {
    const char* target = (requested_url && strncmp(requested_url, "http", 4) == 0)
                             ? requested_url
                             : "https://vixsrc.to/storage/enc.key";
    time_t now = time(NULL);

    pthread_mutex_lock(&g_key_mutex);
    if (g_enc_key_len >= 16 &&
        strcmp(g_enc_key_url, target) == 0 &&
        (now - g_enc_key_time) < KEY_TTL_SECONDS) {
        int copy_len = g_enc_key_len < (int)out_max ? g_enc_key_len : (int)out_max;
        memcpy(out_key, g_enc_key, (size_t)copy_len);
        pthread_mutex_unlock(&g_key_mutex);
        return copy_len;
    }
    pthread_mutex_unlock(&g_key_mutex);

    char tmp[128];
    int len = http_fetch(target, "https://vixsrc.to/", tmp, sizeof(tmp));
    if (len >= 16) {
        pthread_mutex_lock(&g_key_mutex);
        int store_len = len < (int)sizeof(g_enc_key) ? len : (int)sizeof(g_enc_key);
        memcpy(g_enc_key, tmp, (size_t)store_len);
        g_enc_key_len = store_len;
        snprintf(g_enc_key_url, sizeof(g_enc_key_url), "%s", target);
        g_enc_key_time = time(NULL);
        pthread_mutex_unlock(&g_key_mutex);

        int copy_len = store_len < (int)out_max ? store_len : (int)out_max;
        memcpy(out_key, tmp, (size_t)copy_len);
        printf("[PStream] Refreshed AES-128 key (%d bytes) from %s\n", store_len, target);
        return copy_len;
    }
    return -1;
}

static int extract_vix_stream_lang(const char* id, const char* type, int season, int episode,
                                   const char* lang, char* out_url, size_t out_url_max) {
    char* buf1 = malloc(16384);
    char* buf2 = malloc(131072);
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

    printf("[PStream] Resolving VixSrc (%s): %s\n", lang, api_url);
    int len1 = http_fetch(api_url, "https://vixsrc.to/", buf1, 16384);
    if (len1 <= 0) {
        free(buf1);
        free(buf2);
        return -1;
    }

    char* src_pos = strstr(buf1, "\"src\":\"");
    if (src_pos) {
        src_pos += 7;
    } else {
        src_pos = strstr(buf1, "\"src\": \"");
        if (src_pos) src_pos += 8;
    }
    if (!src_pos) {
        free(buf1);
        free(buf2);
        return -1;
    }
    char* src_end = strchr(src_pos, '"');
    if (!src_end) {
        free(buf1);
        free(buf2);
        return -1;
    }
    *src_end = 0;

    char clean_src[2048];
    size_t j = 0;
    for (size_t i = 0; src_pos[i] && j + 1 < sizeof(clean_src); i++) {
        if (src_pos[i] == '\\' && src_pos[i + 1] == '/') continue;
        clean_src[j++] = src_pos[i];
    }
    clean_src[j] = 0;

    char embed_url[2200];
    if (clean_src[0] == '/') {
        snprintf(embed_url, sizeof(embed_url), "https://vixsrc.to%s", clean_src);
    } else {
        snprintf(embed_url, sizeof(embed_url), "%s", clean_src);
    }

    int len2 = http_fetch(embed_url, "https://vixsrc.to/", buf2, 131072);
    if (len2 <= 0) {
        free(buf1);
        free(buf2);
        return -1;
    }

    char token[256] = {0};
    char expires[256] = {0};
    char base_url[512] = {0};
    int can_fhd = (strstr(buf2, "canPlayFHD = true") != NULL || strstr(buf2, "canPlayFHD=true") != NULL) ? 1 : 0;

    char* t_pos = strstr(buf2, "'token': '");
    if (t_pos) {
        t_pos += 10;
    } else {
        t_pos = strstr(buf2, "\"token\": \"");
        if (t_pos) t_pos += 10;
    }
    if (t_pos) {
        char* t_end = strchr(t_pos, '\'');
        if (!t_end) t_end = strchr(t_pos, '"');
        if (t_end && (size_t)(t_end - t_pos) < sizeof(token)) {
            memcpy(token, t_pos, (size_t)(t_end - t_pos));
            token[t_end - t_pos] = 0;
        }
    }

    char* exp_pos = strstr(buf2, "'expires': '");
    if (exp_pos) {
        exp_pos += 12;
    } else {
        exp_pos = strstr(buf2, "\"expires\": \"");
        if (exp_pos) exp_pos += 12;
    }
    if (exp_pos) {
        char* exp_end = strchr(exp_pos, '\'');
        if (!exp_end) exp_end = strchr(exp_pos, '"');
        if (exp_end && (size_t)(exp_end - exp_pos) < sizeof(expires)) {
            memcpy(expires, exp_pos, (size_t)(exp_end - exp_pos));
            expires[exp_end - exp_pos] = 0;
        }
    }

    char* url_pos = strstr(buf2, "url: '");
    if (url_pos) {
        url_pos += 6;
    } else {
        url_pos = strstr(buf2, "url: \"");
        if (url_pos) url_pos += 6;
    }
    if (url_pos) {
        char* u_end = strchr(url_pos, '\'');
        if (!u_end) u_end = strchr(url_pos, '"');
        if (u_end && (size_t)(u_end - url_pos) < sizeof(base_url)) {
            memcpy(base_url, url_pos, (size_t)(u_end - url_pos));
            base_url[u_end - url_pos] = 0;
        }
    }

    if (!base_url[0]) {
        char* p_pos = strstr(buf2, "/playlist/");
        if (p_pos) {
            char* p_end = strchr(p_pos, '\'');
            if (!p_end) p_end = strchr(p_pos, '"');
            if (p_end) {
                size_t p_len = (size_t)(p_end - p_pos);
                snprintf(base_url, sizeof(base_url), "https://vixsrc.to%.*s", (int)p_len, p_pos);
            }
        }
    }

    free(buf1);
    free(buf2);

    if (token[0] && expires[0] && base_url[0]) {
        char sep = strchr(base_url, '?') ? '&' : '?';
        snprintf(out_url, out_url_max, "%s%ctoken=%s&expires=%s%s&lang=%s",
                 base_url, sep, token, expires, can_fhd ? "&h=1" : "&h=1", lang);
        printf("[PStream] Successfully Resolved Stream:\n>>> %s\n", out_url);
        return 0;
    }

    return -1;
}

static int extract_vix_stream(const char* id, const char* type, int season, int episode,
                              const char* lang_in, char* out_url, size_t out_url_max) {
    const char* lang = map_lang(lang_in);
    if (extract_vix_stream_lang(id, type, season, episode, lang, out_url, out_url_max) == 0) {
        return 0;
    }
    if (strcmp(lang, "en") != 0) {
        printf("[PStream] Language '%s' unavailable for %s, falling back to 'en'...\n", lang, id);
        if (extract_vix_stream_lang(id, type, season, episode, "en", out_url, out_url_max) == 0) {
            return 0;
        }
    }
    return -1;
}

static void extract_origin_and_dir(const char* url, char* base_origin, size_t origin_max,
                                   char* base_dir, size_t dir_max) {
    base_origin[0] = 0;
    base_dir[0] = 0;

    const char* colon = strstr(url, "://");
    if (colon) {
        const char* slash = strchr(colon + 3, '/');
        if (slash) {
            size_t o_len = (size_t)(slash - url);
            if (o_len >= origin_max) o_len = origin_max - 1;
            memcpy(base_origin, url, o_len);
            base_origin[o_len] = 0;
        } else {
            snprintf(base_origin, origin_max, "%s", url);
        }
    }

    const char* q = strchr(url, '?');
    size_t path_end_idx = q ? (size_t)(q - url) : strlen(url);
    const char* last_slash = NULL;
    for (size_t i = 0; i < path_end_idx; i++) {
        if (url[i] == '/') last_slash = url + i;
    }
    if (last_slash && (!colon || last_slash > colon + 2)) {
        size_t d_len = (size_t)(last_slash - url);
        if (d_len >= dir_max) d_len = dir_max - 1;
        memcpy(base_dir, url, d_len);
        base_dir[d_len] = 0;
    } else {
        snprintf(base_dir, dir_max, "%s", base_origin);
    }
}

static void resolve_relative_url(const char* target, const char* base_origin, const char* base_dir,
                                 char* out_abs, size_t out_max) {
    if (strncmp(target, "http://", 7) == 0 || strncmp(target, "https://", 8) == 0) {
        snprintf(out_abs, out_max, "%s", target);
    } else if (target[0] == '/') {
        snprintf(out_abs, out_max, "%s%s", base_origin, target);
    } else {
        snprintf(out_abs, out_max, "%s/%s", base_dir, target);
    }
}

static void rewrite_master_playlist(const char* in, const char* master_url, int segproxy,
                                    char* out, size_t out_max) {
    char base_origin[512];
    char base_dir[1024];
    extract_origin_and_dir(master_url, base_origin, sizeof(base_origin), base_dir, sizeof(base_dir));

    out[0] = 0;
    size_t out_len = 0;
    char line[4096];
    const char* p = in;
    const char* sp_suffix = segproxy ? "&segproxy=1" : "";

    while (*p) {
        size_t line_len = 0;
        while (*p && *p != '\n' && *p != '\r' && line_len < sizeof(line) - 1) {
            line[line_len++] = *p++;
        }
        line[line_len] = 0;
        while (*p == '\r' || *p == '\n') p++;

        if (line[0] != 0 && line[0] != '#') {
            char abs_sub[2048];
            resolve_relative_url(line, base_origin, base_dir, abs_sub, sizeof(abs_sub));

            char enc[4096];
            urlencode(abs_sub, enc, sizeof(enc));
            char rep[5120];
            snprintf(rep, sizeof(rep), "%s/api/stream/sub.m3u8?url=%s%s\n", HOST_PREFIX, enc, sp_suffix);
            size_t rlen = strlen(rep);
            if (out_len + rlen < out_max) {
                memcpy(out + out_len, rep, rlen);
                out_len += rlen;
            }
        } else if (strstr(line, "URI=\"") || strstr(line, "URI='")) {
            char quote_char = strstr(line, "URI=\"") ? '"' : '\'';
            char* u_start = quote_char == '"' ? (strstr(line, "URI=\"") + 5) : (strstr(line, "URI='") + 5);
            char* u_end = strchr(u_start, quote_char);
            if (u_end) {
                char target[2048] = {0};
                size_t tlen = (size_t)(u_end - u_start);
                if (tlen < sizeof(target)) {
                    memcpy(target, u_start, tlen);
                    target[tlen] = 0;
                }

                char abs_target[2048];
                resolve_relative_url(target, base_origin, base_dir, abs_target, sizeof(abs_target));

                char enc[4096];
                urlencode(abs_target, enc, sizeof(enc));
                char before[1024];
                size_t b_len = (size_t)(u_start - line);
                if (b_len >= sizeof(before)) b_len = sizeof(before) - 1;
                memcpy(before, line, b_len);
                before[b_len] = 0;

                char rep[6144];
                snprintf(rep, sizeof(rep), "%s%s/api/stream/sub.m3u8?url=%s%s%s\n",
                         before, HOST_PREFIX, enc, sp_suffix, u_end);
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

static void rewrite_sub_playlist(const char* in, const char* sub_url, int segproxy,
                                 char* out, size_t out_max) {
    char base_origin[512];
    char base_dir[1024];
    extract_origin_and_dir(sub_url, base_origin, sizeof(base_origin), base_dir, sizeof(base_dir));

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

        // 1. Rewrite AES-128 Encryption Key URI preserving actual upstream key URL
        if (strstr(line, "#EXT-X-KEY:") && strstr(line, "AES-128") &&
            (strstr(line, "URI=\"") || strstr(line, "URI='"))) {
            char quote_char = strstr(line, "URI=\"") ? '"' : '\'';
            char* u_start = quote_char == '"' ? (strstr(line, "URI=\"") + 5) : (strstr(line, "URI='") + 5);
            char* u_end = strchr(u_start, quote_char);
            if (u_end) {
                char raw_key[1024] = {0};
                size_t klen = (size_t)(u_end - u_start);
                if (klen < sizeof(raw_key)) {
                    memcpy(raw_key, u_start, klen);
                    raw_key[klen] = 0;
                }
                char abs_key[2048];
                resolve_relative_url(raw_key, base_origin, base_dir, abs_key, sizeof(abs_key));
                char enc_key[3072];
                urlencode(abs_key, enc_key, sizeof(enc_key));

                size_t b_len = (size_t)(u_start - line);
                char before[1024];
                if (b_len >= sizeof(before)) b_len = sizeof(before) - 1;
                memcpy(before, line, b_len);
                before[b_len] = 0;

                char rep[4608];
                snprintf(rep, sizeof(rep), "%s%s/api/stream/key?url=%s%s\n",
                         before, HOST_PREFIX, enc_key, u_end);
                size_t rlen = strlen(rep);
                if (out_len + rlen < out_max) {
                    memcpy(out + out_len, rep, rlen);
                    out_len += rlen;
                }
            }
        }
        // 2. Rewrite #EXT-X-MAP (fMP4 init segment map)
        else if (strstr(line, "#EXT-X-MAP:") && (strstr(line, "URI=\"") || strstr(line, "URI='"))) {
            char quote_char = strstr(line, "URI=\"") ? '"' : '\'';
            char* u_start = quote_char == '"' ? (strstr(line, "URI=\"") + 5) : (strstr(line, "URI='") + 5);
            char* u_end = strchr(u_start, quote_char);
            if (u_end) {
                char map_path[1024] = {0};
                size_t mlen = (size_t)(u_end - u_start);
                if (mlen < sizeof(map_path)) {
                    memcpy(map_path, u_start, mlen);
                    map_path[mlen] = 0;
                }
                char abs_map[2048];
                resolve_relative_url(map_path, base_origin, base_dir, abs_map, sizeof(abs_map));

                size_t b_len = (size_t)(u_start - line);
                char before[1024];
                if (b_len >= sizeof(before)) b_len = sizeof(before) - 1;
                memcpy(before, line, b_len);
                before[b_len] = 0;

                char rep[5120];
                if (segproxy) {
                    char enc_map[3584];
                    urlencode(abs_map, enc_map, sizeof(enc_map));
                    snprintf(rep, sizeof(rep), "%s%s/api/stream/seg.ts?url=%s%s\n",
                             before, HOST_PREFIX, enc_map, u_end);
                } else {
                    snprintf(rep, sizeof(rep), "%s%s%s\n", before, abs_map, u_end);
                }
                size_t rlen = strlen(rep);
                if (out_len + rlen < out_max) {
                    memcpy(out + out_len, rep, rlen);
                    out_len += rlen;
                }
            }
        }
        // 3. Media segment lines
        else if (line[0] != 0 && line[0] != '#') {
            char abs_seg[2048];
            resolve_relative_url(line, base_origin, base_dir, abs_seg, sizeof(abs_seg));

            char rep[5120];
            if (segproxy) {
                char enc_seg[4096];
                urlencode(abs_seg, enc_seg, sizeof(enc_seg));
                snprintf(rep, sizeof(rep), "%s/api/stream/seg.ts?url=%s\n", HOST_PREFIX, enc_seg);
            } else {
                snprintf(rep, sizeof(rep), "%s\n", abs_seg);
            }
            size_t rlen = strlen(rep);
            if (out_len + rlen < out_max) {
                memcpy(out + out_len, rep, rlen);
                out_len += rlen;
            }
        }
        // 4. Other tags
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
    const char* status_text = (status_code == 200) ? "OK" :
                              (status_code == 400) ? "Bad Request" :
                              (status_code == 404) ? "Not Found" :
                              (status_code == 502) ? "Bad Gateway" : "Internal Server Error";

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

    if (send_all(client_fd, header, strlen(header)) == 0 && body_len > 0) {
        send_all(client_fd, body, body_len);
    }
}

static const char* get_mime_type(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(dot, ".css") == 0) return "text/css; charset=utf-8";
    if (strcmp(dot, ".js") == 0) return "application/javascript; charset=utf-8";
    if (strcmp(dot, ".json") == 0) return "application/json; charset=utf-8";
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
    if (sz < 0) {
        fclose(f);
        send_cors_response(client_fd, 500, "text/plain", "File Error");
        return;
    }

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
    if (send_all(client_fd, header, strlen(header)) == 0) {
        char buf[16384];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            if (send_all(client_fd, buf, n) < 0) break;
        }
    }
    fclose(f);
}

static void* client_worker(void* arg) {
    int client_fd = (int)(intptr_t)arg;

#ifdef SO_NOSIGPIPE
    int nosig = 1;
    setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
#endif
    struct timeval tv;
    tv.tv_sec = 15;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char req_buf[8192];
    int n = recv(client_fd, req_buf, sizeof(req_buf) - 1, 0);
    if (n <= 0) {
        close(client_fd);
        return NULL;
    }
    req_buf[n] = 0;

    if (strncmp(req_buf, "OPTIONS ", 8) == 0) {
        send_cors_response(client_fd, 200, "text/plain", "");
        close(client_fd);
        return NULL;
    }

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

    // 1. Health check
    if (strcmp(path_start, "/health") == 0) {
        send_cors_response(client_fd, 200, "application/json",
                           "{\"status\":\"ok\",\"service\":\"PStream Native PS5 Engine\",\"version\":\"1.1.0\"}");
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
        char decoded[LOG_LINE_LEN];
        if (get_query_param(path_start, "msg", decoded, sizeof(decoded))) {
            add_log(decoded);
        }
        send_cors_response(client_fd, 200, "text/plain", "OK");
        close(client_fd);
        return NULL;
    }

    // 2b. View Logs API (/api/logs)
    if (strcmp(path_start, "/api/logs") == 0) {
        char* big_logs = malloc(65536);
        if (big_logs) {
            big_logs[0] = 0;
            size_t offset = 0;
            pthread_mutex_lock(&g_log_mutex);
            int count = g_log_count < MAX_LOG_LINES ? g_log_count : MAX_LOG_LINES;
            int start_idx = g_log_count < MAX_LOG_LINES ? 0 : (g_log_count % MAX_LOG_LINES);
            for (int i = 0; i < count; i++) {
                int idx = (start_idx + i) % MAX_LOG_LINES;
                int written = snprintf(big_logs + offset, 65536 - offset, "%s\n", g_log_lines[idx]);
                if (written <= 0 || offset + (size_t)written >= 65535) break;
                offset += (size_t)written;
            }
            pthread_mutex_unlock(&g_log_mutex);
            send_cors_response(client_fd, 200, "text/plain; charset=utf-8", big_logs);
            free(big_logs);
        } else {
            send_cors_response(client_fd, 500, "text/plain", "Out of memory");
        }
        close(client_fd);
        return NULL;
    }

    // 2c. Local TMDb API TLS Proxy (/api/tmdb?path=...)
    if (strncmp(path_start, "/api/tmdb", 9) == 0) {
        char tmdb_path[1536] = {0};
        if (!get_query_param(path_start, "path", tmdb_path, sizeof(tmdb_path)) || tmdb_path[0] != '/') {
            send_cors_response(client_fd, 400, "application/json", "{\"error\":\"Invalid path\"}");
            close(client_fd);
            return NULL;
        }
        char full_url[2048];
        snprintf(full_url, sizeof(full_url), "https://api.themoviedb.org/3%s", tmdb_path);
        char* json_buf = malloc(512 * 1024);
        if (!json_buf) {
            send_cors_response(client_fd, 500, "application/json", "{\"error\":\"OOM\"}");
            close(client_fd);
            return NULL;
        }
        int rlen = http_fetch(full_url, "https://www.themoviedb.org/", json_buf, 512 * 1024);
        if (rlen > 0) {
            send_cors_response(client_fd, 200, "application/json; charset=utf-8", json_buf);
        } else {
            send_cors_response(client_fd, 502, "application/json", "{\"error\":\"Upstream TMDb error\"}");
        }
        free(json_buf);
        close(client_fd);
        return NULL;
    }

    // 2d. Local TMDb Poster/Backdrop Image Proxy (/api/img?size=w500&path=/abc.jpg)
    if (strncmp(path_start, "/api/img", 8) == 0) {
        char size_str[32] = "w500";
        char img_path[512] = {0};
        get_query_param(path_start, "size", size_str, sizeof(size_str));
        if (!get_query_param(path_start, "path", img_path, sizeof(img_path)) || img_path[0] != '/') {
            send_cors_response(client_fd, 400, "text/plain", "Invalid image path");
            close(client_fd);
            return NULL;
        }
        char img_url[1024];
        snprintf(img_url, sizeof(img_url), "https://image.tmdb.org/t/p/%s%s", size_str, img_path);
        const char* mime = strstr(img_path, ".png") ? "image/png" : "image/jpeg";
        if (http_stream_to_client(client_fd, img_url, "https://www.themoviedb.org/",
                                  mime, "public, max-age=86400") < 0) {
            send_cors_response(client_fd, 502, "text/plain", "Image proxy error");
        }
        close(client_fd);
        return NULL;
    }

    // 3. Stream Resolver Endpoint (/api/stream/resolve)
    if (strncmp(path_start, "/api/stream/resolve", 19) == 0) {
        char id[64] = {0};
        char type[16] = "movie";
        char s_buf[16] = "1";
        char e_buf[16] = "1";
        char lang[16] = "en";
        char sp_buf[8] = "0";

        get_query_param(path_start, "id", id, sizeof(id));
        get_query_param(path_start, "type", type, sizeof(type));
        get_query_param(path_start, "season", s_buf, sizeof(s_buf));
        get_query_param(path_start, "episode", e_buf, sizeof(e_buf));
        if (!get_query_param(path_start, "lang", lang, sizeof(lang))) {
            get_query_param(path_start, "provider", lang, sizeof(lang));
        }
        get_query_param(path_start, "segproxy", sp_buf, sizeof(sp_buf));

        int season = atoi(s_buf) > 0 ? atoi(s_buf) : 1;
        int episode = atoi(e_buf) > 0 ? atoi(e_buf) : 1;
        int segproxy = (strcmp(sp_buf, "1") == 0) ? 1 : 0;

        if (id[0]) {
            char raw_stream_url[2048] = {0};
            if (extract_vix_stream(id, type, season, episode, lang, raw_stream_url, sizeof(raw_stream_url)) == 0) {
                char enc_url[4096];
                urlencode(raw_stream_url, enc_url, sizeof(enc_url));
                char res_json[5120];
                snprintf(res_json, sizeof(res_json),
                    "{\"status\":\"ok\",\"streamUrl\":\"%s/api/stream/master.m3u8?url=%s%s\",\"type\":\"hls\"}",
                    HOST_PREFIX, enc_url, segproxy ? "&segproxy=1" : ""
                );
                send_cors_response(client_fd, 200, "application/json", res_json);
                close(client_fd);
                return NULL;
            }
        }

        send_cors_response(client_fd, 500, "application/json",
                           "{\"status\":\"error\",\"error\":\"Stream resolution failed\"}");
        close(client_fd);
        return NULL;
    }

    // 4. Master Playlist Proxy (/api/stream/master.m3u8?url=...)
    if (strncmp(path_start, "/api/stream/master.m3u8", 23) == 0) {
        char target_url[2048] = {0};
        char sp_buf[8] = "0";
        if (!get_query_param(path_start, "url", target_url, sizeof(target_url)) || !target_url[0]) {
            send_cors_response(client_fd, 400, "text/plain", "Missing url parameter");
            close(client_fd);
            return NULL;
        }
        get_query_param(path_start, "segproxy", sp_buf, sizeof(sp_buf));
        int segproxy = (strcmp(sp_buf, "1") == 0) ? 1 : 0;

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

        rewrite_master_playlist(raw_master, target_url, segproxy, rewritten_master, 1024 * 1024);
        free(raw_master);

        send_cors_response(client_fd, 200, "application/vnd.apple.mpegurl; charset=utf-8", rewritten_master);
        free(rewritten_master);
        close(client_fd);
        return NULL;
    }

    // 5. Sub-Playlist Proxy & Key Rewriter (/api/stream/sub.m3u8?url=...)
    if (strncmp(path_start, "/api/stream/sub.m3u8", 20) == 0) {
        char target_url[2048] = {0};
        char sp_buf[8] = "0";
        if (!get_query_param(path_start, "url", target_url, sizeof(target_url)) || !target_url[0]) {
            send_cors_response(client_fd, 400, "text/plain", "Missing url parameter");
            close(client_fd);
            return NULL;
        }
        get_query_param(path_start, "segproxy", sp_buf, sizeof(sp_buf));
        int segproxy = (strcmp(sp_buf, "1") == 0) ? 1 : 0;

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

        rewrite_sub_playlist(raw_sub, target_url, segproxy, rewritten_sub, 4 * 1024 * 1024);
        free(raw_sub);

        send_cors_response(client_fd, 200, "application/vnd.apple.mpegurl; charset=utf-8", rewritten_sub);
        free(rewritten_sub);
        close(client_fd);
        return NULL;
    }

    // 6. AES-128 Key Proxy (/api/stream/key?url=...)
    if (strncmp(path_start, "/api/stream/key", 15) == 0) {
        char key_url[1024] = {0};
        get_query_param(path_start, "url", key_url, sizeof(key_url));

        uint8_t key_bytes[64];
        int k_len = fetch_enc_key(key_url[0] ? key_url : NULL, key_bytes, sizeof(key_bytes));
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
            if (send_all(client_fd, header, strlen(header)) == 0) {
                send_all(client_fd, key_bytes, (size_t)k_len);
            }
        } else {
            send_cors_response(client_fd, 502, "text/plain", "Failed to fetch enc.key");
        }
        close(client_fd);
        return NULL;
    }

    // 6b. Media Segment Relay (/api/stream/seg.ts?url=...)
    if (strncmp(path_start, "/api/stream/seg.ts", 18) == 0) {
        char seg_url[2048] = {0};
        if (!get_query_param(path_start, "url", seg_url, sizeof(seg_url)) || !seg_url[0]) {
            send_cors_response(client_fd, 400, "text/plain", "Missing url parameter");
            close(client_fd);
            return NULL;
        }
        if (http_stream_to_client(client_fd, seg_url, "https://vixsrc.to/",
                                  "video/mp2t", "no-store") < 0) {
            send_cors_response(client_fd, 502, "text/plain", "Segment relay failed");
        }
        close(client_fd);
        return NULL;
    }

    // 7. Check Update Endpoint (/api/check_update)
    if (strncmp(path_start, "/api/check_update", 17) == 0) {
        char remote_ver_buf[2048];
        memset(remote_ver_buf, 0, sizeof(remote_ver_buf));
        int r_len = http_fetch(
            "https://raw.githubusercontent.com/ducky-ux1/PStream/main/ps5/www/version.json",
            "https://github.com", remote_ver_buf, sizeof(remote_ver_buf) - 1);

        char local_ver[64] = "1.1.0";
        FILE* vf = fopen("/user/app/PSTR00001/version.json", "r");
        if (vf) {
            char l_buf[2048];
            size_t nr = fread(l_buf, 1, sizeof(l_buf) - 1, vf);
            l_buf[nr] = 0;
            fclose(vf);
            char* v_pos = strstr(l_buf, "\"version\":");
            if (v_pos) {
                char* q1 = strchr(v_pos + 10, '\"');
                if (q1) {
                    char* q2 = strchr(q1 + 1, '\"');
                    if (q2 && (size_t)(q2 - q1 - 1) < sizeof(local_ver)) {
                        memcpy(local_ver, q1 + 1, (size_t)(q2 - q1 - 1));
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
                    if (q2 && (size_t)(q2 - q1 - 1) < sizeof(remote_ver)) {
                        memcpy(remote_ver, q1 + 1, (size_t)(q2 - q1 - 1));
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
        int num_files = (int)(sizeof(files_to_update) / sizeof(files_to_update[0]));
        int updated_count = 0;

        for (int i = 0; i < num_files; i++) {
            char gh_url[512];
            snprintf(gh_url, sizeof(gh_url),
                     "https://raw.githubusercontent.com/ducky-ux1/PStream/main/ps5/www/%s",
                     files_to_update[i]);

            char* file_buf = malloc(1024 * 1024);
            if (!file_buf) continue;

            int dl_len = http_fetch(gh_url, "https://github.com", file_buf, (1024 * 1024) - 1);
            if (dl_len > 0) {
                char dst_path[512];
                snprintf(dst_path, sizeof(dst_path), "/user/app/PSTR00001/%s", files_to_update[i]);
                FILE* df = fopen(dst_path, "wb");
                if (df) {
                    fwrite(file_buf, 1, (size_t)dl_len, df);
                    fclose(df);
                    updated_count++;
                    printf("[PStream Updater] Updated %s (%d bytes)\n", files_to_update[i], dl_len);
                }
                snprintf(dst_path, sizeof(dst_path), "/system_ex/app/PSTR00001/%s", files_to_update[i]);
                FILE* sf = fopen(dst_path, "wb");
                if (sf) {
                    fwrite(file_buf, 1, (size_t)dl_len, sf);
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
    if (strcmp(path_start, "/") == 0 || strncmp(path_start, "/?", 2) == 0 ||
        strncmp(path_start, "/index.html", 11) == 0) {
        snprintf(full_filepath, sizeof(full_filepath), "%s/index.html", APP_DIR);
    } else {
        const char* rel = (path_start[0] == '/') ? path_start + 1 : path_start;
        char clean_rel[512];
        snprintf(clean_rel, sizeof(clean_rel), "%s", rel);
        char* qmark = strchr(clean_rel, '?');
        if (qmark) *qmark = 0;
        char* ampersand = strchr(clean_rel, '&');
        if (ampersand) *ampersand = 0;

        if (strstr(clean_rel, "..") != NULL) {
            send_cors_response(client_fd, 400, "text/plain", "Invalid Path");
            close(client_fd);
            return NULL;
        }
        snprintf(full_filepath, sizeof(full_filepath), "%s/%s", APP_DIR, clean_rel);
    }

    send_file_response(client_fd, full_filepath);
    close(client_fd);
    return NULL;
}

static void* precache_key_thread(void* arg) {
    (void)arg;
    uint8_t dummy[64];
    fetch_enc_key(NULL, dummy, sizeof(dummy));
    return NULL;
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    // Prevent SIGPIPE from terminating daemon when WebKit/hls.js aborts in-flight sockets
    signal(SIGPIPE, SIG_IGN);

    printf("[PStream Native Engine v1.1.0] Initializing standalone server on port %d...\n", SERVER_PORT);
    klog_puts("[PStream Native Engine v1.1.0] Starting daemon...");

    if (init_net_subsystem() < 0) {
        printf("[PStream Native Engine] Failed to initialize networking.\n");
        return -1;
    }

    pthread_t k_th;
    if (pthread_create(&k_th, NULL, precache_key_thread, NULL) == 0) {
        pthread_detach(k_th);
    }

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

    printf("[PStream Native Engine v1.1.0] Listening on 0.0.0.0:%d - ACTIVE\n", SERVER_PORT);
    klog_puts("[PStream Native Engine v1.1.0] Listening on 0.0.0.0:8086");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd >= 0) {
            pthread_t thread;
            if (pthread_create(&thread, NULL, client_worker, (void*)(intptr_t)client_fd) == 0) {
                pthread_detach(thread);
            } else {
                close(client_fd);
            }
        }
    }

    close(server_fd);
    return 0;
}
