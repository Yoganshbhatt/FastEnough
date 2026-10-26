cmake_minimum_required(VERSION 3.22)
project(FastEnoughLinux VERSION 1.0.21 LANGUAGES C CXX)

# Windows builds (vcxproj) use C++20; keep CMake identical. New Linux-only
# helpers stay at the lowest standard that compiles them warning-clean.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Strict warnings for all Linux-compiled code (AGENTS.md: zero warnings).
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/thirdparty/imgui)
if(NOT EXISTS ${IMGUI_DIR}/imgui.h)
  message(FATAL_ERROR "thirdparty/imgui missing; run ./setup.sh first")
endif()

# --- Portable core: files with no OS-specific calls --------------------------
add_library(afm_portable STATIC
  src/adb_discovery.cpp
  thirdparty/qrcodegen/qrcodegen.cpp
)
target_include_directories(afm_portable PUBLIC
  src
  thirdparty
  thirdparty/qrcodegen
)

# --- Smoke test: proves the toolchain + portable subset before any porting ----
add_executable(afm_smoke tools/afm_smoke.cpp)
target_link_libraries(afm_smoke PRIVATE afm_portable)

enable_testing()
add_test(NAME smoke COMMAND afm_smoke)

# --- Dear ImGui (fetched by setup.sh) + GLFW/OpenGL3 backends ----------------
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/imgui_demo.cpp
  ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_options(imgui PRIVATE -w)  # third-party: silence global -Werror
find_package(glfw3 REQUIRED)
find_package(OpenGL REQUIRED)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

# --- POSIX subprocess runner (new Linux API; Windows keeps process_runner.*) ---
add_library(afm_subprocess STATIC
  src/subprocess_posix.cpp
)
target_include_directories(afm_subprocess PUBLIC src)

add_executable(subprocess_test tools/subprocess_test.cpp)
target_link_libraries(subprocess_test PRIVATE afm_subprocess)
add_test(NAME subprocess COMMAND subprocess_test)

# --- Single-instance handoff (Unix socket; Windows uses WM_COPYDATA) ---------
add_library(afm_single_instance STATIC
  src/single_instance_linux.cpp
)
target_include_directories(afm_single_instance PUBLIC src)

add_executable(single_instance_test tools/single_instance_test.cpp)
target_link_libraries(single_instance_test PRIVATE afm_single_instance)
add_test(NAME single_instance COMMAND single_instance_test)

# --- POSIX socket layer (Linux-only) + loopback tests (no device needed) -----
add_library(afm_net STATIC
  src/net_posix.cpp
)
target_include_directories(afm_net PUBLIC src)

add_executable(net_test tools/net_test.cpp)
target_link_libraries(net_test PRIVATE afm_net)
add_test(NAME net COMMAND net_test)

add_executable(crc_test tools/crc_test.cpp)
target_link_libraries(crc_test PRIVATE afm_client_core)
add_test(NAME crc COMMAND crc_test)

# --- Wire-protocol invariants (src/protocol.h is a view of server/protocol.h)
add_executable(protocol_test tools/protocol_test.cpp)
target_include_directories(protocol_test PRIVATE src)
add_test(NAME protocol COMMAND protocol_test)

# --- DeviceClient transfer core, Linux body (Stage A: lifecycle + browse) ----
add_library(afm_client_core STATIC
  src/device_client_linux.cpp
  src/device_client_transfer.cpp
  src/device_client_crc.cpp
  src/device_client_relay.cpp
  src/device_client_extra.cpp
  src/device_client_backup_linux.cpp
  src/device_client_meta_linux.cpp
)
target_include_directories(afm_client_core PUBLIC src thirdparty)
target_link_libraries(afm_client_core PUBLIC afm_portable afm_subprocess afm_net)

# --- FUSE (Linux virtual drive) ---------------------------------------------
# --- MCRAW local decode/extract (GUI + FUSE view share one implementation) --
add_library(afm_mcraw STATIC
  src/mcraw_local.cpp
  thirdparty/motioncam/lib/Decoder.cpp
  thirdparty/motioncam/lib/RawData.cpp
  thirdparty/motioncam/lib/RawData_Legacy.cpp
)
set_source_files_properties(src/mcraw_local.cpp thirdparty/motioncam/lib/Decoder.cpp
  thirdparty/motioncam/lib/RawData.cpp thirdparty/motioncam/lib/RawData_Legacy.cpp
  PROPERTIES COMPILE_OPTIONS "-w")  # third-party decoder: silence global -Werror
target_include_directories(afm_mcraw PUBLIC
  src ${IMGUI_DIR} ${CMAKE_SOURCE_DIR}/thirdparty
  thirdparty/nlohmann thirdparty/audiofile
  thirdparty/motioncam/include thirdparty/tinydng)
target_link_libraries(afm_mcraw PUBLIC afm_portable pthread)

# libfuse3 is loaded at runtime (see src/fuse_api.h): the headers are needed to
# build, but the binary must not link the shared object, because its SONAME
# changed between 3.17 (libfuse3.so.3) and 3.18 (libfuse3.so.4) and a linked
# binary would refuse to start on the other half of the distributions.
option(AFM_REQUIRE_FUSE "Fail the build when the libfuse3 headers are missing" OFF)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(FUSE3 QUIET fuse3)
endif()
if(FUSE3_FOUND)
  add_library(afm_fuse STATIC
    src/fuse_api.cpp
    src/fuse_common.cpp
    src/fuse_paths.cpp
    src/fs_fuse.cpp
    src/mcraw_fuse.cpp
  )
  target_include_directories(afm_fuse PUBLIC src
    PRIVATE ${IMGUI_DIR} ${CMAKE_SOURCE_DIR}/thirdparty
            ${CMAKE_SOURCE_DIR}/thirdparty/nlohmann
            ${FUSE3_INCLUDE_DIRS})
  target_link_libraries(afm_fuse PUBLIC afm_client_core afm_mcraw ${CMAKE_DL_LIBS})
  target_compile_definitions(afm_fuse PUBLIC AFM_HAVE_FUSE=1)

  add_executable(fuse_test tools/fuse_test.cpp)
  target_link_libraries(fuse_test PRIVATE afm_fuse)
  add_test(NAME fuse COMMAND fuse_test)
  set_tests_properties(fuse PROPERTIES SKIP_RETURN_CODE 77)

  # Manual end-to-end test; requires a host afm-server on 127.0.0.1:5740.
  add_executable(fuse_device_test tools/fuse_device_test.cpp)
  target_link_libraries(fuse_device_test PRIVATE afm_fuse)
  add_executable(fuse_write_test tools/fuse_write_test.cpp)
  target_link_libraries(fuse_write_test PRIVATE afm_fuse)
  add_executable(mcraw_fuse_test tools/mcraw_fuse_test.cpp)
  target_link_libraries(mcraw_fuse_test PRIVATE afm_fuse)
  message(STATUS "FUSE: ${FUSE3_VERSION} (read-only and writable device mount enabled)")
elseif(AFM_REQUIRE_FUSE)
  message(FATAL_ERROR "libfuse3 not found; install fuse3 / libfuse3-dev")
else()
  message(STATUS "FUSE: not found; device mount disabled (install libfuse3-dev)")
endif()

# Stage device binaries beside the executables (mirrors the release layout
# where afm-server ships next to the binary; getServerBinaryPath finds it).
file(COPY ${CMAKE_SOURCE_DIR}/server/afm-server
     DESTINATION ${CMAKE_BINARY_DIR})
file(COPY ${CMAKE_SOURCE_DIR}/server/afm-labels.dex
     DESTINATION ${CMAKE_BINARY_DIR})

add_executable(afm-cli tools/afm_cli.cpp)
target_link_libraries(afm-cli PRIVATE afm_client_core)

# --- Install layout -----------------------------------------------------------
# The client looks for afm-server and afm-labels.dex next to its own executable
# (/proc/self/exe), so the binaries are installed together in
# <libdir>/fast-enough and <bindir> only carries symlinks. This is the single
# definition of the installed layout: packaging/arch/PKGBUILD and
# scripts/package-release.sh both end up with it.
include(GNUInstallDirs)
set(AFM_INSTALL_LIBDIR "${CMAKE_INSTALL_LIBDIR}/fast-enough")

function(afm_install_bin_symlink name)
  if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    set(link_target "${AFM_INSTALL_LIBDIR}/${name}")
  else()
    set(link_target "../${AFM_INSTALL_LIBDIR}/${name}")
  endif()
  install(CODE "
    file(MAKE_DIRECTORY \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\")
    execute_process(COMMAND \"${CMAKE_COMMAND}\" -E create_symlink
      \"${link_target}\"
      \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/${name}\")
  ")
endfunction()

install(TARGETS afm-cli RUNTIME DESTINATION "${AFM_INSTALL_LIBDIR}")
afm_install_bin_symlink(afm-cli)

# --- GUI (Phase 2): shared App code + GLFW/OpenGL3 shell ----------------------
# Build with: cmake -B build-gui -DAFM_BUILD_GUI=ON
option(AFM_BUILD_GUI "Build the fast-enough GUI" OFF)
if(AFM_BUILD_GUI)
# Optional StatusNotifier tray. GTK3 is needed to build it; the indicator
# library itself is loaded with dlopen at runtime, so the GUI still starts on
# systems without it. Without GTK3 headers the GUI builds with a tray stub.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(GTK3 QUIET gtk+-3.0)
endif()
add_executable(fast-enough
  src/app.cpp
  src/app_connections.cpp
  src/app_pairing.cpp
  src/app_sessions.cpp
  src/app_workspace.cpp
  src/backup_archive.cpp
  src/main_linux.cpp
  src/tray_linux.cpp
)
target_include_directories(fast-enough PRIVATE
  src
  thirdparty
  thirdparty/nlohmann
  thirdparty/audiofile
  thirdparty/motioncam/include
  thirdparty/tinydng
  thirdparty/qrcodegen)
target_link_libraries(fast-enough PRIVATE
  afm_client_core afm_portable afm_platform afm_single_instance imgui afm_subprocess
  afm_mcraw pthread dl)
if(FUSE3_FOUND)
  target_link_libraries(fast-enough PRIVATE afm_fuse)
endif()
target_compile_definitions(fast-enough PRIVATE AFM_RES_DIR="${CMAKE_SOURCE_DIR}/res")
if(GTK3_FOUND)
  target_compile_definitions(fast-enough PRIVATE AFM_HAVE_GTK=1)
  target_include_directories(fast-enough PRIVATE ${GTK3_INCLUDE_DIRS})
  target_link_directories(fast-enough PRIVATE ${GTK3_LIBRARY_DIRS})
  target_link_libraries(fast-enough PRIVATE ${GTK3_LIBRARIES} dl)
  message(STATUS "Tray: GTK ${GTK3_VERSION} (appindicator loaded at runtime)")
else()
  message(STATUS "Tray: GTK3 not found; tray disabled")
endif()

install(TARGETS fast-enough RUNTIME DESTINATION "${AFM_INSTALL_LIBDIR}")
afm_install_bin_symlink(fast-enough)
endif()

install(FILES ${CMAKE_SOURCE_DIR}/server/afm-server
              ${CMAKE_SOURCE_DIR}/server/afm-labels.dex
        DESTINATION "${AFM_INSTALL_LIBDIR}")

install(FILES ${CMAKE_SOURCE_DIR}/res/fast-enough.desktop
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
install(FILES ${CMAKE_SOURCE_DIR}/res/app.png
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps"
        RENAME fast-enough.png)

install(FILES ${CMAKE_SOURCE_DIR}/LICENSE
              ${CMAKE_SOURCE_DIR}/LINUX.md
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/fast-enough")

# --- Linux platform layer for the shared GUI code ----------------------------
find_package(OpenSSL REQUIRED)
add_library(afm_platform STATIC
  src/platform_linux.cpp
  src/mount_stubs.cpp
)
target_include_directories(afm_platform PUBLIC src ${IMGUI_DIR})
target_link_libraries(afm_platform PUBLIC OpenSSL::SSL OpenSSL::Crypto afm_subprocess)
if(FUSE3_FOUND)
  target_compile_definitions(afm_platform PRIVATE AFM_HAVE_FUSE=1)
endif()

add_executable(platform_test tools/platform_test.cpp)
target_link_libraries(platform_test PRIVATE afm_platform)
add_test(NAME platform COMMAND platform_test)
