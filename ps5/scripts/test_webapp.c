#include <stdio.h>
#include <stdlib.h>

int sceUserServiceInitialize(void*);
int sceUserServiceTerminate(void);
int sceSystemServiceLaunchWebApp(const char *uri, void*);

int main() {
    if(sceUserServiceInitialize(0)) {
        perror("sceUserServiceInitialize");
    }
    printf("Calling sceSystemServiceLaunchWebApp...\n");
    int ret = sceSystemServiceLaunchWebApp("http://192.168.1.197:8080/index.html", 0);
    printf("sceSystemServiceLaunchWebApp returned: 0x%08X\n", ret);
    sceUserServiceTerminate();
    return ret;
}
