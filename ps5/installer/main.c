/* PStream PS5 Media App Dashboard Installer & Standalone Daemon Deployment */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#ifndef TITLE_ID
#define TITLE_ID "PSTR00001"
#endif

#define IOVEC_SIZE(x) (sizeof(x) / sizeof(struct iovec))
#define IOVEC_ENTRY(x) {x ? x : 0, x ? strlen(x)+1 : 0}

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
  if(!f) {
    printf("[PStream Installer] Failed to open for writing: %s (errno %d)\n", path, errno);
    return -1;
  }

  if(data && size) {
    if(fwrite(data, size, 1, f) != 1) {
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

static int install_app(const char* title_id, const char* dir) {
  printf("[PStream Installer] Registering %s with Sony AppInstUtil...\n", title_id);
  return sceAppInstUtilAppInstallAll(0);
}

static void configure_autoload(const char* current_elf_path) {
    mkdir("/data", 0777);
    mkdir("/data/pldmgr", 0777);
    mkdir("/data/pldmgr/payloads", 0777);

    mkdir("/data/ps5_autoloader", 0777);

    // Deploy pstream-service.elf into /data/pldmgr/payloads/ and /data/ps5_autoloader/
    install_file("/data/pldmgr/payloads/pstream-service.elf", pstream_service, pstream_service_size);
    install_file("/data/ps5_autoloader/pstream-service.elf", pstream_service, pstream_service_size);

    // Read existing /data/pldmgr/autoload.txt
    char existing_pldmgr[4096] = {0};
    FILE* f = fopen("/data/pldmgr/autoload.txt", "r");
    if (f) {
        fread(existing_pldmgr, 1, sizeof(existing_pldmgr) - 1, f);
        fclose(f);
    }

    // Always ensure pstream-service.elf is at the TOP of autoload.txt
    FILE* af = fopen("/data/pldmgr/autoload.txt", "w");
    if (af) {
        fprintf(af, "pstream-service.elf\n");
        char* line = strtok(existing_pldmgr, "\r\n");
        while (line) {
            if (!strstr(line, "pstream-service.elf")) {
                fprintf(af, "%s\n", line);
            }
            line = strtok(NULL, "\r\n");
        }
        fclose(af);
        log_msg("[PStream Installer] Configured pstream-service.elf at top of /data/pldmgr/autoload.txt");
    }

    // Also configure standard /data/ps5_autoloader/autoload.txt
    FILE* af2 = fopen("/data/ps5_autoloader/autoload.txt", "w");
    if (af2) {
        fprintf(af2, "pstream-service.elf\n");
        fclose(af2);
        log_msg("[PStream Installer] Configured pstream-service.elf in /data/ps5_autoloader/autoload.txt");
    }

    // Deploy installer into /data/pldmgr/payloads/PStream-PayloadManager.elf so Payload Manager displays it
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
    int copied = 0;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (candidates[i] && access(candidates[i], F_OK) == 0) {
            if (copy_disk_file(candidates[i], target_inst) == 0) {
                log_msg("[PStream Installer] Configured PStream-PayloadManager.elf in Payload Manager (/data/pldmgr/payloads/)");
                copied = 1;
                break;
            }
        }
    }
    if (!copied && access(target_inst, F_OK) == 0) {
        log_msg("[PStream Installer] PStream-PayloadManager.elf already present in /data/pldmgr/payloads/");
    }
}

static void start_service_if_needed(void) {
    sceNetInit();
    sceNetPoolCreate("pstream_inst_net", 1024 * 1024, 0);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s >= 0) {
        struct sockaddr_in srv;
        memset(&srv, 0, sizeof(srv));
        srv.sin_family = AF_INET;
        srv.sin_port = htons(8086);
        srv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (connect(s, (struct sockaddr*)&srv, sizeof(srv)) == 0) {
            log_msg("[PStream Installer] Native daemon is already running on port 8086.");
            close(s);
            return;
        }
        close(s);
    }

    log_msg("[PStream Installer] Launching native daemon via local elfldr (9021)...");
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
                total += n;
            }
            shutdown(ldr, SHUT_WR);
            log_msg("[PStream Installer] Daemon sent to elfldr (Port 8086 starting).");
        } else {
            log_msg("[PStream Installer] Note: elfldr port 9021 not reachable (daemon will run via pldmgr autoload).");
        }
        close(ldr);
    }
}

static int query_daemon(const char* endpoint, char* out_buf, size_t out_max) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

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
    while (total < out_max - 1) {
        ssize_t n = recv(s, out_buf + total, out_max - 1 - total, 0);
        if (n <= 0) break;
        total += n;
    }
    out_buf[total] = 0;
    close(s);
    return (int)total;
}

static void refresh_installed_assets(void) {
    log_msg("[PStream Installer] Synchronizing core assets from embedded payload...");

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

    log_msg("[PStream Installer] Local files successfully synchronized.");
}

int main(int argc, char *argv[]) {
    int err;
    const char* my_path = (argc > 0) ? argv[0] : NULL;

    log_msg("=====================================================");
    log_msg("  PStream PS5 Standalone Installer & Updater v1.0.2  ");
    log_msg("=====================================================");

    if((err=sceAppInstUtilInitialize())) {
        printf("[PStream] sceAppInstUtilInitialize: error 0x%08X\n", err);
        return -1;
    }

    remount_system_ex();

    // Check if PStream is already installed
    int is_installed = (access("/user/app/"TITLE_ID"/version.json", F_OK) == 0 ||
                        access("/user/app/"TITLE_ID"/app.js", F_OK) == 0);

    if (is_installed) {
        log_msg("[PStream Installer] Existing installation detected.");
        log_msg("[PStream Installer] Checking for updates and ensuring service health...");

        // Ensure autoload & Payload Manager placement
        configure_autoload(my_path);

        // Ensure service is running
        start_service_if_needed();
        usleep(500000); // 500ms delay to allow port 8086 binding

        // Check for updates on GitHub via native daemon
        char update_buf[4096];
        int q_len = query_daemon("/api/check_update", update_buf, sizeof(update_buf));
        int updated_online = 0;

        if (q_len > 0) {
            if (strstr(update_buf, "\"update_available\": true") || strstr(update_buf, "\"update_available\":true")) {
                log_msg("[PStream Installer] Newer update detected on GitHub! Downloading latest files...");
                char apply_buf[4096];
                int a_len = query_daemon("/api/apply_update", apply_buf, sizeof(apply_buf));
                if (a_len > 0 && strstr(apply_buf, "\"success\": true")) {
                    log_msg("[PStream Installer] >>> SUCCESS: PStream updated directly from GitHub! <<<");
                    updated_online = 1;
                } else {
                    log_msg("[PStream Installer] GitHub update download failed; refreshing from embedded payload.");
                }
            } else if (strstr(update_buf, "\"update_available\": false") || strstr(update_buf, "\"update_available\":false")) {
                log_msg("[PStream Installer] App is already up to date with latest GitHub release.");
            } else {
                log_msg("[PStream Installer] GitHub unreachable (offline). Proceeding with embedded refresh.");
            }
        } else {
            log_msg("[PStream Installer] Daemon not answering update query; proceeding with embedded refresh.");
        }

        if (!updated_online) {
            refresh_installed_assets();
        }

        // Re-verify registration with Sony Media Shell
        install_app(TITLE_ID, "/user/app/");
        log_msg("[PStream Installer] Update verification complete! Enjoy PStream.");
        log_msg("=====================================================");
        return 0;
    }

    // FRESH INSTALLATION FLOW
    log_msg("[PStream Installer] Fresh install detected. Performing full deployment...");

    sceAppInstUtilAppUnInstall("FAKE00000");
    sceAppInstUtilAppUnInstall(TITLE_ID);

    // Create System & User app directories
    mkdir("/system_ex/app/"TITLE_ID, 0755);
    mkdir("/system_ex/app/"TITLE_ID"/sce_sys", 0755);
    mkdir("/user/app/"TITLE_ID, 0755);
    mkdir("/user/app/"TITLE_ID"/sce_sys", 0755);
    mkdir("/user/appmeta/"TITLE_ID, 0755);

    // Install all core assets
    refresh_installed_assets();

    // Configure autostart and Payload Manager
    configure_autoload(my_path);

    // Launch daemon
    start_service_if_needed();

    // Register dashboard tile with Sony Media Shell
    if((err=install_app(TITLE_ID, "/user/app/"))) {
        printf("[PStream Installer] install_app: error 0x%08X\n", err);
        return -1;
    }

    log_msg("[PStream Installer] >>> INSTALLATION SUCCESSFUL! <<<");
    log_msg("[PStream Installer] PStream has been added to your PS5 Media Apps bar.");
    log_msg("=====================================================");
    return 0;
}
