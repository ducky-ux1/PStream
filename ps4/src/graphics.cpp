#include "graphics.h"
#include "logger.h"
#include <orbis/VideoOut.h>
#include <cstring>

namespace PStream {

BackdropDisplay::BackdropDisplay(int width, int height)
    : m_width(width)
    , m_height(height)
    , m_videoHandle(-1)
    , m_frameBufferSize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4)
    , m_directMemSize(0)
    , m_directMemOff(0)
    , m_videoMem(nullptr)
    , m_buffers{nullptr, nullptr}
    , m_flipQueue(0)
    , m_activeIdx(0)
    , m_flipArg(1)
    , m_initialized(false)
{
}

BackdropDisplay::~BackdropDisplay() {
    shutdown();
}

bool BackdropDisplay::init() {
    if (m_initialized) return true;

    m_videoHandle = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, nullptr);
    if (m_videoHandle < 0) {
        app_log("[BackdropDisplay] sceVideoOutOpen failed: 0x%08X\n", m_videoHandle);
        return false;
    }

    if (sceKernelCreateEqueue(&m_flipQueue, "pstream_flip_q") == 0) {
        sceVideoOutAddFlipEvent(m_flipQueue, m_videoHandle, nullptr);
    }

    size_t totalBytes = m_frameBufferSize * 2;
    size_t alignment = 0x200000;
    m_directMemSize = ((totalBytes + alignment - 1) / alignment) * alignment;

    int rc = sceKernelAllocateDirectMemory(
        0,
        sceKernelGetDirectMemorySize(),
        m_directMemSize,
        alignment,
        ORBIS_KERNEL_WB_ONION,
        &m_directMemOff
    );
    if (rc < 0) {
        app_log("[BackdropDisplay] sceKernelAllocateDirectMemory failed: 0x%08X\n", rc);
        sceVideoOutClose(m_videoHandle);
        m_videoHandle = -1;
        return false;
    }

    rc = sceKernelMapDirectMemory(
        &m_videoMem,
        m_directMemSize,
        ORBIS_KERNEL_PROT_CPU_RW | ORBIS_KERNEL_PROT_GPU_RW,
        0,
        m_directMemOff,
        alignment
    );
    if (rc < 0) {
        app_log("[BackdropDisplay] sceKernelMapDirectMemory failed: 0x%08X\n", rc);
        sceKernelReleaseDirectMemory(m_directMemOff, m_directMemSize);
        m_directMemSize = 0;
        sceVideoOutClose(m_videoHandle);
        m_videoHandle = -1;
        return false;
    }

    m_buffers[0] = static_cast<char*>(m_videoMem);
    m_buffers[1] = static_cast<char*>(m_videoMem) + m_frameBufferSize;

    // Fill both buffers with PStream obsidian (#0A0A0F -> 0x800A0A0F)
    const uint32_t bgPixel = 0x800A0A0Fu;
    size_t pixelCount = static_cast<size_t>(m_width) * static_cast<size_t>(m_height);
    uint32_t* fb0 = reinterpret_cast<uint32_t*>(m_buffers[0]);
    uint32_t* fb1 = reinterpret_cast<uint32_t*>(m_buffers[1]);
    for (size_t i = 0; i < pixelCount; i++) {
        fb0[i] = bgPixel;
        fb1[i] = bgPixel;
    }

    OrbisVideoOutBufferAttribute attr;
    std::memset(&attr, 0, sizeof(attr));
    sceVideoOutSetBufferAttribute(&attr, 0x80000000, 1, 0, m_width, m_height, m_width);

    rc = sceVideoOutRegisterBuffers(m_videoHandle, 0, reinterpret_cast<void**>(m_buffers), 2, &attr);
    if (rc < 0) {
        app_log("[BackdropDisplay] sceVideoOutRegisterBuffers failed: 0x%08X\n", rc);
        return false;
    }

    sceVideoOutSetFlipRate(m_videoHandle, 0);
    m_initialized = true;
    renderBackdrop();
    app_log("[BackdropDisplay] 1920x1080 VideoOut initialized.\n");
    return true;
}

void BackdropDisplay::renderBackdrop() {
    if (!m_initialized || m_videoHandle < 0) return;

    int64_t frameId = m_flipArg++;
    sceVideoOutSubmitFlip(m_videoHandle, m_activeIdx, ORBIS_VIDEO_OUT_FLIP_VSYNC, frameId);

    for (int tries = 0; tries < 4; tries++) {
        OrbisVideoOutFlipStatus status;
        if (sceVideoOutGetFlipStatus(m_videoHandle, &status) == 0 && status.flipArg >= frameId) {
            break;
        }
        if (m_flipQueue) {
            OrbisKernelEvent evt;
            int outCount = 0;
            OrbisKernelUseconds timeout = 20000; // 20ms max wait
            sceKernelWaitEqueue(m_flipQueue, &evt, 1, &outCount, &timeout);
        }
    }

    m_activeIdx = (m_activeIdx + 1) % 2;
}

void BackdropDisplay::shutdown() {
    if (!m_initialized) return;

    if (m_videoHandle >= 0) {
        sceVideoOutClose(m_videoHandle);
        m_videoHandle = -1;
    }
    if (m_directMemSize > 0) {
        sceKernelReleaseDirectMemory(m_directMemOff, m_directMemSize);
        m_directMemSize = 0;
        m_directMemOff = 0;
        m_videoMem = nullptr;
    }
    m_initialized = false;
}

} // namespace PStream
