// Wire-protocol invariants.
//
// src/protocol.h includes the authoritative server/protocol.h, so there is
// only one definition. These static assertions pin the values and the packed
// layouts that the on-disk protocol depends on: changing any of them breaks
// compatibility with deployed helpers, which must be a deliberate version bump
// rather than an accident.
#include "protocol.h"

#include <cstddef>
#include <cstdio>

static_assert(AFM_PORT == 5740, "server port changed");
static_assert(AFM_MAGIC == 0x41464D53, "magic changed");
static_assert(AFM_VERSION == 3, "version bump needs a protocol review");
static_assert(AFM_CHUNK_SIZE == 4 * 1024 * 1024, "chunk size changed");

static_assert(CMD_PING == 1 && CMD_LIST == 2 && CMD_PULL == 3 && CMD_PUSH == 4);
static_assert(CMD_DELETE == 5 && CMD_MKDIR == 6 && CMD_RENAME == 7 && CMD_STAT == 8);
static_assert(CMD_STORAGE == 9 && CMD_QUIT == 10);
static_assert(CMD_RESUME_PULL == 11 && CMD_RESUME_PUSH == 12);
static_assert(CMD_CRC32 == 13 && CMD_MCRAW_LIST == 14);
static_assert(CMD_MCRAW_EXTRACT == 15 && CMD_READ_RANGE == 16);
static_assert(CMD_DISK_SPACE == 17 && CMD_WRITE_RANGE == 18);
static_assert(CMD_CREATE_FILE == 19 && CMD_SHA256 == 20);
static_assert(CMD_ARCHIVE_PATH == 21 && CMD_EXTRACT_ARCHIVE == 22);
static_assert(CMD_INSTALL_BATCH == 23 && CMD_RESTORE_BACKUP_ARCHIVE == 24);
static_assert(CMD_CREATE_BACKUP_ARCHIVE == 25 && CMD_CREATE_FILE_EX == 26);
static_assert(CMD_TRUNCATE == 27 && CMD_RENAME_EX == 28);
static_assert(CMD_CAPABILITIES == 29 && CMD_REMOVE_FILE == 30);
static_assert(CMD_RMDIR == 31);
static_assert(RSP_OK == 100 && RSP_ERROR == 101 && RSP_DATA == 102 && RSP_DONE == 103);

static_assert(AFM_CAP_FUSE_WRITE == 0x1u, "capability bit changed");
static_assert(AFM_CREATE_TRUNCATE == 0x1u && AFM_CREATE_EXCLUSIVE == 0x2u);
static_assert(AFM_RENAME_REPLACE == 0x1u);

static_assert(sizeof(MsgHeader) == 8, "MsgHeader must stay 8 bytes");
static_assert(offsetof(MsgHeader, cmd) == 0);
static_assert(offsetof(MsgHeader, length) == 4);

static_assert(sizeof(FileEntryHeader) == 21, "FileEntryHeader must stay packed");
static_assert(offsetof(FileEntryHeader, type) == 0);
static_assert(offsetof(FileEntryHeader, size) == 1);
static_assert(offsetof(FileEntryHeader, mtime) == 9);
static_assert(offsetof(FileEntryHeader, name_len) == 17);

static_assert(sizeof(PullHeader) == 8);
static_assert(sizeof(PushHeader) == 8);

static_assert(sizeof(StatResponse) == 17, "StatResponse must stay packed");
static_assert(offsetof(StatResponse, type) == 0);
static_assert(offsetof(StatResponse, size) == 1);
static_assert(offsetof(StatResponse, mtime) == 9);

int main() {
    std::printf("protocol invariants hold (v%d, %zu-byte header)\n", AFM_VERSION,
                sizeof(MsgHeader));
    return 0;
}
