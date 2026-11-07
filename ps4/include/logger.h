#pragma once

#include <orbis/libkernel.h>
#include <cstdarg>
#include <cstdio>
#include <sys/stat.h>

inline void app_log(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    sceKernelDebugOutText(0, buf);
    fputs(buf, stdout);

    mkdir("/data/PStream", 0777);
    FILE* f = fopen("/data/PStream/pstream.log", "a");
    if (f) {
        fputs(buf, f);
        fclose(f);
    }
}
