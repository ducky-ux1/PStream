/* PStream PS5 Clean Uninstaller Payload (v1.1.0) */
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include <ps5/klog.h>

#ifndef TITLE_ID
#define TITLE_ID "PSTR00001"
#endif

#define IOVEC_SIZE(x) (sizeof(x) / sizeof(struct iovec))
#define IOVEC_ENTRY(x) {x ? (void*)(x) : 0, x ? strlen(x) + 1 : 0}

int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppUnInstall(const char*);
int sceNetInit(void);
int sceNetPoolCreate(const char*, int, int);

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

static void stop_running_daemon(void) {
    sceNetInit();
    sceNetPoolCreate("pstream_uninst_net", 512 * 1024, 0);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return;

    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(8086);
    srv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(s, (struct sockaddr*)&srv, sizeof(srv)) == 0) {
        const char* req = "GET /shutdown HTTP/1.1\r\nHost: 127.0.0.1:8086\r\nConnection: close\r\n\r\n";
        send(s, req, strlen(req), 0);
        char buf[128];
        recv(s, buf, sizeof(buf), 0);
        log_msg("[PStream Uninstaller] Stopped background PStream service on port 8086.");
    }
    close(s);
    usleep(300000);
}

static void remove_autoload_entry(const char* autoload_path) {
    FILE* rf = fopen(autoload_path, "r");
    if (!rf) return;

    char existing[8192];
    memset(existing, 0, sizeof(existing));
    size_t nr = fread(existing, 1, sizeof(existing) - 1, rf);
    existing[nr] = 0;
    fclose(rf);

    FILE* wf = fopen(autoload_path, "w");
    if (!wf) return;

    char* saveptr = NULL;
    char* line = strtok_r(existing, "\r\n", &saveptr);
    while (line) {
        if (line[0] != 0 && !strstr(line, "pstream-service.elf")) {
            fprintf(wf, "%s\n", line);
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }
    fclose(wf);
}

static void remove_app_dir_files(const char* base_dir) {
    const char* files[] = {
        "eboot.bin", "index.html", "app.js", "app.css", "input.js",
        "hls.min.js", "logo.png", "favicon.png", "version.json",
        "pstream-service.elf", "param.json", "icon0.png", "pic1.png",
        "sce_sys/param.json", "sce_sys/icon0.png", "sce_sys/pic1.png"
    };
    char path[512];
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", base_dir, files[i]);
        unlink(path);
    }
    snprintf(path, sizeof(path), "%s/sce_sys", base_dir);
    rmdir(path);
    rmdir(base_dir);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    log_msg("=====================================================");
    log_msg("         PStream PS5 Clean Uninstaller v1.1.0        ");
    log_msg("=====================================================");

    stop_running_daemon();

    if (sceAppInstUtilInitialize() == 0) {
        int rc = sceAppInstUtilAppUnInstall(TITLE_ID);
        printf("[PStream Uninstaller] sceAppInstUtilAppUnInstall("TITLE_ID") -> 0x%08X\n", rc);
    }

    remount_system_ex();

    remove_app_dir_files("/system_ex/app/"TITLE_ID);
    remove_app_dir_files("/user/app/"TITLE_ID);
    remove_app_dir_files("/user/appmeta/"TITLE_ID);

    unlink("/data/pldmgr/payloads/pstream-service.elf");
    unlink("/data/pldmgr/payloads/PStream-PayloadManager.elf");
    unlink("/data/pldmgr/payloads/PStream-Installer.elf");
    unlink("/data/pldmgr/payloads/pstream-install.elf");
    unlink("/data/ps5_autoloader/pstream-service.elf");
    unlink("/data/etaHEN/daemons/pstream-service.elf");

    remove_autoload_entry("/data/pldmgr/autoload.txt");
    remove_autoload_entry("/data/ps5_autoloader/autoload.txt");

    log_msg("[PStream Uninstaller] PStream ("TITLE_ID") and its background daemon have been completely removed.");
    log_msg("=====================================================");
    return 0;
}
