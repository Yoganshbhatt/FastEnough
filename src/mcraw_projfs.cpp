#define _CRT_SECURE_NO_WARNINGS
#define _CRT_NONSTDC_NO_DEPRECATE
#include "mcraw_projfs.h"
#include "mcraw_local.h"
#include "app.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <objbase.h>
#include <projectedfslib.h>

#pragma comment(lib, "ole32.lib")

#include <motioncam/Decoder.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <algorithm>
#include <unordered_map>
#include <atomic>
#include <cstdio>
#include <cwctype>

#pragma comment(lib, "ProjectedFSLib.lib")

struct VirtualEntry {
    std::wstring name;
    uint64_t size = 0;
    bool isDirectory = false;
    int frameIndex = -1;   // -1 for non-frame entries
    enum Kind { METADATA, FRAME, AUDIO } kind;
};

struct MountContext {
    std::string mcrawPath;
    std::unique_ptr<motioncam::Decoder> decoder;
    nlohmann::json containerMeta;
    std::vector<motioncam::Timestamp> frames;

    std::vector<VirtualEntry> entries; // cached listing
    std::unordered_map<std::wstring, size_t> entryIndex;
    std::mutex cacheMutex;
    std::mutex decoderMutex;
    struct CachedFrame {
        std::shared_ptr<const std::vector<uint8_t>> data;
        uint64_t lastAccess = 0;
    };
    std::unordered_map<int, CachedFrame> dngCache;
    size_t cachedBytes = 0;
    uint64_t cacheClock = 0;
    static constexpr size_t MAX_CACHE_BYTES = 256 * 1024 * 1024;
    static constexpr size_t MAX_CACHE_FRAMES = 8;

    // Audio cache
    std::shared_ptr<const std::vector<uint8_t>> audioCache;

    // Metadata cache
    std::string metadataJson;
    std::shared_ptr<const std::vector<uint8_t>> metadataBytes;

    PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT virtContext = nullptr;
};

static bool generateDngInMemory(MountContext* ctx, int frameIndex, std::vector<uint8_t>& outDng) {
    try {
        std::vector<uint8_t> pixelData;
        nlohmann::json frameMeta;
        ctx->decoder->loadFrame(ctx->frames[frameIndex], pixelData, frameMeta);

        return generateDngInMemoryFast(pixelData.data(), pixelData.size(),
                                       frameMeta, ctx->containerMeta, outDng);
    } catch (...) {
        return false;
    }
}

static bool generateWavInMemory(MountContext* ctx, std::vector<uint8_t>& outWav) {
    try {
        std::vector<motioncam::AudioChunk> audioChunks;
        ctx->decoder->loadAudio(audioChunks);
        if (audioChunks.empty()) return false;

        int sampleRate = ctx->decoder->audioSampleRateHz();
        int numChannels = ctx->decoder->numAudioChannels();

        size_t totalSamples = 0;
        for (auto& c : audioChunks) totalSamples += c.second.size();

        uint32_t dataSize = (uint32_t)(totalSamples * sizeof(int16_t));
        uint32_t fileSize = 36 + dataSize;

        std::ostringstream oss(std::ios::binary);
        oss.write("RIFF", 4);
        oss.write((const char*)&fileSize, 4);
        oss.write("WAVE", 4);
        oss.write("fmt ", 4);
        uint32_t fmtSize = 16; oss.write((const char*)&fmtSize, 4);
        uint16_t audioFormat = 1; oss.write((const char*)&audioFormat, 2);
        uint16_t channels = (uint16_t)numChannels; oss.write((const char*)&channels, 2);
        uint32_t sr = (uint32_t)sampleRate; oss.write((const char*)&sr, 4);
        uint32_t byteRate = sr * channels * sizeof(int16_t); oss.write((const char*)&byteRate, 4);
        uint16_t blockAlign = channels * sizeof(int16_t); oss.write((const char*)&blockAlign, 2);
        uint16_t bitsPerSample = 16; oss.write((const char*)&bitsPerSample, 2);
        oss.write("data", 4);
        oss.write((const char*)&dataSize, 4);
        for (auto& c : audioChunks)
            oss.write((const char*)c.second.data(), c.second.size() * sizeof(int16_t));

        std::string s = oss.str();
        outWav.assign(s.begin(), s.end());
        return true;
    } catch (...) {
        return false;
    }
}

static const VirtualEntry* findEntry(MountContext* ctx, const std::wstring& relativePath) {
    // Strip leading backslash
    std::wstring name = relativePath;
    if (!name.empty() && name[0] == L'\\') name = name.substr(1);
    // Also strip trailing backslash
    if (!name.empty() && name.back() == L'\\') name.pop_back();

    std::transform(name.begin(), name.end(), name.begin(), towlower);
    auto it = ctx->entryIndex.find(name);
    return it == ctx->entryIndex.end() ? nullptr : &ctx->entries[it->second];
}

static std::shared_ptr<const std::vector<uint8_t>> getFileData(MountContext* ctx, const VirtualEntry* entry) {
    if (entry->kind == VirtualEntry::METADATA) return ctx->metadataBytes;

    auto cached = [&]() -> std::shared_ptr<const std::vector<uint8_t>> {
        std::lock_guard<std::mutex> lock(ctx->cacheMutex);
        if (entry->kind == VirtualEntry::AUDIO) return ctx->audioCache;
        auto it = ctx->dngCache.find(entry->frameIndex);
        if (it == ctx->dngCache.end()) return nullptr;
        it->second.lastAccess = ++ctx->cacheClock;
        return it->second.data;
    };
    if (auto data = cached()) return data;

    std::lock_guard<std::mutex> decoderLock(ctx->decoderMutex);
    if (auto data = cached()) return data;
    auto data = std::make_shared<std::vector<uint8_t>>();
    if (entry->kind == VirtualEntry::AUDIO) {
        if (!generateWavInMemory(ctx, *data)) return nullptr;
        std::lock_guard<std::mutex> lock(ctx->cacheMutex);
        ctx->audioCache = data;
    } else if (entry->kind == VirtualEntry::FRAME) {
        if (!generateDngInMemory(ctx, entry->frameIndex, *data)) return nullptr;
        std::lock_guard<std::mutex> lock(ctx->cacheMutex);
        while (!ctx->dngCache.empty() &&
               (ctx->dngCache.size() >= MountContext::MAX_CACHE_FRAMES ||
                data->size() > MountContext::MAX_CACHE_BYTES - ctx->cachedBytes)) {
            auto oldest = std::min_element(ctx->dngCache.begin(), ctx->dngCache.end(),
                [](const auto& a, const auto& b) { return a.second.lastAccess < b.second.lastAccess; });
            ctx->cachedBytes -= oldest->second.data->size();
            ctx->dngCache.erase(oldest);
        }
        if (data->size() <= MountContext::MAX_CACHE_BYTES) {
            ctx->cachedBytes += data->size();
            ctx->dngCache.emplace(entry->frameIndex, MountContext::CachedFrame{data, ++ctx->cacheClock});
        }
    } else {
        return nullptr;
    }
    return data;
}

// Directory enumeration state
struct EnumSession {
    MountContext* context = nullptr;
    std::vector<const VirtualEntry*> sorted;
    size_t index = 0;
    bool filled = false;
};

static std::mutex g_enumMutex;
static std::map<GUID, std::unique_ptr<EnumSession>,
    decltype([](const GUID& a, const GUID& b) {
        return memcmp(&a, &b, sizeof(GUID)) < 0;
    })> g_enumSessions;

static HRESULT CALLBACK cb_StartDirEnum(
    const PRJ_CALLBACK_DATA* cbd,
    const GUID* enumId)
{
    auto* ctx = (MountContext*)cbd->InstanceContext;
    auto session = std::make_unique<EnumSession>();
    session->context = ctx;

    // Root listing: add all entries
    for (auto& e : ctx->entries)
        session->sorted.push_back(&e);

    std::lock_guard<std::mutex> lk(g_enumMutex);
    g_enumSessions[*enumId] = std::move(session);
    return S_OK;
}

static HRESULT CALLBACK cb_EndDirEnum(
    const PRJ_CALLBACK_DATA* cbd,
    const GUID* enumId)
{
    std::lock_guard<std::mutex> lk(g_enumMutex);
    g_enumSessions.erase(*enumId);
    return S_OK;
}

static HRESULT CALLBACK cb_GetDirEnum(
    const PRJ_CALLBACK_DATA* cbd,
    const GUID* enumId,
    PCWSTR searchExpression,
    PRJ_DIR_ENTRY_BUFFER_HANDLE dirEntryBufferHandle)
{
    std::lock_guard<std::mutex> lk(g_enumMutex);
    auto it = g_enumSessions.find(*enumId);
    if (it == g_enumSessions.end()) return E_INVALIDARG;

    auto* session = it->second.get();

    // On restart, reset index
    if (cbd->Flags & PRJ_CB_DATA_FLAG_ENUM_RESTART_SCAN) {
        session->index = 0;
    }

    while (session->index < session->sorted.size()) {
        const VirtualEntry* entry = session->sorted[session->index];

        // Apply search expression filter
        if (searchExpression && !PrjFileNameMatch(entry->name.c_str(), searchExpression)) {
            session->index++;
            continue;
        }

        PRJ_FILE_BASIC_INFO info = {};
        info.IsDirectory = entry->isDirectory ? TRUE : FALSE;
        info.FileSize = entry->size;

        HRESULT hr = PrjFillDirEntryBuffer(entry->name.c_str(), &info, dirEntryBufferHandle);
        if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) {
            // Buffer full, ProjFS will call us again
            return S_OK;
        }
        if (FAILED(hr)) return hr;

        session->index++;
    }

    return S_OK;
}

static HRESULT CALLBACK cb_GetPlaceholderInfo(
    const PRJ_CALLBACK_DATA* cbd)
{
    auto* ctx = (MountContext*)cbd->InstanceContext;
    const VirtualEntry* entry = findEntry(ctx, cbd->FilePathName);
    if (!entry) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    uint64_t fileSize = entry->size;

    // Frame metadata can change the DNG trailer size between frames.
    if (entry->kind == VirtualEntry::FRAME) {
        const auto data = getFileData(ctx, entry);
        if (!data) return HRESULT_FROM_WIN32(ERROR_READ_FAULT);
        fileSize = data->size();
    }

    PRJ_PLACEHOLDER_INFO placeholderInfo = {};
    placeholderInfo.FileBasicInfo.IsDirectory = entry->isDirectory ? TRUE : FALSE;
    placeholderInfo.FileBasicInfo.FileSize = fileSize;

    return PrjWritePlaceholderInfo(
        ctx->virtContext,
        cbd->FilePathName,
        &placeholderInfo,
        sizeof(placeholderInfo));
}

static HRESULT CALLBACK cb_GetFileData(
    const PRJ_CALLBACK_DATA* cbd,
    UINT64 byteOffset,
    UINT32 length)
{
    auto* ctx = (MountContext*)cbd->InstanceContext;
    const VirtualEntry* entry = findEntry(ctx, cbd->FilePathName);
    if (!entry) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    const auto data = getFileData(ctx, entry);
    if (!data) return HRESULT_FROM_WIN32(ERROR_READ_FAULT);

    // Clamp to actual data size
    if (length == 0 || byteOffset >= data->size()) return S_OK;
    uint64_t available = data->size() - byteOffset;
    uint32_t toWrite = (uint32_t)std::min((uint64_t)length, available);

    // ProjFS requires aligned buffer allocated with PrjAllocateAlignedBuffer
    void* alignedBuf = PrjAllocateAlignedBuffer(ctx->virtContext, toWrite);
    if (!alignedBuf) return E_OUTOFMEMORY;

    memcpy(alignedBuf, data->data() + byteOffset, toWrite);

    HRESULT hr = PrjWriteFileData(
        ctx->virtContext,
        &cbd->DataStreamId,
        alignedBuf,
        byteOffset,
        toWrite);

    PrjFreeAlignedBuffer(alignedBuf);
    return hr;
}

McrawMount::~McrawMount() {
    unmount();
}

void McrawMount::unmount() {
    if (m_mounted && m_virtContext) {
        PrjStopVirtualizing((PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT)m_virtContext);
        m_virtContext = nullptr;
        m_mounted = false;

        // Delete the MountContext (closes the decoder's FILE* handle on the MCRAW file)
        if (m_mountContext) {
            std::lock_guard<std::mutex> lock(g_enumMutex);
            std::erase_if(g_enumSessions, [&](const auto& entry) {
                return entry.second->context == m_mountContext;
            });
            delete (MountContext*)m_mountContext;
            m_mountContext = nullptr;
        }

        LOG_INFO("MCRAW", "Unmounted ProjFS: " + m_mountPath);
    }

    // Clean up mount directory (best effort)
    if (!m_mountPath.empty()) {
        try { std::filesystem::remove_all(m_mountPath); } catch (...) {}
    }
}

std::shared_ptr<McrawMount> McrawMount::mount(const std::string& mcrawPath, std::string& outMountPath) {
    // Create mount context with decoder
    auto ctx = new MountContext();
    ctx->mcrawPath = mcrawPath;

    try {
        ctx->decoder = std::make_unique<motioncam::Decoder>(mcrawPath);
        ctx->containerMeta = ctx->decoder->getContainerMetadata();
        ctx->frames = ctx->decoder->getFrames(); // copy
        ctx->metadataJson = ctx->containerMeta.dump(2);
        ctx->metadataBytes = std::make_shared<const std::vector<uint8_t>>(
            ctx->metadataJson.begin(), ctx->metadataJson.end());
    } catch (const std::exception& e) {
        LOG_ERROR("MCRAW", "Failed to open MCRAW: " + std::string(e.what()));
        delete ctx;
        return nullptr;
    }

    // Build virtual entry list
    // metadata.json
    {
        VirtualEntry e;
        e.name = L"metadata.json";
        e.size = ctx->metadataJson.size();
        e.kind = VirtualEntry::METADATA;
        ctx->entries.push_back(std::move(e));
    }

    // Get exact DNG size by generating the first frame (all frames are same resolution)
    uint64_t dngExactSize = 0;
    if (!ctx->frames.empty()) {
        try {
            std::vector<uint8_t> firstDng;
            if (generateDngInMemory(ctx, 0, firstDng)) {
                dngExactSize = firstDng.size();
                // Cache it so we don't regenerate on first read
                std::lock_guard<std::mutex> lk(ctx->cacheMutex);
                if (firstDng.size() <= MountContext::MAX_CACHE_BYTES) {
                    ctx->cachedBytes = firstDng.size();
                    ctx->dngCache[0] = {std::make_shared<const std::vector<uint8_t>>(std::move(firstDng)),
                                        ++ctx->cacheClock};
                }
            }
        } catch (...) {}
    }

    // Frame entries
    for (size_t i = 0; i < ctx->frames.size(); i++) {
        VirtualEntry e;
        wchar_t fname[32];
        swprintf(fname, 32, L"frame_%06zu.dng", i + 1);
        e.name = fname;
        e.size = dngExactSize;
        e.kind = VirtualEntry::FRAME;
        e.frameIndex = (int)i;
        ctx->entries.push_back(std::move(e));
    }

    // Audio sizes come from indexed chunk headers without loading PCM data.
    try {
        if (ctx->containerMeta.contains("extraData") &&
            ctx->containerMeta["extraData"].contains("audioSampleRate") &&
            ctx->containerMeta["extraData"].contains("audioChannels")) {
            int sr = ctx->containerMeta["extraData"]["audioSampleRate"];
            int ch = ctx->containerMeta["extraData"]["audioChannels"];
            if (sr > 0 && ch > 0) {
                uint64_t audioBytes = ctx->decoder->audioSizeBytes();
                if (audioBytes > 0) {
                    VirtualEntry e;
                    e.name = L"audio.wav";
                    e.size = 44 + audioBytes;
                    e.kind = VirtualEntry::AUDIO;
                    ctx->entries.push_back(std::move(e));
                }
            }
        }
    } catch (...) {}

    std::sort(ctx->entries.begin(), ctx->entries.end(), [](const auto& a, const auto& b) {
        return PrjFileNameCompare(a.name.c_str(), b.name.c_str()) < 0;
    });
    for (size_t i = 0; i < ctx->entries.size(); ++i)
        ctx->entryIndex.emplace(ctx->entries[i].name, i);

    // Create mount directory
    char tmpDir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpDir);

    // Use hash of mcraw path for unique mount dir
    size_t hash = std::hash<std::string>{}(mcrawPath);
    std::string mountDir = std::string(tmpDir) + "FastEnough_mcraw_" + std::to_string(hash);

    // Clean up any previous mount at this path
    try { std::filesystem::remove_all(mountDir); } catch (...) {}
    try { std::filesystem::create_directories(mountDir); } catch (...) {
        LOG_ERROR("MCRAW", "Failed to create mount dir: " + mountDir);
        delete ctx;
        return nullptr;
    }

    std::wstring mountDirW(mountDir.begin(), mountDir.end());

    // Mark directory as ProjFS virtualization root
    GUID instanceId;
    CoCreateGuid(&instanceId);
    HRESULT hr = PrjMarkDirectoryAsPlaceholder(
        mountDirW.c_str(), nullptr, nullptr, &instanceId);
    if (FAILED(hr)) {
        LOG_ERROR("MCRAW", "PrjMarkDirectoryAsPlaceholder failed: " + std::to_string(hr));
        delete ctx;
        return nullptr;
    }

    // Set up callback table
    PRJ_CALLBACKS callbacks = {};
    callbacks.StartDirectoryEnumerationCallback = cb_StartDirEnum;
    callbacks.EndDirectoryEnumerationCallback = cb_EndDirEnum;
    callbacks.GetDirectoryEnumerationCallback = cb_GetDirEnum;
    callbacks.GetPlaceholderInfoCallback = cb_GetPlaceholderInfo;
    callbacks.GetFileDataCallback = cb_GetFileData;

    // Start virtualizing
    PRJ_STARTVIRTUALIZING_OPTIONS options = {};
    hr = PrjStartVirtualizing(
        mountDirW.c_str(),
        &callbacks,
        ctx,  // InstanceContext — passed to all callbacks
        &options,
        &ctx->virtContext);

    if (FAILED(hr)) {
        LOG_ERROR("MCRAW", "PrjStartVirtualizing failed: " + std::to_string(hr));
        try { std::filesystem::remove_all(mountDir); } catch (...) {}
        delete ctx;
        return nullptr;
    }

    // Create McrawMount object
    auto mount = std::shared_ptr<McrawMount>(new McrawMount());
    mount->m_mcrawPath = mcrawPath;
    mount->m_mountPath = mountDir;
    mount->m_mountPathW = mountDirW;
    mount->m_virtContext = ctx->virtContext;
    mount->m_mountContext = ctx; // store so we can delete it on unmount
    mount->m_mounted = true;

    outMountPath = mountDir;
    LOG_INFO("MCRAW", "ProjFS mounted: " + mcrawPath + " -> " + mountDir);

    return mount;
}

McrawMountManager& McrawMountManager::instance() {
    static McrawMountManager s;
    return s;
}

std::string McrawMountManager::mountMcraw(const std::string& mcrawPath) {
    std::lock_guard<std::mutex> lk(m_mutex);

    // Check if already mounted
    for (auto& m : m_mounts) {
        if (m->mcrawPath() == mcrawPath) return m->mountPath();
    }

    std::string mountPath;
    auto mount = McrawMount::mount(mcrawPath, mountPath);
    if (mount) {
        m_mounts.push_back(mount);
        return mountPath;
    }
    return "";
}

bool McrawMountManager::isInsideMount(const std::string& path) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& m : m_mounts) {
        if (path.find(m->mountPath()) == 0) return true;
    }
    return false;
}

void McrawMountManager::unmountAll() {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_mounts.clear(); // shared_ptr destructors call unmount()
}
