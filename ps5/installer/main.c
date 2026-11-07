/* PStream PS5 Media App Dashboard Installer & Standalone Daemon Deployment (v1.1.0) */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#ifndef TITLE_ID
#define TITLE_ID "PSTR00001"
#endif

#define IOVEC_SIZE(x) (sizeof(x) / sizeof(struct iovec))
#define IOVEC_ENTRY(x) {x ? (void*)(x) : 0, x ? strlen(x) + 1 : 0}

#define INCASSET(name, file)			\
  __asm__(".section .rodata\n"			\
	  ".global " #name "\n"			\
	  ".global " #name "_end\n"		\
	  ".global " #name "_size\n"		\
	  ".align 16\n"				\
	  #name ":\n"				\
	  ".incbin \"" file "\"\n"		\
	  #name "_end:\n"			\
	  ".quad " #name "_end - " #name "\n"	\
	  ".previous\n");			\
  extern const uint8_t name[];

/* Explicit size symbol helper matching original layout */
#undef INCASSET
#define INCASSET(name, file)			\
  __asm__(".section .rodata\n"			\
	  ".global " #name "\n"			\
	  ".global " #name "_end\n"		\
	  ".global " #name "_size\n"		\
	  ".align 16\n"				\
	  #name ":\n"				\
	  ".incbin \"" file "\"\n"		\
	  #name "_end:\n"			\
	  #name "_size:\n"			\
	  ".quad " #name "_end - " #name "\n"	\
	  ".previous\n");			\
  extern const uint8_t name[];			\
  extern const size_t name##_size;

int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallAll(void*);
int sceAppInstUtilAppUnInstall(const char*);
int sceNetInit(void);
int sceNetPoolCreate(const char*, int, int);

INCASSET(param, "sce_sys/param.json");
INCASSET(icon0, "sce_sys/icon0.png");
INCASSET(pic1, "sce_sys/pic1.png");
INCASSET(index_html, "../www/index.html");
INCASSET(app_js, "../www/app.js");
INCASSET(app_css, "../www/app.css");
INCASSET(input_js, "../www/input.js");
INCASSET(hls_js, "../www/hls.min.js");
INCASSET(logo_png, "../www/logo.png");
INCASSET(favicon_png, "../www/favicon.png");
INCASSET(version_json, "../www/version.json");
INCASSET(pstream_service, "../scripts/pstream-service.elf");

static void log_msg(const char* msg) {
    printf("%s\n", msg);
    klog_puts(msg);
}

static int remount_system_ex(void) {
    struct iovec iov[] = {
        IOVEC_ENTRY("from"),      IOVEC_ENTRY("/dev/ssd0.system_ex"),
        IOVEC_ENTRY("fspath"),    IOVEC_ENTRY("/system_ex"),
        IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("exfatfs"),
        IOVEC_ENTRY("large"),     IOVEC_ENTRY("yes"),
        IOVEC_ENTRY("timezone"),  IOVEC_ENTRY("static"),
        IOVEC_ENTRY("async"),     IOVEC_ENTRY(NULL),
        IOVEC_ENTRY("ignoreacl"), IOVEC_ENTRY(NULL),
    };
    return nmount(iov, IOVEC_SIZE(iov), MNT_UPDATE);
}

static int install_file(const char* path, const uint8_t* data, size_t size) {
    unlink(path);
    FILE* f = fopen(path, "wb");
    if (!f) {
        printf("[PStream Installer] Failed to open for writing: %s (errno %d)\n", path, errno);
        return -1;
    }
    if (data && size) {
        if (fwrite(data, size, 1, f) != 1) {
            printf("[PStream Installer] Failed to write %zu bytes to: %s\n", size, path);
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}

static int copy_disk_file(const char* src_path, const char* dst_path) {
    if (!src_path || !dst_path) return -1;
    if (strcmp(src_path, dst_path) == 0) return 0;
    FILE* sf = fopen(src_path, "rb");
    if (!sf) return -1;
    unlink(dst_path);
    FILE* df = fopen(dst_path, "wb");
    if (!df) {
        fclose(sf);
        return -1;
    }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), sf)) > 0) {
        fwrite(buf, 1, n, df);
    }
    fclose(df);
    fclose(sf);
    return 0;
}

static void prepend_autoload_entry(const char* autoload_path, const char* entry_name) {
    char existing[8192];
    memset(existing, 0, sizeof(existing));
    FILE* rf = fopen(autoload_path, "r");
    if (rf) {
        size_t nr = fread(existing, 1, sizeof(existing) - 1, rf);
        existing[nr] = 0;
        fclose(rf);
    }

    FILE* wf = fopen(autoload_path, "w");
    if (!wf) return;

    fprintf(wf, "%s\n", entry_name);
    char* saveptr = NULL;
    char* line = strtok_r(existing, "\r\n", &saveptr);
    while (line) {
        if (line[0] != 0 && !strstr(line, entry_name)) {
            fprintf(wf, "%s\n", line);
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }
    fclose(wf);
}

static int install_app(const char* title_id) {
    printf("[PStream Installer] Registering %s with Sony AppInstUtil...\n", title_id);
    return sceAppInstUtilAppInstallAll(0);
}

static void configure_autoload(const char* current_elf_path) {
    mkdir("/data", 0777);
    mkdir("/data/pldmgr", 0777);
    mkdir("/data/pldmgr/payloads", 0777);
    mkdir("/data/ps5_autoloader", 0777);

    install_file("/data/pldmgr/payloads/pstream-service.elf", pstream_service, pstream_service_size);
    install_file("/data/ps5_autoloader/pstream-service.elf", pstream_service, pstream_service_size);

    // Preserve existing user entries in both autoload.txt locations
    prepend_autoload_entry("/data/pldmgr/autoload.txt", "pstream-service.elf");
    prepend_autoload_entry("/data/ps5_autoloader/autoload.txt", "pstream-service.elf");
    log_msg("[PStream Installer] Configured pstream-service.elf in autoloaders (existing entries preserved)");

    // Optional etaHEN daemons directory support
    if (access("/data/etaHEN", F_OK) == 0) {
        mkdir("/data/etaHEN/daemons", 0777);
        install_file("/data/etaHEN/daemons/pstream-service.elf", pstream_service, pstream_service_size);
        log_msg("[PStream Installer] Deployed pstream-service.elf to /data/etaHEN/daemons/");
    }

    const char* target_inst = "/data/pldmgr/payloads/PStream-PayloadManager.elf";
    const char* candidates[] = {
        current_elf_path,
        "/mnt/usb0/PStream-PayloadManager.elf",
        "/mnt/usb0/PStream-PS5Upload.elf",
        "/mnt/usb0/PStream-Installer.elf",
        "/mnt/usb0/pstream-install.elf",
        "/mnt/usb1/PStream-PayloadManager.elf",
        "/mnt/usb1/PStream-PS5Upload.elf",
        "/mnt/usb1/PStream-Installer.elf",
        "/mnt/usb1/pstream-install.elf",
        "/data/pldmgr/payloads/PStream-PayloadManager.elf",
        "/data/pldmgr/payloads/PStream-Installer.elf",
        "/data/pldmgr/payloads/pstream-install.elf"
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (candidates[i] && access(candidates[i], F_OK) == 0) {
            if (copy_disk_file(candidates[i], target_inst) == 0) {
                log_msg("[PStream Installer] Updated PStream-PayloadManager.elf in /data/pldmgr/payloads/");
                break;
            }
        }
    }
}

static int query_daemon(const char* endpoint, char* out_buf, size_t out_max) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    struct timeval tv;
    tv.tv_sec = 8;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(8086);
    srv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(s, (struct sockaddr*)&srv, sizeof(srv)) != 0) {
        close(s);
        return -1;
    }

    char req[512];
    snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: 127.0.0.1:8086\r\nConnection: close\r\n\r\n", endpoint);
    send(s, req, strlen(req), 0);

    size_t total = 0;
    if (out_buf && out_max > 1) {
        while (total < out_max - 1) {
            ssize_t n = recv(s, out_buf + total, out_max - 1 - total, 0);
            if (n <= 0) break;
            total += (size_t)n;
        }
        out_buf[total] = 0;
    }
    close(s);
    return (int)total;
}

static void restart_service_with_latest(void) {
    sceNetInit();
    sceNetPoolCreate("pstream_inst_net", 1024 * 1024, 0);

    // Stop any previously running older daemon so v1.1.0 binds port 8086 cleanly
    char resp[256];
    if (query_daemon("/shutdown", resp, sizeof(resp)) >= 0) {
        log_msg("[PStream Installer] Stopped previous daemon instance on port 8086.");
        usleep(450000);
    }

    log_msg("[PStream Installer] Launching native daemon v1.1.0 via local elfldr (9021)...");
    int ldr = socket(AF_INET, SOCK_STREAM, 0);
    if (ldr >= 0) {
        struct sockaddr_in ldr_addr;
        memset(&ldr_addr, 0, sizeof(ldr_addr));
        ldr_addr.sin_family = AF_INET;
        ldr_addr.sin_port = htons(9021);
        ldr_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (connect(ldr, (struct sockaddr*)&ldr_addr, sizeof(ldr_addr)) == 0) {
            size_t total = 0;
            while (total < pstream_service_size) {
                ssize_t n = send(ldr, pstream_service + total, pstream_service_size - total, 0);
                if (n <= 0) break;
                total += (size_t)n;
            }
            shutdown(ldr, SHUT_WR);
            log_msg("[PStream Installer] Daemon v1.1.0 sent to elfldr (Port 8086 active).");
        } else {
            log_msg("[PStream Installer] Note: elfldr port 9021 not reachable (daemon will start via autoload).");
        }
        close(ldr);
    }
}

static void refresh_installed_assets(void) {
    log_msg("[PStream Installer] Deploying core assets and updated param.json...");

    mkdir("/system_ex/app/"TITLE_ID, 0755);
    mkdir("/system_ex/app/"TITLE_ID"/sce_sys", 0755);
    mkdir("/user/app/"TITLE_ID, 0755);
    mkdir("/user/app/"TITLE_ID"/sce_sys", 0755);
    mkdir("/user/appmeta/"TITLE_ID, 0755);

    // System_ex application files
    install_file("/system_ex/app/"TITLE_ID"/eboot.bin", 0, 0);
    install_file("/system_ex/app/"TITLE_ID"/sce_sys/param.json", param, param_size);
    install_file("/system_ex/app/"TITLE_ID"/sce_sys/icon0.png", icon0, icon0_size);
    install_file("/system_ex/app/"TITLE_ID"/sce_sys/pic1.png", pic1, pic1_size);
    install_file("/system_ex/app/"TITLE_ID"/index.html", index_html, index_html_size);
    install_file("/system_ex/app/"TITLE_ID"/app.js", app_js, app_js_size);
    install_file("/system_ex/app/"TITLE_ID"/app.css", app_css, app_css_size);
    install_file("/system_ex/app/"TITLE_ID"/input.js", input_js, input_js_size);
    install_file("/system_ex/app/"TITLE_ID"/hls.min.js", hls_js, hls_js_size);
    install_file("/system_ex/app/"TITLE_ID"/logo.png", logo_png, logo_png_size);
    install_file("/system_ex/app/"TITLE_ID"/favicon.png", favicon_png, favicon_png_size);
    install_file("/system_ex/app/"TITLE_ID"/version.json", version_json, version_json_size);

    // User application files
    install_file("/user/app/"TITLE_ID"/sce_sys/param.json", param, param_size);
    install_file("/user/app/"TITLE_ID"/sce_sys/icon0.png", icon0, icon0_size);
    install_file("/user/app/"TITLE_ID"/sce_sys/pic1.png", pic1, pic1_size);
    install_file("/user/app/"TITLE_ID"/index.html", index_html, index_html_size);
    install_file("/user/app/"TITLE_ID"/app.js", app_js, app_js_size);
    install_file("/user/app/"TITLE_ID"/app.css", app_css, app_css_size);
    install_file("/user/app/"TITLE_ID"/input.js", input_js, input_js_size);
    install_file("/user/app/"TITLE_ID"/hls.min.js", hls_js, hls_js_size);
    install_file("/user/app/"TITLE_ID"/logo.png", logo_png, logo_png_size);
    install_file("/user/app/"TITLE_ID"/favicon.png", favicon_png, favicon_png_size);
    install_file("/user/app/"TITLE_ID"/version.json", version_json, version_json_size);
    install_file("/user/app/"TITLE_ID"/pstream-service.elf", pstream_service, pstream_service_size);

    // App metadata
    install_file("/user/appmeta/"TITLE_ID"/param.json", param, param_size);
    install_file("/user/appmeta/"TITLE_ID"/icon0.png", icon0, icon0_size);
    install_file("/user/appmeta/"TITLE_ID"/pic1.png", pic1, pic1_size);

    log_msg("[PStream Installer] Core files synchronized.");
}

int main(int argc, char *argv[]) {
    int err;
    const char* my_path = (argc > 0) ? argv[0] : NULL;

    signal(SIGPIPE, SIG_IGN);

    log_msg("=====================================================");
    log_msg("  PStream PS5 Standalone Installer & Updater v1.1.0  ");
    log_msg("=====================================================");

    if ((err = sceAppInstUtilInitialize())) {
        printf("[PStream] sceAppInstUtilInitialize: error 0x%08X\n", err);
        return -1;
    }

    remount_system_ex();

    int was_installed = (access("/user/app/"TITLE_ID"/version.json", F_OK) == 0 ||
                         access("/user/app/"TITLE_ID"/app.js", F_OK) == 0);

    if (was_installed) {
        log_msg("[PStream Installer] Existing installation detected. Refreshing registration & daemon...");
    } else {
        log_msg("[PStream Installer] Fresh install detected. Performing full deployment...");
    }

    // Purge stale ShellCore app.db registration (e.g. old TinyURL webAppUri) before re-registering
    sceAppInstUtilAppUnInstall("FAKE00000");
    sceAppInstUtilAppUnInstall(TITLE_ID);

    // Deploy embedded v1.1.0 assets & GitHub Pages param.json
    refresh_installed_assets();

    // Configure autoloaders safely (preserving existing user entries)
    configure_autoload(my_path);

    // Restart background daemon with v1.1.0 binary
    restart_service_with_latest();
    usleep(500000);

    // Check GitHub for any newer OTA web frontend bundle
    char update_buf[4096];
    int q_len = query_daemon("/api/check_update", update_buf, sizeof(update_buf));
    if (q_len > 0 && (strstr(update_buf, "\"update_available\": true") ||
                      strstr(update_buf, "\"update_available\":true"))) {
        log_msg("[PStream Installer] Newer web assets found on GitHub! Applying OTA update...");
        char apply_buf[4096];
        if (query_daemon("/api/apply_update", apply_buf, sizeof(apply_buf)) > 0 &&
            strstr(apply_buf, "\"success\": true")) {
            log_msg("[PStream Installer] >>> OTA web assets updated from GitHub! <<<");
        }
    }

    // Register dashboard tile in PS5 Media Apps bar
    if ((err = install_app(TITLE_ID))) {
        printf("[PStream Installer] install_app: error 0x%08X\n", err);
        return -1;
    }

    log_msg("[PStream Installer] >>> PSTREAM v1.1.0 READY! <<<");
    log_msg("[PStream Installer] Launch PStream from your PS5 Media tab.");
    log_msg("=====================================================");
    return 0;
}
