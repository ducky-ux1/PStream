#pragma once

#include <orbis/libkernel.h>
#include <cstddef>
#include <cstdint>

namespace PStream {

class BackdropDisplay {
public:
    BackdropDisplay(int width = 1920, int height = 1080);
    ~BackdropDisplay();

    bool init();
    void renderBackdrop();
    void shutdown();

private:
    int m_width;
    int m_height;
    int m_videoHandle;
    size_t m_frameBufferSize;
    size_t m_directMemSize;
    off_t m_directMemOff;
    void* m_videoMem;
    char* m_buffers[2];
    OrbisKernelEqueue m_flipQueue;
    int m_activeIdx;
    int64_t m_flipArg;
    bool m_initialized;
};

} // namespace PStream
