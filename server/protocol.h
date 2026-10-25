// AFM Server Protocol - shared between server and client
#pragma once

#include <stdint.h>

// All multi-byte values are little-endian

#define AFM_PORT 5740
#define AFM_MAGIC 0x41464D53  // "AFMS"
#define AFM_VERSION 3
#define AFM_CHUNK_SIZE (4 * 1024 * 1024)  // 4MB transfer chunks

// Struct packing: GCC/Clang take an attribute, MSVC takes #pragma pack. Both
// sides of the wire must agree on layout, so the macros live here.
#if defined(_MSC_VER)
#define AFM_PACKED
#pragma pack(push, 1)
#else
#define AFM_PACKED __attribute__((packed))
#endif

// Commands (client -> server)
enum {
    CMD_PING       = 1,
    CMD_LIST       = 2,   // payload: path string
    CMD_PULL       = 3,   // payload: path string
    CMD_PUSH       = 4,   // payload: [8B size][path string]
    CMD_DELETE     = 5,   // payload: path string
    CMD_MKDIR      = 6,   // payload: path string
    CMD_RENAME     = 7,   // payload: [4B old_len][old_path][new_path]
    CMD_STAT       = 8,   // payload: path string
    CMD_STORAGE    = 9,   // payload: none - detect storage root
    CMD_QUIT       = 10,  // payload: none - shutdown server
    CMD_RESUME_PULL= 11,  // payload: [8B offset][path string] - resume pull from offset
    CMD_RESUME_PUSH= 12,  // payload: [8B total_size][path string] - resume push, server returns current offset
    CMD_CRC32      = 13,  // payload: path string - returns 4B CRC32 of file
    CMD_MCRAW_LIST = 14,  // payload: path to .mcraw file - returns virtual dir listing
    CMD_MCRAW_EXTRACT = 15, // payload: [4B mcraw_path_len][mcraw_path][virtual_name] - streams DNG/WAV/JSON
    CMD_READ_RANGE = 16, // payload: [8B offset][8B length][path string] - read byte range
    CMD_DISK_SPACE = 17, // payload: none - returns [8B total][8B free] bytes
    CMD_WRITE_RANGE = 18, // payload: [8B offset][8B length][path string] then RSP_DATA chunks, RSP_DONE
    CMD_CREATE_FILE = 19, // payload: [8B total_size][path string] - create/truncate file
    CMD_SHA256     = 20, // payload: path string - returns 32B SHA-256
    CMD_ARCHIVE_PATH = 21, // payload: [4B exclude_cache][path string] - streams tar archive of directory
    CMD_EXTRACT_ARCHIVE = 22, // payload: [4B target_len][4B archive_len][target][archive_path]
    CMD_INSTALL_BATCH = 23, // payload: JSON manifest of staged APK groups
    CMD_RESTORE_BACKUP_ARCHIVE = 24, // payload: JSON restore manifest for a staged ZIP64 store archive
    CMD_CREATE_BACKUP_ARCHIVE = 25, // payload: JSON app selection, returns a device-local ZIP64 store archive
    CMD_CREATE_FILE_EX = 26, // payload: [4B flags][8B total_size][path string]
    CMD_TRUNCATE = 27,       // payload: [8B size][path string]
    CMD_RENAME_EX = 28,      // payload: [4B flags][4B old_len][old_path][new_path]
    CMD_CAPABILITIES = 29,   // payload: none - returns capability flags
    CMD_REMOVE_FILE = 30,    // payload: path string - unlink a file (never recursive)
    CMD_RMDIR = 31,          // payload: path string - rmdir an empty directory
};

// Capability bits returned by CMD_CAPABILITIES.
#define AFM_CAP_FUSE_WRITE 0x00000001u  // CREATE_FILE_EX, TRUNCATE, RENAME_EX, REMOVE_FILE, RMDIR

// Flags for CMD_CREATE_FILE_EX.
#define AFM_CREATE_TRUNCATE  0x00000001u
#define AFM_CREATE_EXCLUSIVE 0x00000002u

// Flags for CMD_RENAME_EX.
#define AFM_RENAME_REPLACE 0x00000001u

// Responses (server -> client)
enum {
    RSP_OK         = 100,
    RSP_ERROR      = 101, // payload: error string
    RSP_DATA       = 102, // payload: raw data chunk
    RSP_DONE       = 103, // payload: none - end of stream
};

// Message header (8 bytes)
typedef struct {
    uint32_t cmd;        // command or response code
    uint32_t length;     // payload length in bytes
} AFM_PACKED MsgHeader;

// File entry in LIST response
typedef struct {
    uint8_t  type;       // 0=file, 1=dir, 2=symlink
    uint64_t size;
    int64_t  mtime;      // unix timestamp
    uint32_t name_len;
    // followed by name_len bytes of name
} AFM_PACKED FileEntryHeader;

// PULL response header (after RSP_OK)
typedef struct {
    uint64_t file_size;
} AFM_PACKED PullHeader;

// PUSH command payload prefix
typedef struct {
    uint64_t file_size;
    // followed by path string (length = msg.length - 8)
} AFM_PACKED PushHeader;

// STAT response
typedef struct {
    uint8_t  type;       // 0=file, 1=dir, 2=symlink, 255=not found
    uint64_t size;
    int64_t  mtime;
} AFM_PACKED StatResponse;

#if defined(_MSC_VER)
#pragma pack(pop)
#endif
