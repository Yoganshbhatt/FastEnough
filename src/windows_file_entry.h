#pragma once
#include <cstdint>
#include <string>

struct WindowsFileEntry {
    std::string name;
    uint64_t size = 0;
    std::string dateModified;
    bool isDirectory = false;
    bool isHidden = false;
};
