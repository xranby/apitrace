#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>

void (*__apitraceNewFramePtr) (void);
int initialized = 0;
void apitraceNewFrame()
{
    if (!initialized)
        __apitraceNewFramePtr = dlsym(0, "apitraceNewFrame");
    if (__apitraceNewFramePtr)
        __apitraceNewFramePtr();
}
