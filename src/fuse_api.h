// Runtime binding for libfuse3.
//
// libfuse changed its shared-library SONAME from libfuse3.so.3 to
// libfuse3.so.4 in the 3.18 release, and 3.18 also replaced the public
// fuse_new() entry point with an ABI-versioned inline. Linking against either
// SONAME makes the binary refuse to start on distributions that ship the other
// one — and the application would not launch at all on those systems, even for
// users who never mount a device.
//
// The library is therefore loaded at runtime through its stable exported
// symbols (fuse_new, fuse_mount, ... — all present since FUSE 3.0), trying
// every known SONAME. FUSE is an optional feature: when no compatible library
// is present the GUI still starts, browsing and transfers work, and only the
// mount action reports why it is unavailable.
#pragma once

#if defined(__linux__)

#include <cstddef>
#include <string>

// Opaque libfuse types. The real declarations come from <fuse.h>, which every
// translation unit that needs them includes itself.
struct fuse;
struct fuse_args;
struct fuse_context;
struct fuse_operations;

namespace afm::fuse::api {

// `fuse` cannot be spelled unqualified inside this namespace (afm::fuse is the
// enclosing namespace), so the handle gets an alias.
using FuseHandle = ::fuse;

// Loads libfuse3 on first use. Returns false and sets loadError() when no
// compatible library is available on this system.
bool ensureLoaded();

// Human-readable reason the library could not be loaded.
const std::string& loadError();

// The SONAME that was actually loaded, for diagnostics ("libfuse3.so.4").
const std::string& loadedLibrary();

// Thin forwarding wrappers around the stable ABI. Call them only after
// ensureLoaded() returned true.
FuseHandle* newFs(struct fuse_args* args, const struct fuse_operations* ops,
                  size_t size, void* data);
int mount(FuseHandle* handle, const char* mountPoint);
void unmount(FuseHandle* handle);
void destroy(FuseHandle* handle);
int loop(FuseHandle* handle);
void exitFs(FuseHandle* handle);
int optAddArg(struct fuse_args* args, const char* arg);
void optFreeArgs(struct fuse_args* args);
struct fuse_context* context();

}  // namespace afm::fuse::api

#endif  // defined(__linux__)
