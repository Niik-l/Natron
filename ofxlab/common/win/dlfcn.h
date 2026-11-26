// Windows shim for the three dl* calls ZDefocusZ's OpenCL loader makes (local test build).
#ifndef OFXLAB_DLFCN_SHIM_H
#define OFXLAB_DLFCN_SHIM_H
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define RTLD_LAZY 0
#define RTLD_GLOBAL 0
static inline int dlclose(void *h) { return FreeLibrary((HMODULE)h) ? 0 : -1; }
static inline void *dlopen(const char *name, int) { return (void *)LoadLibraryA(name); }
static inline void *dlsym(void *h, const char *sym) { return (void *)GetProcAddress((HMODULE)h, sym); }
static inline const char *dlerror() { return 0; }
#endif
#endif
