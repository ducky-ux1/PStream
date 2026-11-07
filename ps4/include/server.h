#pragma once

#include <atomic>
#include <cstdint>
#include <pthread.h>

namespace PStream {

class LocalEngineServer {
public:
    static bool start(uint16_t port = 8086);
    static void stop();
    static bool isReady();

private:
    static void* acceptLoop(void* arg);
    static void* clientWorker(void* arg);

    static std::atomic<bool> s_running;
    static std::atomic<bool> s_ready;
    static int s_serverFd;
    static pthread_t s_acceptThread;
};

} // namespace PStream
