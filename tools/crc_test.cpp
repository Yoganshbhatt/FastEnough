// Verifies the slicing-by-8 CRC against published vectors and the GF(2)
// combine against a split computation. Local only — no device, no sockets.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "device_client.h"

namespace {
int failures = 0;
void check(bool ok, const char* name, const std::string& detail = {}) {
    if (ok) {
        std::puts(("  ok: " + std::string(name)).c_str());
    } else {
        ++failures;
        std::puts(("  FAIL: " + std::string(name) +
                   (detail.empty() ? "" : " -- " + detail)).c_str());
    }
}

uint32_t crcOf(const char* data, size_t len) {
    uint32_t crc = ~(uint32_t)0;
    crc32Update(crc, data, len);
    return ~crc;
}
}  // namespace

int main() {
    // Published check vector: CRC32("123456789") = 0xCBF43926.
    check(crcOf("123456789", 9) == 0xCBF43926u, "check vector 0xCBF43926");
    check(crcOf("", 0) == 0u, "empty input crc is 0");

    // Incremental update must equal one-shot.
    const std::string data(100000, 'Q');
    const uint32_t whole = crcOf(data.data(), data.size());
    uint32_t split = ~(uint32_t)0;
    crc32Update(split, data.data(), 40960);
    crc32Update(split, data.data() + 40960, data.size() - 40960);
    check(~split == whole, "incremental equals one-shot");

    // Combine: crc(first) + crc(second) over len(second) == crc(all).
    const std::string a("first-half-", 11), b("-second-half", 12);
    uint32_t crcA = ~(uint32_t)0;
    crc32Update(crcA, a.data(), a.size());
    crcA = ~crcA;
    uint32_t crcB = ~(uint32_t)0;
    crc32Update(crcB, b.data(), b.size());
    crcB = ~crcB;
    const std::string all = a + b;
    check(crc32Combine(crcA, crcB, b.size()) == crcOf(all.data(), all.size()),
          "combine matches full");
    check(crc32Combine(crcA, 0, 0) == crcA, "combine with empty tail is identity");

    // Misaligned buffers (slicing-by-8 must not assume alignment).
    char mis[24];
    for (int off = 0; off < 8; ++off) {
        std::memset(mis, 'X', sizeof(mis));
        std::memcpy(mis + off, "123456789", 9);
        if (crcOf(mis + off, 9) != 0xCBF43926u) {
            check(false, "misaligned buffer", "offset " + std::to_string(off));
            break;
        }
    }
    check(failures == 0, "all misaligned offsets match");

    if (failures == 0) std::puts("crc_test: ALL OK");
    return failures == 0 ? 0 : 1;
}
