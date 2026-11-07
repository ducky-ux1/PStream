#include "server.h"
#include "logger.h"

#include <orbis/Http.h>
#include <orbis/Ssl.h>
#include <orbis/Net.h>
#include <orbis/NetCtl.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define HOST_PREFIX "http://127.0.0.1:8086"
#define OTA_WWW_DIR "/data/PStream/www"
#define PKG_WWW_DIR "/app0/www"
#define KEY_TTL_SECONDS 600
#define MAX_LOG_LINES 250
#define LOG_LINE_LEN 256

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace PStream {

std::atomic<bool> LocalEngineServer::s_running(false);
std::atomic<bool> LocalEngineServer::s_ready(false);
int LocalEngineServer::s_serverFd = -1;
pthread_t LocalEngineServer::s_acceptThread;

namespace {

int g_netMemId = -1;
int g_sslCtxId = -1;
int g_httpCtxId = -1;
uint64_t g_httpGen = 0;
pthread_rwlock_t g_httpRwLock = PTHREAD_RWLOCK_INITIALIZER;

uint8_t g_encKey[64];
int g_encKeyLen = 0;
char g_encKeyUrl[1024] = {0};
time_t g_encKeyTime = 0;
pthread_mutex_t g_keyMutex = PTHREAD_MUTEX_INITIALIZER;

char g_logLines[MAX_LOG_LINES][LOG_LINE_LEN];
int g_logCount = 0;
pthread_mutex_t g_logMutex = PTHREAD_MUTEX_INITIALIZER;

const char* USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36";

void addWebLog(const char* msg) {
    if (!msg || !msg[0]) return;
    pthread_mutex_lock(&g_logMutex);
    std::snprintf(g_logLines[g_logCount % MAX_LOG_LINES], LOG_LINE_LEN, "%s", msg);
    g_logCount++;
    pthread_mutex_unlock(&g_logMutex);
    app_log("[WebKit] %s\n", msg);
}

int skipSslCallback(int libsslId, unsigned int verifyErr, void* const sslCert[], int certNum, void* userArg) {
    (void)libsslId;
    (void)verifyErr;
    (void)sslCert;
    (void)certNum;
    (void)userArg;
    return 1;
}

bool initOrbisHttp() {
    sceNetInit();
    sceNetCtlInit();
    g_netMemId = sceNetPoolCreate("PStreamNetPool", 4 * 1024 * 1024, 0);
    g_sslCtxId = sceSslInit(2 * 1024 * 1024);
    g_httpCtxId = sceHttpInit(g_netMemId, g_sslCtxId, 4 * 1024 * 1024);
    g_httpGen = 1;
    app_log("[PS4 Engine] Orbis Net/SSL/HTTP initialized (httpCtx=%d)\n", g_httpCtxId);
    return (g_httpCtxId >= 0);
}

void reinitOrbisHttp(uint64_t failedGen) {
    pthread_rwlock_wrlock(&g_httpRwLock);
    if (g_httpGen == failedGen) {
        app_log("[PS4 Engine] Re-initializing Orbis HTTP/SSL context after error (gen %lu)...\n",
                static_cast<unsigned long>(failedGen));
        if (g_httpCtxId >= 0) {
            sceHttpTerm(g_httpCtxId);
            g_httpCtxId = -1;
        }
        if (g_sslCtxId >= 0) {
            reinterpret_cast<int(*)(int)>(sceSslTerm)(g_sslCtxId);
            g_sslCtxId = -1;
        }
        if (g_netMemId < 0) {
            g_netMemId = sceNetPoolCreate("PStreamNetPool", 4 * 1024 * 1024, 0);
        }
        g_sslCtxId = sceSslInit(2 * 1024 * 1024);
        g_httpCtxId = sceHttpInit(g_netMemId, g_sslCtxId, 4 * 1024 * 1024);
        g_httpGen++;
    }
    pthread_rwlock_unlock(&g_httpRwLock);
}

int sendAll(int fd, const void* data, size_t len) {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, ptr + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        sent += static_cast<size_t>(n);
    }
    return 0;
}

int openHttpGet(const char* url, const char* referer, int* outTmpl, int* outConn, int* outReq) {
    int tmId = sceHttpCreateTemplate(g_httpCtxId, USER_AGENT, ORBIS_HTTP_VERSION_1_1, 1);
    if (tmId < 0) return -1;

    reinterpret_cast<int32_t(*)(int32_t, int32_t)>(sceHttpSetAutoRedirect)(tmId, 1);
    sceHttpsSetSslCallback(tmId, skipSslCallback, nullptr);
    sceHttpsDisableOption(tmId, 0xFFFFFFFFu);
    sceHttpSetResolveTimeOut(tmId, 8 * 1000 * 1000);
    sceHttpSetConnectTimeOut(tmId, 10 * 1000 * 1000);
    sceHttpSetSendTimeOut(tmId, 10 * 1000 * 1000);
    reinterpret_cast<int32_t(*)(int32_t, uint32_t)>(sceHttpSetRecvTimeOut)(tmId, 20 * 1000 * 1000);

    int connId = sceHttpCreateConnectionWithURL(tmId, url, true);
    if (connId < 0) {
        sceHttpDeleteTemplate(tmId);
        return -1;
    }

    int reqId = sceHttpCreateRequestWithURL(connId, ORBIS_METHOD_GET, url, 0);
    if (reqId < 0) {
        sceHttpDeleteConnection(connId);
        sceHttpDeleteTemplate(tmId);
        return -1;
    }

    sceHttpAddRequestHeader(reqId, "User-Agent", USER_AGENT, 0);
    sceHttpAddRequestHeader(reqId, "Accept", "*/*", 0);
    sceHttpAddRequestHeader(reqId, "Accept-Language", "en-US,en;q=0.9", 0);
    if (referer && referer[0]) {
        sceHttpAddRequestHeader(reqId, "Referer", referer, 0);
    }
    if (std::strstr(url, "vixsrc") || (referer && std::strstr(referer, "vixsrc"))) {
        sceHttpAddRequestHeader(reqId, "Origin", "https://vixsrc.to", 0);
    }

    *outTmpl = tmId;
    *outConn = connId;
    *outReq = reqId;
    return 0;
}

void closeHttpHandles(int tmId, int connId, int reqId) {
    if (reqId >= 0) sceHttpDeleteRequest(reqId);
    if (connId >= 0) sceHttpDeleteConnection(connId);
    if (tmId >= 0) sceHttpDeleteTemplate(tmId);
}

int httpFetch(const char* url, const char* referer, char* outBuf, size_t outMax) {
    if (!url || !outBuf || outMax < 2) return -1;

    for (int attempt = 0; attempt < 2; attempt++) {
        pthread_rwlock_rdlock(&g_httpRwLock);
        uint64_t curGen = g_httpGen;

        int tmId = -1, connId = -1, reqId = -1;
        if (openHttpGet(url, referer, &tmId, &connId, &reqId) < 0) {
            pthread_rwlock_unlock(&g_httpRwLock);
            if (attempt == 0) {
                reinitOrbisHttp(curGen);
                continue;
            }
            return -1;
        }

        int ret = sceHttpSendRequest(reqId, nullptr, 0);
        if (ret < 0) {
            closeHttpHandles(tmId, connId, reqId);
            pthread_rwlock_unlock(&g_httpRwLock);
            if (attempt == 0) {
                reinitOrbisHttp(curGen);
                continue;
            }
            return -1;
        }

        int32_t status = 0;
        sceHttpGetStatusCode(reqId, &status);
        if (status < 200 || status >= 300) {
            closeHttpHandles(tmId, connId, reqId);
            pthread_rwlock_unlock(&g_httpRwLock);
            return -1;
        }

        size_t total = 0;
        while (total < outMax - 1) {
            int n = sceHttpReadData(reqId, outBuf + total, static_cast<uint32_t>(outMax - 1 - total));
            if (n <= 0) break;
            total += static_cast<size_t>(n);
        }
        outBuf[total] = 0;

        closeHttpHandles(tmId, connId, reqId);
        pthread_rwlock_unlock(&g_httpRwLock);
        return static_cast<int>(total);
    }
    return -1;
}

int httpStreamToClient(int clientFd, const char* url, const char* referer,
                       const char* contentType, const char* cacheCtrl) {
    for (int attempt = 0; attempt < 2; attempt++) {
        pthread_rwlock_rdlock(&g_httpRwLock);
        uint64_t curGen = g_httpGen;

        int tmId = -1, connId = -1, reqId = -1;
        if (openHttpGet(url, referer, &tmId, &connId, &reqId) < 0) {
            pthread_rwlock_unlock(&g_httpRwLock);
            if (attempt == 0) {
                reinitOrbisHttp(curGen);
                continue;
            }
            return -1;
        }

        int ret = sceHttpSendRequest(reqId, nullptr, 0);
        if (ret < 0) {
            closeHttpHandles(tmId, connId, reqId);
            pthread_rwlock_unlock(&g_httpRwLock);
            if (attempt == 0) {
                reinitOrbisHttp(curGen);
                continue;
            }
            return -1;
        }

        int32_t status = 0;
        sceHttpGetStatusCode(reqId, &status);
        if (status < 200 || status >= 300) {
            closeHttpHandles(tmId, connId, reqId);
            pthread_rwlock_unlock(&g_httpRwLock);
            return -1;
        }

        char header[512];
        std::snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
            "Access-Control-Allow-Headers: *\r\n"
            "Content-Type: %s\r\n"
            "Cache-Control: %s\r\n"
            "Connection: close\r\n\r\n",
            contentType, cacheCtrl ? cacheCtrl : "no-cache"
        );

        if (sendAll(clientFd, header, std::strlen(header)) == 0) {
            char* chunk = static_cast<char*>(std::malloc(65536));
            if (chunk) {
                while (true) {
                    int n = sceHttpReadData(reqId, chunk, 65536);
                    if (n <= 0) break;
                    if (sendAll(clientFd, chunk, static_cast<size_t>(n)) < 0) break;
                }
                std::free(chunk);
            }
        }

        closeHttpHandles(tmId, connId, reqId);
        pthread_rwlock_unlock(&g_httpRwLock);
        return 0;
    }
    return -1;
}

void urlEncode(const char* src, char* dst, size_t dstMax) {
    static const char* hex = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 4 < dstMax; i++) {
        unsigned char c = static_cast<unsigned char>(src[i]);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            dst[j++] = static_cast<char>(c);
        } else {
            dst[j++] = '%';
            dst[j++] = hex[c >> 4];
            dst[j++] = hex[c & 0x0F];
        }
    }
    dst[j] = 0;
}

void urlDecode(const char* src, char* dst, size_t dstMax) {
    size_t i = 0, j = 0;
    while (src[i] && j + 1 < dstMax) {
        if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            char hex[3] = {src[i + 1], src[i + 2], 0};
            dst[j++] = static_cast<char>(std::strtol(hex, nullptr, 16));
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

bool getQueryParam(const char* path, const char* key, char* out, size_t outMax) {
    const char* q = std::strchr(path, '?');
    if (!q) return false;
    q++;
    size_t klen = std::strlen(key);
    while (*q) {
        const char* amp = std::strchr(q, '&');
        size_t segLen = amp ? static_cast<size_t>(amp - q) : std::strlen(q);
        if (segLen > klen + 1 && std::strncmp(q, key, klen) == 0 && q[klen] == '=') {
            char raw[4096];
            size_t vlen = segLen - (klen + 1);
            if (vlen >= sizeof(raw)) vlen = sizeof(raw) - 1;
            std::memcpy(raw, q + klen + 1, vlen);
            raw[vlen] = 0;
            urlDecode(raw, out, outMax);
            return true;
        }
        if (!amp) break;
        q = amp + 1;
    }
    return false;
}

const char* mapLang(const char* in) {
    if (!in || !in[0]) return "en";
    if (std::strcmp(in, "eng") == 0 || std::strcmp(in, "en") == 0) return "en";
    if (std::strcmp(in, "ita") == 0 || std::strcmp(in, "it") == 0) return "it";
    if (std::strcmp(in, "esp") == 0 || std::strcmp(in, "es") == 0) return "es";
    if (std::strcmp(in, "ger") == 0 || std::strcmp(in, "de") == 0) return "de";
    if (std::strcmp(in, "fra") == 0 || std::strcmp(in, "fr") == 0) return "fr";
    if (std::strcmp(in, "por") == 0 || std::strcmp(in, "pt") == 0) return "pt";
    if (std::strcmp(in, "anime") == 0) return "en";
    return in;
}

int fetchEncKey(const char* requestedUrl, uint8_t* outKey, size_t outMax) {
    const char* target = (requestedUrl && std::strncmp(requestedUrl, "http", 4) == 0)
                             ? requestedUrl
                             : "https://vixsrc.to/storage/enc.key";
    time_t now = std::time(nullptr);

    pthread_mutex_lock(&g_keyMutex);
    if (g_encKeyLen >= 16 &&
        std::strcmp(g_encKeyUrl, target) == 0 &&
        (now - g_encKeyTime) < KEY_TTL_SECONDS) {
        int copyLen = g_encKeyLen < static_cast<int>(outMax) ? g_encKeyLen : static_cast<int>(outMax);
        std::memcpy(outKey, g_encKey, static_cast<size_t>(copyLen));
        pthread_mutex_unlock(&g_keyMutex);
        return copyLen;
    }
    pthread_mutex_unlock(&g_keyMutex);

    char tmp[128];
    int len = httpFetch(target, "https://vixsrc.to/", tmp, sizeof(tmp));
    if (len >= 16) {
        pthread_mutex_lock(&g_keyMutex);
        int storeLen = len < static_cast<int>(sizeof(g_encKey)) ? len : static_cast<int>(sizeof(g_encKey));
        std::memcpy(g_encKey, tmp, static_cast<size_t>(storeLen));
        g_encKeyLen = storeLen;
        std::snprintf(g_encKeyUrl, sizeof(g_encKeyUrl), "%s", target);
        g_encKeyTime = std::time(nullptr);
        pthread_mutex_unlock(&g_keyMutex);

        int copyLen = storeLen < static_cast<int>(outMax) ? storeLen : static_cast<int>(outMax);
        std::memcpy(outKey, tmp, static_cast<size_t>(copyLen));
        return copyLen;
    }
    return -1;
}

int extractVixStreamLang(const char* id, const char* type, int season, int episode,
                         const char* lang, char* outUrl, size_t outUrlMax) {
    char* buf1 = static_cast<char*>(std::malloc(16384));
    char* buf2 = static_cast<char*>(std::malloc(131072));
    if (!buf1 || !buf2) {
        if (buf1) std::free(buf1);
        if (buf2) std::free(buf2);
        return -1;
    }

    char apiUrl[512];
    if (std::strcmp(type, "tv") == 0) {
        std::snprintf(apiUrl, sizeof(apiUrl), "https://vixsrc.to/api/tv/%s/%d/%d?lang=%s", id, season, episode, lang);
    } else {
        std::snprintf(apiUrl, sizeof(apiUrl), "https://vixsrc.to/api/movie/%s?lang=%s", id, lang);
    }

    int len1 = httpFetch(apiUrl, "https://vixsrc.to/", buf1, 16384);
    if (len1 <= 0) {
        std::free(buf1);
        std::free(buf2);
        return -1;
    }

    char* srcPos = std::strstr(buf1, "\"src\":\"");
    if (srcPos) {
        srcPos += 7;
    } else {
        srcPos = std::strstr(buf1, "\"src\": \"");
        if (srcPos) srcPos += 8;
    }
    if (!srcPos) {
        std::free(buf1);
        std::free(buf2);
        return -1;
    }
    char* srcEnd = std::strchr(srcPos, '"');
    if (!srcEnd) {
        std::free(buf1);
        std::free(buf2);
        return -1;
    }
    *srcEnd = 0;

    char cleanSrc[2048];
    size_t j = 0;
    for (size_t i = 0; srcPos[i] && j + 1 < sizeof(cleanSrc); i++) {
        if (srcPos[i] == '\\' && srcPos[i + 1] == '/') continue;
        cleanSrc[j++] = srcPos[i];
    }
    cleanSrc[j] = 0;

    char embedUrl[2200];
    if (cleanSrc[0] == '/') {
        std::snprintf(embedUrl, sizeof(embedUrl), "https://vixsrc.to%s", cleanSrc);
    } else {
        std::snprintf(embedUrl, sizeof(embedUrl), "%s", cleanSrc);
    }

    int len2 = httpFetch(embedUrl, "https://vixsrc.to/", buf2, 131072);
    if (len2 <= 0) {
        std::free(buf1);
        std::free(buf2);
        return -1;
    }

    char token[256] = {0};
    char expires[256] = {0};
    char baseUrl[512] = {0};

    char* tPos = std::strstr(buf2, "'token': '");
    if (tPos) tPos += 10;
    else {
        tPos = std::strstr(buf2, "\"token\": \"");
        if (tPos) tPos += 10;
    }
    if (tPos) {
        char* tEnd = std::strchr(tPos, '\'');
        if (!tEnd) tEnd = std::strchr(tPos, '"');
        if (tEnd && static_cast<size_t>(tEnd - tPos) < sizeof(token)) {
            std::memcpy(token, tPos, static_cast<size_t>(tEnd - tPos));
            token[tEnd - tPos] = 0;
        }
    }

    char* expPos = std::strstr(buf2, "'expires': '");
    if (expPos) expPos += 12;
    else {
        expPos = std::strstr(buf2, "\"expires\": \"");
        if (expPos) expPos += 12;
    }
    if (expPos) {
        char* expEnd = std::strchr(expPos, '\'');
        if (!expEnd) expEnd = std::strchr(expPos, '"');
        if (expEnd && static_cast<size_t>(expEnd - expPos) < sizeof(expires)) {
            std::memcpy(expires, expPos, static_cast<size_t>(expEnd - expPos));
            expires[expEnd - expPos] = 0;
        }
    }

    char* urlPos = std::strstr(buf2, "url: '");
    if (urlPos) urlPos += 6;
    else {
        urlPos = std::strstr(buf2, "url: \"");
        if (urlPos) urlPos += 6;
    }
    if (urlPos) {
        char* uEnd = std::strchr(urlPos, '\'');
        if (!uEnd) uEnd = std::strchr(urlPos, '"');
        if (uEnd && static_cast<size_t>(uEnd - urlPos) < sizeof(baseUrl)) {
            std::memcpy(baseUrl, urlPos, static_cast<size_t>(uEnd - urlPos));
            baseUrl[uEnd - urlPos] = 0;
        }
    }

    if (!baseUrl[0]) {
        char* pPos = std::strstr(buf2, "/playlist/");
        if (pPos) {
            char* pEnd = std::strchr(pPos, '\'');
            if (!pEnd) pEnd = std::strchr(pPos, '"');
            if (pEnd) {
                size_t pLen = static_cast<size_t>(pEnd - pPos);
                std::snprintf(baseUrl, sizeof(baseUrl), "https://vixsrc.to%.*s", static_cast<int>(pLen), pPos);
            }
        }
    }

    std::free(buf1);
    std::free(buf2);

    if (token[0] && expires[0] && baseUrl[0]) {
        char sep = std::strchr(baseUrl, '?') ? '&' : '?';
        std::snprintf(outUrl, outUrlMax, "%s%ctoken=%s&expires=%s&h=1&lang=%s",
                      baseUrl, sep, token, expires, lang);
        app_log("[PS4 Engine] Resolved VixSrc stream (%s): %s\n", lang, outUrl);
        return 0;
    }
    return -1;
}

int extractVixStream(const char* id, const char* type, int season, int episode,
                     const char* langIn, char* outUrl, size_t outUrlMax) {
    const char* lang = mapLang(langIn);
    if (extractVixStreamLang(id, type, season, episode, lang, outUrl, outUrlMax) == 0) {
        return 0;
    }
    if (std::strcmp(lang, "en") != 0) {
        if (extractVixStreamLang(id, type, season, episode, "en", outUrl, outUrlMax) == 0) {
            return 0;
        }
    }
    return -1;
}

void extractOriginAndDir(const char* url, char* baseOrigin, size_t originMax,
                         char* baseDir, size_t dirMax) {
    baseOrigin[0] = 0;
    baseDir[0] = 0;

    const char* colon = std::strstr(url, "://");
    if (colon) {
        const char* slash = std::strchr(colon + 3, '/');
        if (slash) {
            size_t oLen = static_cast<size_t>(slash - url);
            if (oLen >= originMax) oLen = originMax - 1;
            std::memcpy(baseOrigin, url, oLen);
            baseOrigin[oLen] = 0;
        } else {
            std::snprintf(baseOrigin, originMax, "%s", url);
        }
    }

    const char* q = std::strchr(url, '?');
    size_t pathEndIdx = q ? static_cast<size_t>(q - url) : std::strlen(url);
    const char* lastSlash = nullptr;
    for (size_t i = 0; i < pathEndIdx; i++) {
        if (url[i] == '/') lastSlash = url + i;
    }
    if (lastSlash && (!colon || lastSlash > colon + 2)) {
        size_t dLen = static_cast<size_t>(lastSlash - url);
        if (dLen >= dirMax) dLen = dirMax - 1;
        std::memcpy(baseDir, url, dLen);
        baseDir[dLen] = 0;
    } else {
        std::snprintf(baseDir, dirMax, "%s", baseOrigin);
    }
}

void resolveRelativeUrl(const char* target, const char* baseOrigin, const char* baseDir,
                        char* outAbs, size_t outMax) {
    if (std::strncmp(target, "http://", 7) == 0 || std::strncmp(target, "https://", 8) == 0) {
        std::snprintf(outAbs, outMax, "%s", target);
    } else if (target[0] == '/') {
        std::snprintf(outAbs, outMax, "%s%s", baseOrigin, target);
    } else {
        std::snprintf(outAbs, outMax, "%s/%s", baseDir, target);
    }
}

void rewriteMasterPlaylist(const char* in, const char* masterUrl, int segproxy,
                           char* out, size_t outMax) {
    char baseOrigin[512];
    char baseDir[1024];
    extractOriginAndDir(masterUrl, baseOrigin, sizeof(baseOrigin), baseDir, sizeof(baseDir));

    out[0] = 0;
    size_t outLen = 0;
    char line[4096];
    const char* p = in;
    const char* spSuffix = segproxy ? "&segproxy=1" : "";

    while (*p) {
        size_t lineLen = 0;
        while (*p && *p != '\n' && *p != '\r' && lineLen < sizeof(line) - 1) {
            line[lineLen++] = *p++;
        }
        line[lineLen] = 0;
        while (*p == '\r' || *p == '\n') p++;

        if (line[0] != 0 && line[0] != '#') {
            char absSub[2048];
            resolveRelativeUrl(line, baseOrigin, baseDir, absSub, sizeof(absSub));

            char enc[4096];
            urlEncode(absSub, enc, sizeof(enc));
            char rep[5120];
            std::snprintf(rep, sizeof(rep), "%s/api/stream/sub.m3u8?url=%s%s\n", HOST_PREFIX, enc, spSuffix);
            size_t rlen = std::strlen(rep);
            if (outLen + rlen < outMax) {
                std::memcpy(out + outLen, rep, rlen);
                outLen += rlen;
            }
        } else if (std::strstr(line, "URI=\"") || std::strstr(line, "URI='")) {
            char quoteChar = std::strstr(line, "URI=\"") ? '"' : '\'';
            char* uStart = quoteChar == '"' ? (std::strstr(line, "URI=\"") + 5) : (std::strstr(line, "URI='") + 5);
            char* uEnd = std::strchr(uStart, quoteChar);
            if (uEnd) {
                char target[2048] = {0};
                size_t tlen = static_cast<size_t>(uEnd - uStart);
                if (tlen < sizeof(target)) {
                    std::memcpy(target, uStart, tlen);
                    target[tlen] = 0;
                }
                char absTarget[2048];
                resolveRelativeUrl(target, baseOrigin, baseDir, absTarget, sizeof(absTarget));

                char enc[4096];
                urlEncode(absTarget, enc, sizeof(enc));
                char before[1024];
                size_t bLen = static_cast<size_t>(uStart - line);
                if (bLen >= sizeof(before)) bLen = sizeof(before) - 1;
                std::memcpy(before, line, bLen);
                before[bLen] = 0;

                char rep[6144];
                std::snprintf(rep, sizeof(rep), "%s%s/api/stream/sub.m3u8?url=%s%s%s\n",
                              before, HOST_PREFIX, enc, spSuffix, uEnd);
                size_t rlen = std::strlen(rep);
                if (outLen + rlen < outMax) {
                    std::memcpy(out + outLen, rep, rlen);
                    outLen += rlen;
                }
            } else {
                size_t llen = std::strlen(line);
                if (outLen + llen + 1 < outMax) {
                    std::memcpy(out + outLen, line, llen);
                    outLen += llen;
                    out[outLen++] = '\n';
                }
            }
        } else {
            size_t llen = std::strlen(line);
            if (outLen + llen + 1 < outMax) {
                std::memcpy(out + outLen, line, llen);
                outLen += llen;
                out[outLen++] = '\n';
            }
        }
    }
    out[outLen] = 0;
}

void rewriteSubPlaylist(const char* in, const char* subUrl, int segproxy,
                        char* out, size_t outMax) {
    char baseOrigin[512];
    char baseDir[1024];
    extractOriginAndDir(subUrl, baseOrigin, sizeof(baseOrigin), baseDir, sizeof(baseDir));

    out[0] = 0;
    size_t outLen = 0;
    char line[4096];
    const char* p = in;

    while (*p) {
        size_t lineLen = 0;
        while (*p && *p != '\n' && *p != '\r' && lineLen < sizeof(line) - 1) {
            line[lineLen++] = *p++;
        }
        line[lineLen] = 0;
        while (*p == '\r' || *p == '\n') p++;

        if (std::strstr(line, "#EXT-X-KEY:") && std::strstr(line, "AES-128") &&
            (std::strstr(line, "URI=\"") || std::strstr(line, "URI='"))) {
            char quoteChar = std::strstr(line, "URI=\"") ? '"' : '\'';
            char* uStart = quoteChar == '"' ? (std::strstr(line, "URI=\"") + 5) : (std::strstr(line, "URI='") + 5);
            char* uEnd = std::strchr(uStart, quoteChar);
            if (uEnd) {
                char rawKey[1024] = {0};
                size_t klen = static_cast<size_t>(uEnd - uStart);
                if (klen < sizeof(rawKey)) {
                    std::memcpy(rawKey, uStart, klen);
                    rawKey[klen] = 0;
                }
                char absKey[2048];
                resolveRelativeUrl(rawKey, baseOrigin, baseDir, absKey, sizeof(absKey));
                char encKey[3072];
                urlEncode(absKey, encKey, sizeof(encKey));

                size_t bLen = static_cast<size_t>(uStart - line);
                char before[1024];
                if (bLen >= sizeof(before)) bLen = sizeof(before) - 1;
                std::memcpy(before, line, bLen);
                before[bLen] = 0;

                char rep[4608];
                std::snprintf(rep, sizeof(rep), "%s%s/api/stream/key?url=%s%s\n",
                              before, HOST_PREFIX, encKey, uEnd);
                size_t rlen = std::strlen(rep);
                if (outLen + rlen < outMax) {
                    std::memcpy(out + outLen, rep, rlen);
                    outLen += rlen;
                }
            }
        } else if (std::strstr(line, "#EXT-X-MAP:") && (std::strstr(line, "URI=\"") || std::strstr(line, "URI='"))) {
            char quoteChar = std::strstr(line, "URI=\"") ? '"' : '\'';
            char* uStart = quoteChar == '"' ? (std::strstr(line, "URI=\"") + 5) : (std::strstr(line, "URI='") + 5);
            char* uEnd = std::strchr(uStart, quoteChar);
            if (uEnd) {
                char mapPath[1024] = {0};
                size_t mlen = static_cast<size_t>(uEnd - uStart);
                if (mlen < sizeof(mapPath)) {
                    std::memcpy(mapPath, uStart, mlen);
                    mapPath[mlen] = 0;
                }
                char absMap[2048];
                resolveRelativeUrl(mapPath, baseOrigin, baseDir, absMap, sizeof(absMap));

                size_t bLen = static_cast<size_t>(uStart - line);
                char before[1024];
                if (bLen >= sizeof(before)) bLen = sizeof(before) - 1;
                std::memcpy(before, line, bLen);
                before[bLen] = 0;

                char rep[5120];
                if (segproxy) {
                    char encMap[3584];
                    urlEncode(absMap, encMap, sizeof(encMap));
                    std::snprintf(rep, sizeof(rep), "%s%s/api/stream/seg.ts?url=%s%s\n",
                                  before, HOST_PREFIX, encMap, uEnd);
                } else {
                    std::snprintf(rep, sizeof(rep), "%s%s%s\n", before, absMap, uEnd);
                }
                size_t rlen = std::strlen(rep);
                if (outLen + rlen < outMax) {
                    std::memcpy(out + outLen, rep, rlen);
                    outLen += rlen;
                }
            }
        } else if (line[0] != 0 && line[0] != '#') {
            char absSeg[2048];
            resolveRelativeUrl(line, baseOrigin, baseDir, absSeg, sizeof(absSeg));

            char rep[5120];
            if (segproxy) {
                char encSeg[4096];
                urlEncode(absSeg, encSeg, sizeof(encSeg));
                std::snprintf(rep, sizeof(rep), "%s/api/stream/seg.ts?url=%s\n", HOST_PREFIX, encSeg);
            } else {
                std::snprintf(rep, sizeof(rep), "%s\n", absSeg);
            }
            size_t rlen = std::strlen(rep);
            if (outLen + rlen < outMax) {
                std::memcpy(out + outLen, rep, rlen);
                outLen += rlen;
            }
        } else {
            size_t llen = std::strlen(line);
            if (outLen + llen + 1 < outMax) {
                std::memcpy(out + outLen, line, llen);
                outLen += llen;
                out[outLen++] = '\n';
            }
        }
    }
    out[outLen] = 0;
}

void sendCorsResponse(int clientFd, int statusCode, const char* contentType, const char* body) {
    char header[1024];
    size_t bodyLen = body ? std::strlen(body) : 0;
    const char* statusText = (statusCode == 200) ? "OK" :
                             (statusCode == 400) ? "Bad Request" :
                             (statusCode == 404) ? "Not Found" :
                             (statusCode == 502) ? "Bad Gateway" : "Internal Server Error";

    std::snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: *\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        statusCode, statusText, contentType, bodyLen
    );

    if (sendAll(clientFd, header, std::strlen(header)) == 0 && bodyLen > 0) {
        sendAll(clientFd, body, bodyLen);
    }
}

const char* getMimeType(const char* path) {
    const char* dot = std::strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (std::strcmp(dot, ".html") == 0) return "text/html; charset=utf-8";
    if (std::strcmp(dot, ".css") == 0) return "text/css; charset=utf-8";
    if (std::strcmp(dot, ".js") == 0) return "application/javascript; charset=utf-8";
    if (std::strcmp(dot, ".json") == 0) return "application/json; charset=utf-8";
    if (std::strcmp(dot, ".png") == 0) return "image/png";
    if (std::strcmp(dot, ".jpg") == 0 || std::strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (std::strcmp(dot, ".svg") == 0) return "image/svg+xml";
    return "application/octet-stream";
}

void sendFileResponse(int clientFd, const char* relPath) {
    char otaPath[1024];
    char pkgPath[1024];
    std::snprintf(otaPath, sizeof(otaPath), "%s/%s", OTA_WWW_DIR, relPath);
    std::snprintf(pkgPath, sizeof(pkgPath), "%s/%s", PKG_WWW_DIR, relPath);

    FILE* f = std::fopen(otaPath, "rb");
    if (!f) {
        f = std::fopen(pkgPath, "rb");
    }
    if (!f) {
        sendCorsResponse(clientFd, 404, "text/plain", "File Not Found");
        return;
    }

    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        std::fclose(f);
        sendCorsResponse(clientFd, 500, "text/plain", "File Error");
        return;
    }

    const char* mime = getMimeType(relPath);
    char header[512];
    std::snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n",
        mime, sz
    );

    if (sendAll(clientFd, header, std::strlen(header)) == 0) {
        char buf[16384];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
            if (sendAll(clientFd, buf, n) < 0) break;
        }
    }
    std::fclose(f);
}

int parseBuildNumber(const char* filepath) {
    FILE* f = std::fopen(filepath, "r");
    if (!f) return -1;
    char buf[1024];
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    std::fclose(f);
    char* b = std::strstr(buf, "\"build\"");
    if (!b) return -1;
    char* colon = std::strchr(b, ':');
    if (!colon) return -1;
    return std::atoi(colon + 1);
}

void reconcileOtaDirectory() {
    mkdir("/data/PStream", 0777);
    mkdir(OTA_WWW_DIR, 0777);

    int pkgBuild = parseBuildNumber(PKG_WWW_DIR "/version.json");
    int otaBuild = parseBuildNumber(OTA_WWW_DIR "/version.json");
    if (otaBuild >= 0 && pkgBuild >= otaBuild) {
        const char* files[] = { "app.js", "app.css", "index.html", "input.js", "version.json" };
        char p[512];
        for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
            std::snprintf(p, sizeof(p), "%s/%s", OTA_WWW_DIR, files[i]);
            unlink(p);
        }
        app_log("[PS4 Engine] Cleared older OTA cache (ota=%d <= pkg=%d)\n", otaBuild, pkgBuild);
    }
}

void readLocalVersion(char* outVer, size_t outMax) {
    std::snprintf(outVer, outMax, "1.1.0");
    FILE* vf = std::fopen(OTA_WWW_DIR "/version.json", "r");
    if (!vf) vf = std::fopen(PKG_WWW_DIR "/version.json", "r");
    if (!vf) return;

    char buf[1024];
    size_t nr = std::fread(buf, 1, sizeof(buf) - 1, vf);
    buf[nr] = 0;
    std::fclose(vf);

    char* vPos = std::strstr(buf, "\"version\":");
    if (vPos) {
        char* q1 = std::strchr(vPos + 10, '\"');
        if (q1) {
            char* q2 = std::strchr(q1 + 1, '\"');
            if (q2 && static_cast<size_t>(q2 - q1 - 1) < outMax) {
                std::memcpy(outVer, q1 + 1, static_cast<size_t>(q2 - q1 - 1));
                outVer[q2 - q1 - 1] = 0;
            }
        }
    }
}

void* precacheKeyThread(void* arg) {
    (void)arg;
    uint8_t dummy[64];
    fetchEncKey(nullptr, dummy, sizeof(dummy));
    return nullptr;
}

} // anonymous namespace

bool LocalEngineServer::start(uint16_t port) {
    if (s_running.load()) return true;

    std::signal(SIGPIPE, SIG_IGN);
    reconcileOtaDirectory();
    initOrbisHttp();

    pthread_t kth;
    if (pthread_create(&kth, nullptr, precacheKeyThread, nullptr) == 0) {
        pthread_detach(kth);
    }

    s_serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (s_serverFd < 0) {
        app_log("[PS4 Engine] socket() failed: errno=%d\n", errno);
        return false;
    }

    int opt = 1;
    setsockopt(s_serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(s_serverFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        app_log("[PS4 Engine] bind(%u) failed: errno=%d\n", port, errno);
        close(s_serverFd);
        s_serverFd = -1;
        return false;
    }

    if (listen(s_serverFd, 64) < 0) {
        app_log("[PS4 Engine] listen() failed: errno=%d\n", errno);
        close(s_serverFd);
        s_serverFd = -1;
        return false;
    }

    s_running.store(true);
    s_ready.store(true);

    if (pthread_create(&s_acceptThread, nullptr, acceptLoop, nullptr) != 0) {
        s_running.store(false);
        s_ready.store(false);
        close(s_serverFd);
        s_serverFd = -1;
        return false;
    }

    app_log("[PS4 Engine] Local HTTP/HLS Engine listening on 127.0.0.1:%u\n", port);
    return true;
}

bool LocalEngineServer::isReady() {
    return s_ready.load();
}

void LocalEngineServer::stop() {
    if (!s_running.load()) return;
    s_running.store(false);
    s_ready.store(false);
    if (s_serverFd >= 0) {
        close(s_serverFd);
        s_serverFd = -1;
    }
}

void* LocalEngineServer::acceptLoop(void* arg) {
    (void)arg;
    while (s_running.load()) {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = accept(s_serverFd, reinterpret_cast<struct sockaddr*>(&clientAddr), &clientLen);
        if (clientFd >= 0) {
            pthread_t worker;
            if (pthread_create(&worker, nullptr, clientWorker, reinterpret_cast<void*>(static_cast<intptr_t>(clientFd))) == 0) {
                pthread_detach(worker);
            } else {
                close(clientFd);
            }
        } else if (!s_running.load()) {
            break;
        }
    }
    return nullptr;
}

void* LocalEngineServer::clientWorker(void* arg) {
    int clientFd = static_cast<int>(reinterpret_cast<intptr_t>(arg));

#ifdef SO_NOSIGPIPE
    int nosig = 1;
    setsockopt(clientFd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
#endif
    struct timeval tv;
    tv.tv_sec = 15;
    tv.tv_usec = 0;
    setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char reqBuf[8192];
    int n = recv(clientFd, reqBuf, sizeof(reqBuf) - 1, 0);
    if (n <= 0) {
        close(clientFd);
        return nullptr;
    }
    reqBuf[n] = 0;

    if (std::strncmp(reqBuf, "OPTIONS ", 8) == 0) {
        sendCorsResponse(clientFd, 200, "text/plain", "");
        close(clientFd);
        return nullptr;
    }

    if (std::strncmp(reqBuf, "GET ", 4) != 0) {
        sendCorsResponse(clientFd, 405, "text/plain", "Method Not Allowed");
        close(clientFd);
        return nullptr;
    }

    char* pathStart = reqBuf + 4;
    char* pathEnd = std::strchr(pathStart, ' ');
    if (!pathEnd) {
        close(clientFd);
        return nullptr;
    }
    *pathEnd = 0;

    // 1. Health check
    if (std::strcmp(pathStart, "/health") == 0) {
        sendCorsResponse(clientFd, 200, "application/json",
            "{\"status\":\"ok\",\"service\":\"PStream Native PS4 Engine\",\"version\":\"1.1.0\",\"platform\":\"ps4\"}");
        close(clientFd);
        return nullptr;
    }

    // 2. Logging endpoints
    if (std::strncmp(pathStart, "/log", 4) == 0) {
        char decoded[LOG_LINE_LEN];
        if (getQueryParam(pathStart, "msg", decoded, sizeof(decoded))) {
            addWebLog(decoded);
        }
        sendCorsResponse(clientFd, 200, "text/plain", "OK");
        close(clientFd);
        return nullptr;
    }

    if (std::strcmp(pathStart, "/api/logs") == 0) {
        char* bigLogs = static_cast<char*>(std::malloc(65536));
        if (bigLogs) {
            bigLogs[0] = 0;
            size_t offset = 0;
            pthread_mutex_lock(&g_logMutex);
            int count = g_logCount < MAX_LOG_LINES ? g_logCount : MAX_LOG_LINES;
            int startIdx = g_logCount < MAX_LOG_LINES ? 0 : (g_logCount % MAX_LOG_LINES);
            for (int i = 0; i < count; i++) {
                int idx = (startIdx + i) % MAX_LOG_LINES;
                int written = std::snprintf(bigLogs + offset, 65536 - offset, "%s\n", g_logLines[idx]);
                if (written <= 0 || offset + static_cast<size_t>(written) >= 65535) break;
                offset += static_cast<size_t>(written);
            }
            pthread_mutex_unlock(&g_logMutex);
            sendCorsResponse(clientFd, 200, "text/plain; charset=utf-8", bigLogs);
            std::free(bigLogs);
        } else {
            sendCorsResponse(clientFd, 500, "text/plain", "OOM");
        }
        close(clientFd);
        return nullptr;
    }

    // 3. TMDb API TLS Proxy (/api/tmdb?path=...)
    if (std::strncmp(pathStart, "/api/tmdb", 9) == 0) {
        char tmdbPath[1536] = {0};
        if (!getQueryParam(pathStart, "path", tmdbPath, sizeof(tmdbPath)) || tmdbPath[0] != '/') {
            sendCorsResponse(clientFd, 400, "application/json", "{\"error\":\"Invalid path\"}");
            close(clientFd);
            return nullptr;
        }
        char fullUrl[2048];
        std::snprintf(fullUrl, sizeof(fullUrl), "https://api.themoviedb.org/3%s", tmdbPath);
        char* jsonBuf = static_cast<char*>(std::malloc(512 * 1024));
        if (!jsonBuf) {
            sendCorsResponse(clientFd, 500, "application/json", "{\"error\":\"OOM\"}");
            close(clientFd);
            return nullptr;
        }
        int rlen = httpFetch(fullUrl, "https://www.themoviedb.org/", jsonBuf, 512 * 1024);
        if (rlen > 0) {
            sendCorsResponse(clientFd, 200, "application/json; charset=utf-8", jsonBuf);
        } else {
            sendCorsResponse(clientFd, 502, "application/json", "{\"error\":\"Upstream TMDb error\"}");
        }
        std::free(jsonBuf);
        close(clientFd);
        return nullptr;
    }

    // 4. TMDb Image TLS Proxy (/api/img?size=w500&path=/abc.jpg)
    if (std::strncmp(pathStart, "/api/img", 8) == 0) {
        char sizeStr[32] = "w500";
        char imgPath[512] = {0};
        getQueryParam(pathStart, "size", sizeStr, sizeof(sizeStr));
        if (!getQueryParam(pathStart, "path", imgPath, sizeof(imgPath)) || imgPath[0] != '/') {
            sendCorsResponse(clientFd, 400, "text/plain", "Invalid image path");
            close(clientFd);
            return nullptr;
        }
        char imgUrl[1024];
        std::snprintf(imgUrl, sizeof(imgUrl), "https://image.tmdb.org/t/p/%s%s", sizeStr, imgPath);
        const char* mime = std::strstr(imgPath, ".png") ? "image/png" : "image/jpeg";
        if (httpStreamToClient(clientFd, imgUrl, "https://www.themoviedb.org/",
                               mime, "public, max-age=86400") < 0) {
            sendCorsResponse(clientFd, 502, "text/plain", "Image proxy error");
        }
        close(clientFd);
        return nullptr;
    }

    // 5. Stream Resolver (/api/stream/resolve)
    if (std::strncmp(pathStart, "/api/stream/resolve", 19) == 0) {
        char id[64] = {0};
        char type[16] = "movie";
        char sBuf[16] = "1";
        char eBuf[16] = "1";
        char lang[16] = "en";
        char spBuf[8] = "1"; // Default segproxy=1 on PS4 so WebKit avoids upstream SSL & .html MIME issues

        getQueryParam(pathStart, "id", id, sizeof(id));
        getQueryParam(pathStart, "type", type, sizeof(type));
        getQueryParam(pathStart, "season", sBuf, sizeof(sBuf));
        getQueryParam(pathStart, "episode", eBuf, sizeof(eBuf));
        if (!getQueryParam(pathStart, "lang", lang, sizeof(lang))) {
            getQueryParam(pathStart, "provider", lang, sizeof(lang));
        }
        getQueryParam(pathStart, "segproxy", spBuf, sizeof(spBuf));

        int season = std::atoi(sBuf) > 0 ? std::atoi(sBuf) : 1;
        int episode = std::atoi(eBuf) > 0 ? std::atoi(eBuf) : 1;
        int segproxy = (std::strcmp(spBuf, "0") == 0) ? 0 : 1;

        if (id[0]) {
            char rawStreamUrl[2048] = {0};
            if (extractVixStream(id, type, season, episode, lang, rawStreamUrl, sizeof(rawStreamUrl)) == 0) {
                char encUrl[4096];
                urlEncode(rawStreamUrl, encUrl, sizeof(encUrl));
                char resJson[5120];
                std::snprintf(resJson, sizeof(resJson),
                    "{\"status\":\"ok\",\"streamUrl\":\"%s/api/stream/master.m3u8?url=%s%s\",\"type\":\"hls\"}",
                    HOST_PREFIX, encUrl, segproxy ? "&segproxy=1" : ""
                );
                sendCorsResponse(clientFd, 200, "application/json", resJson);
                close(clientFd);
                return nullptr;
            }
        }

        sendCorsResponse(clientFd, 500, "application/json",
                         "{\"status\":\"error\",\"error\":\"Stream resolution failed\"}");
        close(clientFd);
        return nullptr;
    }

    // 6. Master Playlist Proxy (/api/stream/master.m3u8?url=...)
    if (std::strncmp(pathStart, "/api/stream/master.m3u8", 23) == 0) {
        char targetUrl[2048] = {0};
        char spBuf[8] = "1";
        if (!getQueryParam(pathStart, "url", targetUrl, sizeof(targetUrl)) || !targetUrl[0]) {
            sendCorsResponse(clientFd, 400, "text/plain", "Missing url parameter");
            close(clientFd);
            return nullptr;
        }
        getQueryParam(pathStart, "segproxy", spBuf, sizeof(spBuf));
        int segproxy = (std::strcmp(spBuf, "0") == 0) ? 0 : 1;

        char* rawMaster = static_cast<char*>(std::malloc(512 * 1024));
        char* rewrittenMaster = static_cast<char*>(std::malloc(1024 * 1024));
        if (!rawMaster || !rewrittenMaster) {
            if (rawMaster) std::free(rawMaster);
            if (rewrittenMaster) std::free(rewrittenMaster);
            sendCorsResponse(clientFd, 500, "text/plain", "OOM");
            close(clientFd);
            return nullptr;
        }

        int mLen = httpFetch(targetUrl, "https://vixsrc.to/", rawMaster, 512 * 1024);
        if (mLen <= 0) {
            std::free(rawMaster);
            std::free(rewrittenMaster);
            sendCorsResponse(clientFd, 502, "text/plain", "Failed to fetch master playlist");
            close(clientFd);
            return nullptr;
        }

        rewriteMasterPlaylist(rawMaster, targetUrl, segproxy, rewrittenMaster, 1024 * 1024);
        std::free(rawMaster);

        sendCorsResponse(clientFd, 200, "application/vnd.apple.mpegurl; charset=utf-8", rewrittenMaster);
        std::free(rewrittenMaster);
        close(clientFd);
        return nullptr;
    }

    // 7. Sub-Playlist Proxy (/api/stream/sub.m3u8?url=...)
    if (std::strncmp(pathStart, "/api/stream/sub.m3u8", 20) == 0) {
        char targetUrl[2048] = {0};
        char spBuf[8] = "1";
        if (!getQueryParam(pathStart, "url", targetUrl, sizeof(targetUrl)) || !targetUrl[0]) {
            sendCorsResponse(clientFd, 400, "text/plain", "Missing url parameter");
            close(clientFd);
            return nullptr;
        }
        getQueryParam(pathStart, "segproxy", spBuf, sizeof(spBuf));
        int segproxy = (std::strcmp(spBuf, "0") == 0) ? 0 : 1;

        char* rawSub = static_cast<char*>(std::malloc(2 * 1024 * 1024));
        char* rewrittenSub = static_cast<char*>(std::malloc(6 * 1024 * 1024));
        if (!rawSub || !rewrittenSub) {
            if (rawSub) std::free(rawSub);
            if (rewrittenSub) std::free(rewrittenSub);
            sendCorsResponse(clientFd, 500, "text/plain", "OOM");
            close(clientFd);
            return nullptr;
        }

        int sLen = httpFetch(targetUrl, "https://vixsrc.to/", rawSub, 2 * 1024 * 1024);
        if (sLen <= 0) {
            std::free(rawSub);
            std::free(rewrittenSub);
            sendCorsResponse(clientFd, 502, "text/plain", "Failed to fetch sub playlist");
            close(clientFd);
            return nullptr;
        }

        rewriteSubPlaylist(rawSub, targetUrl, segproxy, rewrittenSub, 6 * 1024 * 1024);
        std::free(rawSub);

        sendCorsResponse(clientFd, 200, "application/vnd.apple.mpegurl; charset=utf-8", rewrittenSub);
        std::free(rewrittenSub);
        close(clientFd);
        return nullptr;
    }

    // 8. AES-128 Key Proxy (/api/stream/key?url=...)
    if (std::strncmp(pathStart, "/api/stream/key", 15) == 0) {
        char keyUrl[1024] = {0};
        getQueryParam(pathStart, "url", keyUrl, sizeof(keyUrl));

        uint8_t keyBytes[64];
        int kLen = fetchEncKey(keyUrl[0] ? keyUrl : nullptr, keyBytes, sizeof(keyBytes));
        if (kLen >= 16) {
            char header[512];
            std::snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
                "Access-Control-Allow-Headers: *\r\n"
                "Content-Type: application/octet-stream\r\n"
                "Content-Length: %d\r\n"
                "Connection: close\r\n\r\n",
                kLen
            );
            if (sendAll(clientFd, header, std::strlen(header)) == 0) {
                sendAll(clientFd, keyBytes, static_cast<size_t>(kLen));
            }
        } else {
            sendCorsResponse(clientFd, 502, "text/plain", "Failed to fetch enc.key");
        }
        close(clientFd);
        return nullptr;
    }

    // 9. Streaming Segment Relay (/api/stream/seg.ts?url=...)
    if (std::strncmp(pathStart, "/api/stream/seg.ts", 18) == 0) {
        char segUrl[2048] = {0};
        if (!getQueryParam(pathStart, "url", segUrl, sizeof(segUrl)) || !segUrl[0]) {
            sendCorsResponse(clientFd, 400, "text/plain", "Missing url parameter");
            close(clientFd);
            return nullptr;
        }
        if (httpStreamToClient(clientFd, segUrl, "https://vixsrc.to/",
                               "video/mp2t", "no-store") < 0) {
            sendCorsResponse(clientFd, 502, "text/plain", "Segment relay failed");
        }
        close(clientFd);
        return nullptr;
    }

    // 10. Check Update (/api/check_update)
    if (std::strncmp(pathStart, "/api/check_update", 17) == 0) {
        char remoteVerBuf[2048];
        std::memset(remoteVerBuf, 0, sizeof(remoteVerBuf));
        int rLen = httpFetch(
            "https://raw.githubusercontent.com/ducky-ux1/PStream/main/ps5/www/version.json",
            "https://github.com", remoteVerBuf, sizeof(remoteVerBuf) - 1);

        char localVer[64];
        readLocalVersion(localVer, sizeof(localVer));

        if (rLen > 0) {
            char remoteVer[64] = "";
            char* vPos = std::strstr(remoteVerBuf, "\"version\":");
            if (vPos) {
                char* q1 = std::strchr(vPos + 10, '\"');
                if (q1) {
                    char* q2 = std::strchr(q1 + 1, '\"');
                    if (q2 && static_cast<size_t>(q2 - q1 - 1) < sizeof(remoteVer)) {
                        std::memcpy(remoteVer, q1 + 1, static_cast<size_t>(q2 - q1 - 1));
                        remoteVer[q2 - q1 - 1] = 0;
                    }
                }
            }
            bool hasUpdate = (remoteVer[0] && std::strcmp(remoteVer, localVer) != 0);
            char resp[1024];
            std::snprintf(resp, sizeof(resp),
                "{\"update_available\": %s, \"current_version\": \"%s\", \"latest_version\": \"%s\"}",
                hasUpdate ? "true" : "false", localVer, remoteVer[0] ? remoteVer : localVer);
            sendCorsResponse(clientFd, 200, "application/json", resp);
        } else {
            char resp[512];
            std::snprintf(resp, sizeof(resp),
                "{\"update_available\": false, \"current_version\": \"%s\", \"status\": \"offline\"}", localVer);
            sendCorsResponse(clientFd, 200, "application/json", resp);
        }
        close(clientFd);
        return nullptr;
    }

    // 11. Apply Update (/api/apply_update) -> writes OTA updates to /data/PStream/www/
    if (std::strncmp(pathStart, "/api/apply_update", 17) == 0) {
        mkdir("/data/PStream", 0777);
        mkdir(OTA_WWW_DIR, 0777);

        const char* filesToUpdate[] = { "app.js", "app.css", "index.html", "input.js", "version.json" };
        int numFiles = static_cast<int>(sizeof(filesToUpdate) / sizeof(filesToUpdate[0]));
        int updatedCount = 0;

        for (int i = 0; i < numFiles; i++) {
            char ghUrl[512];
            std::snprintf(ghUrl, sizeof(ghUrl),
                "https://raw.githubusercontent.com/ducky-ux1/PStream/main/ps5/www/%s",
                filesToUpdate[i]);

            char* fileBuf = static_cast<char*>(std::malloc(1024 * 1024));
            if (!fileBuf) continue;

            int dlLen = httpFetch(ghUrl, "https://github.com", fileBuf, (1024 * 1024) - 1);
            if (dlLen > 0) {
                char dstPath[512];
                std::snprintf(dstPath, sizeof(dstPath), "%s/%s", OTA_WWW_DIR, filesToUpdate[i]);
                FILE* df = std::fopen(dstPath, "wb");
                if (df) {
                    std::fwrite(fileBuf, 1, static_cast<size_t>(dlLen), df);
                    std::fclose(df);
                    updatedCount++;
                }
            }
            std::free(fileBuf);
        }

        char resp[512];
        std::snprintf(resp, sizeof(resp),
            "{\"success\": %s, \"updated_files\": %d, \"message\": \"%s\"}",
            (updatedCount > 0) ? "true" : "false", updatedCount,
            (updatedCount > 0) ? "Update applied successfully from GitHub!" : "Failed to download updates from GitHub.");
        sendCorsResponse(clientFd, 200, "application/json", resp);
        close(clientFd);
        return nullptr;
    }

    // 12. Static File Serving (/data/PStream/www -> fallback /app0/www)
    char cleanRel[512];
    if (std::strcmp(pathStart, "/") == 0 || std::strncmp(pathStart, "/?", 2) == 0 ||
        std::strncmp(pathStart, "/index.html", 11) == 0) {
        std::snprintf(cleanRel, sizeof(cleanRel), "index.html");
    } else {
        const char* rel = (pathStart[0] == '/') ? pathStart + 1 : pathStart;
        std::snprintf(cleanRel, sizeof(cleanRel), "%s", rel);
        char* qmark = std::strchr(cleanRel, '?');
        if (qmark) *qmark = 0;
        char* amp = std::strchr(cleanRel, '&');
        if (amp) *amp = 0;
        if (std::strstr(cleanRel, "..") != nullptr) {
            sendCorsResponse(clientFd, 400, "text/plain", "Invalid Path");
            close(clientFd);
            return nullptr;
        }
    }

    sendFileResponse(clientFd, cleanRel);
    close(clientFd);
    return nullptr;
}

} // namespace PStream
