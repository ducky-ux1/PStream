#pragma once

#include <orbis/CommonDialog.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Orbis SceWebBrowserDialog ABI definitions (verified 64-bit Orbis OS layout)
#define PSTREAM_WB_URL_SIZE                (2048)
#define PSTREAM_WB_URL_SIZE_EXTENDED       (8192)
#define PSTREAM_WB_DOMAIN_COUNT            (20)

#define PSTREAM_WB_MODE_DEFAULT            (1)
#define PSTREAM_WB_MODE_CUSTOM             (2)

#define PSTREAM_WB_CALLBACK_NONE           (0)
#define PSTREAM_WB_CALLBACK_URL            (1)

#define PSTREAM_WB_PARTS_NONE              (0)
#define PSTREAM_WB_PARTS_TITLE             (1u << 0)
#define PSTREAM_WB_PARTS_ADDRESS           (1u << 1)
#define PSTREAM_WB_PARTS_FOOTER            (1u << 2)
#define PSTREAM_WB_PARTS_BACKGROUND        (1u << 3)
#define PSTREAM_WB_PARTS_WAIT_DIALOG       (1u << 4)

#define PSTREAM_WB_CONTROL_NONE            (0)
#define PSTREAM_WB_CONTROL_EXIT            (1u << 0)
#define PSTREAM_WB_CONTROL_RELOAD          (1u << 1)
#define PSTREAM_WB_CONTROL_BACK            (1u << 2)
#define PSTREAM_WB_CONTROL_FORWARD         (1u << 3)

#define PSTREAM_WB_WEBVIEW_OPT_NONE        (0)
#define PSTREAM_WB_WEBVIEW_OPT_TRANSPARENT (1u << 0)
#define PSTREAM_WB_WEBVIEW_OPT_CURSOR_NONE (1u << 1)

#define PSTREAM_WB_ANIMATION_DEFAULT       (0)
#define PSTREAM_WB_ANIMATION_DISABLE       (1)

struct PStreamWbCallbackInitParam {
    size_t size;
    int32_t type;
    int : 32;
    const char* data;
    char reserved[32];
} __attribute__((__aligned__(8)));

struct PStreamWbCallbackResultParam {
    size_t size;
    int32_t type;
    char data[PSTREAM_WB_URL_SIZE];
    uint8_t reserved0[4];
    char* buffer;
    uint32_t bufferSize;
    char reserved[20];
} __attribute__((__aligned__(8)));

struct PStreamWbImeParam {
    size_t size;
    uint32_t option;
    char reserved[256];
    int : 32;
} __attribute__((__aligned__(8)));

struct PStreamWbWebViewParam {
    size_t size;
    uint32_t option;
    char reserved[256];
    int : 32;
} __attribute__((__aligned__(8)));

struct PStreamWbDialogParam {
    OrbisCommonDialogBaseParam baseParam;
    size_t size;
    int32_t mode;
    int32_t userId;
    const char* url;
    PStreamWbCallbackInitParam* callbackInitParam;
    uint16_t width;
    uint16_t height;
    uint16_t positionX;
    uint16_t positionY;
    uint32_t parts;
    uint16_t headerWidth;
    uint16_t headerPositionX;
    uint16_t headerPositionY;
    short : 16;
    uint32_t control;
    PStreamWbImeParam* imeParam;
    PStreamWbWebViewParam* webviewParam;
    uint32_t animation;
    char reserved[202];
    short : 16;
};

struct PStreamWbDialogResult {
    int32_t result;
    int : 32;
    PStreamWbCallbackResultParam* callbackResultParam;
    char reserved[240];
};

static_assert(sizeof(OrbisCommonDialogBaseParam) == 48, "OrbisCommonDialogBaseParam ABI mismatch");
static_assert(sizeof(PStreamWbCallbackInitParam) == 56, "PStreamWbCallbackInitParam ABI mismatch");
static_assert(sizeof(PStreamWbWebViewParam) == 272, "PStreamWbWebViewParam ABI mismatch");
static_assert(sizeof(PStreamWbDialogParam) == 328, "PStreamWbDialogParam ABI mismatch");
static_assert(sizeof(PStreamWbDialogResult) == 256, "PStreamWbDialogResult ABI mismatch");

inline void pstreamWbParamInit(PStreamWbDialogParam* param) {
    std::memset(param, 0, sizeof(PStreamWbDialogParam));
    param->baseParam.size = static_cast<uint32_t>(sizeof(OrbisCommonDialogBaseParam));
    param->baseParam.magic = static_cast<uint32_t>(
        ORBIS_COMMON_DIALOG_MAGIC_NUMBER + reinterpret_cast<uint64_t>(&param->baseParam));
    param->size = sizeof(PStreamWbDialogParam);
}

extern "C" {
int32_t sceWebBrowserDialogInitialize(void);
int32_t sceWebBrowserDialogOpen(const PStreamWbDialogParam* param);
OrbisCommonDialogStatus sceWebBrowserDialogUpdateStatus(void);
OrbisCommonDialogStatus sceWebBrowserDialogGetStatus(void);
int32_t sceWebBrowserDialogGetResult(PStreamWbDialogResult* result);
int32_t sceWebBrowserDialogClose(void);
int32_t sceWebBrowserDialogTerminate(void);
}
