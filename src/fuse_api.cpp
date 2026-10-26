#include "fuse_api.h"

#if defined(__linux__)

#include <dlfcn.h>

#include <cstdlib>
#include <mutex>
#include <vector>

// The version must be pinned before <fuse.h> is read; fuse_common.cpp uses the
// same value so the operation tables agree. The header declares some entry
// points as macros/inlines in newer releases, which is why the symbols below
// are declared here with their stable signatures instead of being taken from
// the header.
#define FUSE_USE_VERSION 35
#include <fuse.h>

namespace {

using NewFn = ::fuse* (*)(struct fuse_args*, const struct fuse_operations*, size_t, void*);
using MountFn = int (*)(::fuse*, const char*);
using UnmountFn = void (*)(::fuse*);
using DestroyFn = void (*)(::fuse*);
using LoopFn = int (*)(::fuse*);
using ExitFn = void (*)(::fuse*);
using OptAddArgFn = int (*)(struct fuse_args*, const char*);
using OptFreeArgsFn = void (*)(struct fuse_args*);
using ContextFn = struct fuse_context* (*)();

struct Table {
    NewFn newFs = nullptr;
    MountFn mount = nullptr;
    UnmountFn unmount = nullptr;
    DestroyFn destroy = nullptr;
    LoopFn loop = nullptr;
    ExitFn exitFs = nullptr;
    OptAddArgFn optAddArg = nullptr;
    OptFreeArgsFn optFreeArgs = nullptr;
    ContextFn context = nullptr;
};

Table g_table;
void* g_handle = nullptr;
std::string g_error;
std::string g_loaded;
bool g_ready = false;
std::once_flag g_once;

template <typename Slot>
bool bindSymbol(Slot& slot, const char* name) {
    void* symbol = ::dlsym(g_handle, name);
    if (symbol == nullptr) {
        g_error = std::string("libfuse3 is missing symbol ") + name;
        return false;
    }
    slot = reinterpret_cast<Slot>(symbol);
    return true;
}

// libfuse 3.18 replaced the plain "fuse_new" export with ABI-versioned
// aliases, so dlsym() finds nothing under that name; the 4-argument entry
// point is exported as "fuse_new_30" instead. The FUSE_3.1 alias takes an
// extra version pointer and must never be called with four arguments.
bool bindNewFs() {
    if (void* symbol = ::dlsym(g_handle, "fuse_new"); symbol != nullptr) {
        g_table.newFs = reinterpret_cast<NewFn>(symbol);
        return true;
    }
    if (void* symbol = ::dlsym(g_handle, "fuse_new_30"); symbol != nullptr) {
        g_table.newFs = reinterpret_cast<NewFn>(symbol);
        return true;
    }
    g_error = "libfuse3 is missing symbol fuse_new";
    return false;
}

bool bindAll() {
    return bindNewFs() &&
           bindSymbol(g_table.mount, "fuse_mount") &&
           bindSymbol(g_table.unmount, "fuse_unmount") &&
           bindSymbol(g_table.destroy, "fuse_destroy") &&
           bindSymbol(g_table.loop, "fuse_loop") &&
           bindSymbol(g_table.exitFs, "fuse_exit") &&
           bindSymbol(g_table.optAddArg, "fuse_opt_add_arg") &&
           bindSymbol(g_table.optFreeArgs, "fuse_opt_free_args") &&
           bindSymbol(g_table.context, "fuse_get_context");
}

std::vector<std::string> candidates() {
    // Explicit override: point at a specific library, or at a missing path to
    // verify the "fuse unavailable" degradation path. When set it is the only
    // candidate, so a bad path really simulates a system without libfuse3.
    if (const char* override = std::getenv("AFM_FUSE_LIB");
        override != nullptr && *override != '\0') {
        return {override};
    }
    return {"libfuse3.so.4", "libfuse3.so.3", "libfuse3.so"};
}

void load() {
    const std::vector<std::string> names = candidates();
    std::string tried;
    for (const std::string& name : names) {
        void* handle = ::dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) {
            if (!tried.empty()) tried += ", ";
            tried += name;
            continue;
        }
        g_handle = handle;
        if (bindAll()) {
            g_ready = true;
            g_loaded = name;
            return;
        }
        ::dlclose(handle);
        g_handle = nullptr;
    }
    if (g_error.empty()) {
        g_error = "libfuse3 not found (tried " + tried +
                  "); install the fuse3 package to enable mounting";
    }
}

}  // namespace

namespace afm::fuse::api {

bool ensureLoaded() {
    std::call_once(g_once, load);
    return g_ready;
}

const std::string& loadError() { return g_error; }

const std::string& loadedLibrary() { return g_loaded; }

FuseHandle* newFs(struct fuse_args* args, const struct fuse_operations* ops, size_t size,
                  void* data) {
    return g_table.newFs(args, ops, size, data);
}

int mount(FuseHandle* handle, const char* mountPoint) {
    return g_table.mount(handle, mountPoint);
}

void unmount(FuseHandle* handle) { g_table.unmount(handle); }

void destroy(FuseHandle* handle) { g_table.destroy(handle); }

int loop(FuseHandle* handle) { return g_table.loop(handle); }

void exitFs(FuseHandle* handle) { g_table.exitFs(handle); }

int optAddArg(struct fuse_args* args, const char* arg) {
    return g_table.optAddArg(args, arg);
}

void optFreeArgs(struct fuse_args* args) { g_table.optFreeArgs(args); }

struct fuse_context* context() { return g_table.context(); }

}  // namespace afm::fuse::api

#endif  // defined(__linux__)
