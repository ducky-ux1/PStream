#include "graphics.h"
#include "logger.h"
#include "ps4_web_browser.h"
#include "server.h"

#include <orbis/CommonDialog.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>

#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace {

void loadRequiredSysmodules() {
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_USER_SERVICE);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_PAD);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NETCTL);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_COMMON_DIALOG);

    sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG);
    int rcWb = sceSysmoduleLoadModule(ORBIS_SYSMODULE_WEB_BROWSER_DIALOG);
    app_log("[MAIN] Loaded sysmodules (WEB_BROWSER_DIALOG rc=0x%08X)\n", rcWb);
}

int32_t resolveActiveUserId() {
    sceUserServiceInitialize(nullptr);
    int32_t userId = -1;
    if (sceUserServiceGetInitialUser(&userId) != 0 || userId < 0) {
        sceUserServiceGetForegroundUser(&userId);
    }
    if (userId < 0) {
        OrbisUserServiceLoginUserIdList list;
        std::memset(&list, 0, sizeof(list));
        if (sceUserServiceGetLoginUserIdList(&list) == 0) {
            for (int i = 0; i < 4; i++) {
                if (list.userId[i] >= 0) {
                    userId = list.userId[i];
                    break;
                }
            }
        }
    }
    app_log("[MAIN] Active Orbis UserId: 0x%08X\n", userId);
    return userId;
}

bool openFullscreenPStreamView(int32_t userId, PStreamWbCallbackInitParam* cbInit, PStreamWbWebViewParam* wvParam) {
    static const char* APP_URL = "http://127.0.0.1:8086/index.html?platform=ps4";

    std::memset(cbInit, 0, sizeof(*cbInit));
    cbInit->size = sizeof(*cbInit);
    cbInit->type = PSTREAM_WB_CALLBACK_URL;
    cbInit->data = "pstream:";

    std::memset(wvParam, 0, sizeof(*wvParam));
    wvParam->size = sizeof(*wvParam);
    wvParam->option = PSTREAM_WB_WEBVIEW_OPT_NONE;

    // Attempt 1: 100% borderless 1920x1080 custom canvas (zero browser chrome, D-pad spatial nav)
    PStreamWbDialogParam param;
    pstreamWbParamInit(&param);
    param.mode = PSTREAM_WB_MODE_CUSTOM;
    param.userId = userId;
    param.url = APP_URL;
    param.callbackInitParam = cbInit;
    param.width = 1920;
    param.height = 1080;
    param.positionX = 0;
    param.positionY = 0;
    param.parts = PSTREAM_WB_PARTS_NONE;
    param.control = PSTREAM_WB_CONTROL_NONE;
    param.webviewParam = wvParam;
    param.animation = PSTREAM_WB_ANIMATION_DISABLE;

    int32_t rc = sceWebBrowserDialogOpen(&param);
    if (rc == 0) {
        app_log("[MAIN] Opened borderless 1920x1080 WebBrowserDialog (CONTROL_NONE).\n");
        return true;
    }
    app_log("[MAIN] Borderless CONTROL_NONE returned 0x%08X, retrying with CONTROL_EXIT...\n", rc);

    // Attempt 2: Borderless 1920x1080 custom canvas with EXIT button bit enabled (required on some FW revisions)
    pstreamWbParamInit(&param);
    param.mode = PSTREAM_WB_MODE_CUSTOM;
    param.userId = userId;
    param.url = APP_URL;
    param.callbackInitParam = cbInit;
    param.width = 1920;
    param.height = 1080;
    param.positionX = 0;
    param.positionY = 0;
    param.parts = PSTREAM_WB_PARTS_NONE;
    param.control = PSTREAM_WB_CONTROL_EXIT;
    param.webviewParam = wvParam;
    param.animation = PSTREAM_WB_ANIMATION_DISABLE;

    rc = sceWebBrowserDialogOpen(&param);
    if (rc == 0) {
        app_log("[MAIN] Opened borderless 1920x1080 WebBrowserDialog (CONTROL_EXIT).\n");
        return true;
    }
    app_log("[MAIN] Borderless CONTROL_EXIT returned 0x%08X, falling back to MODE_DEFAULT...\n", rc);

    // Attempt 3: Default fullscreen mode fallback
    pstreamWbParamInit(&param);
    param.mode = PSTREAM_WB_MODE_DEFAULT;
    param.userId = userId;
    param.url = APP_URL;
    param.callbackInitParam = cbInit;

    rc = sceWebBrowserDialogOpen(&param);
    app_log("[MAIN] MODE_DEFAULT returned 0x%08X\n", rc);
    return (rc == 0);
}

} // namespace

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    app_log("\n=====================================================\n");
    app_log("  PStream PlayStation 4 Standalone Edition v1.1.0\n");
    app_log("=====================================================\n");

    loadRequiredSysmodules();
    int32_t userId = resolveActiveUserId();

    // 1. Bring up 1080p VideoOut backdrop and dismiss PS4 splash screen
    PStream::BackdropDisplay display(1920, 1080);
    if (!display.init()) {
        app_log("[MAIN] FATAL: VideoOut initialization failed.\n");
        for (;;) {
            sceKernelUsleep(1000000);
        }
    }

    // 2. Start embedded C++ HTTP + HLS stream engine on 127.0.0.1:8086
    if (!PStream::LocalEngineServer::start(8086)) {
        app_log("[MAIN] FATAL: Failed to start LocalEngineServer on port 8086.\n");
        for (;;) {
            display.renderBackdrop();
            sceKernelUsleep(16666);
        }
    }

    sceSystemServiceHideSplashScreen();

    // 3. Initialize CommonDialog + WebBrowserDialog
    int32_t rcCd = sceCommonDialogInitialize();
    int32_t rcWb = sceWebBrowserDialogInitialize();
    app_log("[MAIN] CommonDialogInit=0x%08X, WebBrowserDialogInit=0x%08X\n", rcCd, rcWb);

    PStreamWbCallbackInitParam cbInit;
    PStreamWbWebViewParam wvParam;
    bool browserOpen = openFullscreenPStreamView(userId, &cbInit, &wvParam);

    uint32_t frameTick = 0;
    while (browserOpen) {
        OrbisCommonDialogStatus status = sceWebBrowserDialogUpdateStatus();
        if (status == ORBIS_COMMON_DIALOG_STATUS_FINISHED) {
            PStreamWbCallbackResultParam cbRes;
            std::memset(&cbRes, 0, sizeof(cbRes));
            cbRes.size = sizeof(cbRes);

            PStreamWbDialogResult res;
            std::memset(&res, 0, sizeof(res));
            res.callbackResultParam = &cbRes;
            sceWebBrowserDialogGetResult(&res);

            app_log("[MAIN] WebBrowserDialog finished (result=%d, callback='%s')\n",
                    res.result, cbRes.data);
            break;
        }

        // Flip dark backdrop & keep PS4 screen-saver / rest timer awake during movie playback
        display.renderBackdrop();
        if (++frameTick % 60 == 0) {
            sceSystemServicePowerTick();
            sceSystemServiceTickVideoPlayback();
        }
    }

    sceWebBrowserDialogTerminate();
    PStream::LocalEngineServer::stop();
    display.shutdown();

    app_log("[MAIN] Exiting PStream PS4 cleanly.\n");
    sceSystemServiceLoadExec("exit", nullptr);
    return 0;
}
