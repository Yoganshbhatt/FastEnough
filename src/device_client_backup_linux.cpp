// Linux implementations of the app-management + backup DeviceClient methods
// (Stage D port). Windows keeps its versions in device_client.cpp; that file
// is not part of the Linux build. Small helpers below are local copies of the
// Windows statics (same behavior); the one real difference is socket waiting:
// Windows swaps SO_RCVTIMEO for the length of the operation, Linux passes an
// explicit per-frame deadline to sendMsgOn/recvMsgOn (AGENTS.md: every socket
// operation has a timeout; no socket-wide state).
#include "device_client.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "log_facade.h"
#include "path_util.h"

namespace {

constexpr uint32_t kBackupOpMs = 30 * 60 * 1000;    // backup/restore staging window
constexpr uint32_t kInstallOpMs = 10 * 60 * 1000;   // batch install window

std::string quoteCmdArg(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += "\"";
    return out;
}

std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

bool isAdbSuccess(const std::string& out) {
    return out.find("Success") != std::string::npos &&
           out.find("Failure") == std::string::npos &&
           out.find("Exception") == std::string::npos &&
           out.find("Error") == std::string::npos;
}

std::string sanitizeAdbOutput(std::string text) {
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back();
    return text;
}

std::string sanitizePathPart(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-')
            out.push_back(c);
        else
            out.push_back('_');
    }
    return out.empty() ? "unknown" : out;
}

std::string numericValueAfter(const std::string& text, const std::string& marker) {
    const size_t markerPosition = text.find(marker);
    if (markerPosition == std::string::npos) return {};
    size_t position = markerPosition + marker.size();
    while (position < text.size() && !std::isdigit(static_cast<unsigned char>(text[position])))
        ++position;
    const size_t start = position;
    while (position < text.size() && std::isdigit(static_cast<unsigned char>(text[position])))
        ++position;
    return text.substr(start, position - start);
}

std::string friendlyRestoreFailure(const std::string& rawOutput,
                                   const std::string& appName,
                                   const std::string& packageName,
                                   bool allowDowngrade) {
    const std::string heading = (appName.empty() ? packageName : appName) +
        (appName.empty() || appName == packageName ? "" : " (" + packageName + ")");
    std::string result = heading + "\n";
    if (rawOutput.find("INSTALL_FAILED_VERSION_DOWNGRADE") != std::string::npos) {
        result += "The APK was not installed because the backup contains an older app version than the device.\n";
        const std::string backupVersion = numericValueAfter(rawOutput, "Update version code");
        const std::string installedVersion = numericValueAfter(rawOutput, "older than current");
        if (!backupVersion.empty()) result += "Backup version code: " + backupVersion + "\n";
        if (!installedVersion.empty()) result += "Installed version code: " + installedVersion + "\n";
        result += allowDowngrade
            ? "Android rejected the downgrade. Uninstall the newer version first, then restore again."
            : "Enable Allow downgrade and retry, or uninstall the newer version first.";
    } else if (rawOutput.find("INSTALL_FAILED_UPDATE_INCOMPATIBLE") != std::string::npos) {
        result += "The installed app has a different signing certificate from the backup.\n";
        result += "Uninstall the installed version before restoring this backup.";
    } else if (rawOutput.find("INSTALL_FAILED_INSUFFICIENT_STORAGE") != std::string::npos) {
        result += "The device does not have enough free storage to install this app.";
    } else if (rawOutput.find("INSTALL_FAILED_MISSING_SPLIT") != std::string::npos) {
        result += "The backup is missing one or more required split APK files.";
    } else if (rawOutput.find("INSTALL_FAILED_INVALID_APK") != std::string::npos ||
               rawOutput.find("INSTALL_PARSE_FAILED") != std::string::npos) {
        result += "Android rejected the backed-up APK because it is invalid or incomplete.";
    } else if (rawOutput.find("INSTALL_FAILED_USER_RESTRICTED") != std::string::npos) {
        result += "Android blocked the installation because app installation is restricted on this device.";
    } else if (rawOutput.find("INSTALL_FAILED_CONFLICTING_PROVIDER") != std::string::npos) {
        result += "The app conflicts with a content provider owned by another installed app.";
    } else if (rawOutput.find("data.tar is missing") != std::string::npos ||
               rawOutput.find("data_de.tar is missing") != std::string::npos) {
        result += "The backup is missing one or more selected app-data archives.";
    } else {
        const size_t failurePosition = rawOutput.find("Failure [");
        if (failurePosition != std::string::npos) {
            const size_t end = rawOutput.find_first_of("\r\n", failurePosition);
            result += "Android reported: " + rawOutput.substr(
                failurePosition, end == std::string::npos ? std::string::npos : end - failurePosition);
        } else {
            result += "The app could not be restored.";
            const std::string cleaned = sanitizeAdbOutput(rawOutput);
            if (!cleaned.empty()) result += "\nDevice response: " + cleaned;
        }
    }
    return result;
}

// Monotonic stamp for unique remote staging paths (Windows: GetTickCount64).
uint64_t processStampMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

bool DeviceClient::installApk(const std::string& serial, const std::string& apkPath, std::string& output,
                              bool reinstall, bool grantRuntimePermissions, bool allowDowngrade) {
    if (serial.empty() || apkPath.empty()) {
        output = "No device or APK path was provided.";
        return false;
    }
    // splitAdbArgs strips the double quotes and adb re-escapes the argument for
    // the device shell, so paths with spaces stay one argument end-to-end.
    std::string args = "-s " + serial + " install";
    if (reinstall) args += " -r";
    if (grantRuntimePermissions) args += " -g";
    if (allowDowngrade) args += " -d";
    args += " " + quoteCmdArg(apkPath);
    output = runAdbCommand(args, 10 * 60 * 1000);
    return isAdbSuccess(output);
}

bool DeviceClient::uninstallPackage(const std::string& serial, const std::string& packageName, std::string& output,
                                    bool keepData, bool userOnly, bool useRoot) {
    if (serial.empty() || packageName.empty()) {
        output = "No device or package was provided.";
        return false;
    }

    std::string flags;
    if (keepData) flags += " -k";
    if (userOnly) flags += " --user 0";

    if (useRoot) {
        const std::string inner = "pm uninstall" + flags + " " + packageName;
        output = runAdbCommand("-s " + serial + " shell \"su -c '" + inner + "'\"");
    } else if (userOnly) {
        output = runAdbCommand("-s " + serial + " shell pm uninstall" + flags + " " + packageName);
    } else {
        output = runAdbCommand("-s " + serial + " uninstall" + (keepData ? " -k " : " ") + packageName);
    }
    return isAdbSuccess(output);
}

bool DeviceClient::createDeviceBackupArchive(const std::string& serial,
                                             const std::string& created,
                                             const std::vector<ArchiveBackupApp>& apps,
                                             const AppBackupOptions& options,
                                             std::string& remoteArchive,
                                             uint64_t& archiveSize,
                                             std::vector<bool>& backedUp,
                                             std::string& output,
                                             InstallProgressCallback progress) {
    remoteArchive.clear();
    archiveSize = 0;
    backedUp.assign(apps.size(), false);
    output.clear();
    if (!m_connected || apps.empty()) {
        output = m_connected ? "No apps were selected for backup" : "The device helper is not connected";
        return false;
    }

    nlohmann::json request;
    request["serial"] = serial;
    request["created"] = created;
    request["includeApk"] = options.includeApk;
    request["includeData"] = options.includeData;
    request["includeDeviceProtectedData"] = options.includeDeviceProtectedData;
    request["includeExternalData"] = options.includeExternalData;
    request["includeObb"] = options.includeObb;
    request["includeMedia"] = options.includeMedia;
    request["excludeCache"] = options.excludeCache;
    request["apps"] = nlohmann::json::array();
    for (const auto& app : apps) {
        request["apps"].push_back({{"packageName", app.packageName}, {"label", app.label}});
    }
    const std::string encoded = request.dump();

    std::lock_guard<std::mutex> lock(m_mutex);
    const uintptr_t sock = m_socket;
    if (sock == ~(uintptr_t)0) {
        output = "The device helper is not connected";
        return false;
    }
    MsgHeader header{};
    std::vector<char> payload;
    const bool received = sendMsgOn(sock, CMD_CREATE_BACKUP_ARCHIVE, encoded.data(),
                                    static_cast<uint32_t>(encoded.size())) &&
        recvMsgOn(sock, header, payload, kBackupOpMs);
    if (!received || header.cmd != RSP_OK) {
        if (received && header.cmd == RSP_ERROR) output.assign(payload.begin(), payload.end());
        else output = m_lastError.empty() ? "The device did not accept the backup request" : m_lastError;
        return false;
    }

    bool protocolOk = false;
    while (recvMsgOn(sock, header, payload, kBackupOpMs)) {
        if (header.cmd == RSP_DONE) {
            nlohmann::json done = nlohmann::json::parse(payload.begin(), payload.end(), nullptr, false);
            if (!done.is_discarded()) {
                remoteArchive = done.value("archive", "");
                archiveSize = done.value("size", uint64_t{0});
                if (done.contains("results") && done["results"].is_array()) {
                    const size_t count = std::min(backedUp.size(), done["results"].size());
                    for (size_t index = 0; index < count; ++index) {
                        if (done["results"][index].is_boolean()) backedUp[index] = done["results"][index].get<bool>();
                    }
                }
                protocolOk = !remoteArchive.empty() && archiveSize > 0;
            }
            break;
        }
        if (header.cmd == RSP_ERROR) {
            output.append(payload.begin(), payload.end());
            break;
        }
        if (header.cmd != RSP_DATA) break;
        nlohmann::json event = nlohmann::json::parse(payload.begin(), payload.end(), nullptr, false);
        if (event.is_discarded()) continue;
        const size_t index = event.value("index", apps.size());
        if (index >= apps.size()) continue;
        const std::string state = event.value("state", "");
        if (state == "complete") {
            backedUp[index] = event.value("success", false);
            if (!backedUp[index]) {
                output += apps[index].packageName + ": " + event.value("output", "Backup failed") + "\n";
            }
        }
        if (progress) {
            AppInstallProgress update;
            update.packageName = event.value("packageName", apps[index].packageName);
            update.stageLabel = event.value("stage", state);
            update.backupIndex = static_cast<int>(index);
            update.completedApps = event.value("completed", 0);
            update.totalApps = static_cast<int>(apps.size());
            update.appProgress = event.value("progress", state == "complete" ? 1.0f : 0.0f);
            progress(update);
        }
    }
    if (!protocolOk && output.empty()) output = "The device returned an incomplete backup archive result";
    return protocolOk;
}

bool DeviceClient::installBackupApksBatch(const std::vector<std::string>& backupPaths,
                                          const AppBackupOptions& options,
                                          std::vector<bool>& installed, std::string& output,
                                          ProgressCallback progress,
                                          InstallProgressCallback installProgress) {
    installed.assign(backupPaths.size(), true);
    output.clear();
    if (!options.includeApk || backupPaths.empty()) return true;
    if (!m_connected || !useRoot()) {
        m_lastError = "Root device server is not connected";
        output = m_lastError;
        return false;
    }

    struct LocalGroup {
        size_t backupIndex = 0;
        std::string packageName;
        std::vector<std::filesystem::path> files;
        std::vector<std::string> remotePaths;
        uint64_t totalBytes = 0;
    };
    std::vector<LocalGroup> groups;
    uint64_t totalBytes = 0;
    std::error_code ec;

    for (size_t i = 0; i < backupPaths.size(); ++i) {
        std::filesystem::path dir = pathutil::toFsPath(backupPaths[i]);
        std::ifstream mf(dir / "backup.json", std::ios::binary);
        nlohmann::json meta = nlohmann::json::parse(mf, nullptr, false);
        if (meta.is_discarded()) {
            installed[i] = false;
            output += backupPaths[i] + ": Backup metadata is not valid.\n";
            continue;
        }
        if (!meta.value("has", nlohmann::json::object()).value("apk", false)) continue;

        LocalGroup group;
        group.backupIndex = i;
        group.packageName = meta.value("packageName", backupPaths[i]);
        for (const auto& apk : meta.value("apkFiles", nlohmann::json::array())) {
            std::filesystem::path file = dir / apk.value("name", "");
            if (!std::filesystem::is_regular_file(file, ec)) {
                installed[i] = false;
                output += backupPaths[i] + ": An APK file is missing.\n";
                group.files.clear();
                break;
            }
            const uint64_t fileSize = std::filesystem::file_size(file, ec);
            totalBytes += fileSize;
            group.totalBytes += fileSize;
            group.files.push_back(std::move(file));
        }
        if (!group.files.empty()) {
            installed[i] = false;
            groups.push_back(std::move(group));
        }
    }
    if (groups.empty())
        return std::all_of(installed.begin(), installed.end(), [](bool value) { return value; });

    const std::string stagingRoot = "/data/local/tmp/afm-install-" +
        std::to_string(::getpid()) + "-" + std::to_string(processStampMs());
    if (!createDirectory(stagingRoot)) {
        output += "Could not create the APK staging directory: " + m_lastError;
        return false;
    }

    uint64_t completedBytes = 0;
    for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        auto& group = groups[groupIndex];
        uint64_t groupCompletedBytes = 0;
        for (size_t apkIndex = 0; apkIndex < group.files.size(); ++apkIndex) {
            const uint64_t size = std::filesystem::file_size(group.files[apkIndex], ec);
            std::string remote = stagingRoot + "/" + std::to_string(groupIndex) + "/" +
                std::to_string(apkIndex) + ".apk";
            auto uploadProgress = [&](uint64_t done, uint64_t) {
                if (installProgress) {
                    AppInstallProgress appProgress;
                    appProgress.packageName = group.packageName;
                    appProgress.stageLabel = "Uploading APKs";
                    appProgress.backupIndex = static_cast<int>(group.backupIndex);
                    appProgress.totalApps = static_cast<int>(backupPaths.size());
                    appProgress.appProgress = group.totalBytes > 0
                        ? 0.55f * static_cast<float>(groupCompletedBytes + done) /
                            static_cast<float>(group.totalBytes)
                        : 0.55f;
                    installProgress(appProgress);
                }
                return !progress || progress(completedBytes + done, totalBytes);
            };
            if (!pushFile(pathutil::pathToUtf8(group.files[apkIndex]), remote, size, uploadProgress)) {
                output += backupPaths[group.backupIndex] + ": APK upload failed: " + m_lastError + "\n";
                deleteFile(stagingRoot);
                return false;
            }
            completedBytes += size;
            groupCompletedBytes += size;
            group.remotePaths.push_back(std::move(remote));
        }
    }

    nlohmann::json request;
    request["root"] = stagingRoot;
    request["reinstall"] = options.reinstall;
    request["grant"] = options.grantRuntimePermissions;
    request["downgrade"] = options.allowDowngrade;
    request["parallelism"] = 3;
    request["packages"] = nlohmann::json::array();
    for (const auto& group : groups) request["packages"].push_back({{"apks", group.remotePaths}});
    const std::string encoded = request.dump();

    MsgHeader hdr{};
    std::vector<char> responsePayload;
    bool received = false;
    bool allOk = true;
    std::vector<bool> resultSeen(groups.size(), false);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const uintptr_t sock = m_socket;
        if (sock == ~(uintptr_t)0) {
            output += "The device server is no longer connected.";
            return false;
        }
        const bool sent = sendMsgOn(sock, CMD_INSTALL_BATCH, encoded.data(),
                                    static_cast<uint32_t>(encoded.size()));
        received = sent && recvMsgOn(sock, hdr, responsePayload, kInstallOpMs);
        if (received && hdr.cmd == RSP_OK) {
            while (recvMsgOn(sock, hdr, responsePayload, kInstallOpMs)) {
                if (hdr.cmd == RSP_DONE) break;
                if (hdr.cmd == RSP_ERROR) {
                    output += std::string(responsePayload.data(), responsePayload.size());
                    received = false;
                    break;
                }
                if (hdr.cmd != RSP_DATA) {
                    received = false;
                    break;
                }
                nlohmann::json event = nlohmann::json::parse(responsePayload.begin(),
                                                             responsePayload.end(), nullptr, false);
                if (event.is_discarded()) continue;
                const size_t groupIndex = event.value("index", groups.size());
                if (groupIndex >= groups.size()) continue;
                const std::string state = event.value("state", "");
                if (installProgress) {
                    AppInstallProgress appProgress;
                    appProgress.packageName = groups[groupIndex].packageName;
                    appProgress.backupIndex = static_cast<int>(groups[groupIndex].backupIndex);
                    appProgress.completedApps = event.value("completed", 0);
                    appProgress.totalApps = static_cast<int>(groups.size());
                    appProgress.stageLabel = state == "installing" ? "Installing app" :
                        (event.value("success", false) ? "App installed" : "Install failed");
                    appProgress.appProgress = state == "installing" ? 0.70f : 1.0f;
                    installProgress(appProgress);
                }
                if (state == "complete") {
                    const bool success = event.value("success", false);
                    installed[groups[groupIndex].backupIndex] = success;
                    resultSeen[groupIndex] = true;
                    if (!success) {
                        allOk = false;
                        output += backupPaths[groups[groupIndex].backupIndex] + ":\n" +
                            event.value("output", "Install failed") + "\n";
                    }
                }
            }
        }
    }
    if (!received) {
        output += "The device server did not return the batch install result.";
        return false;
    }
    if (hdr.cmd == RSP_ERROR) {
        output += std::string(responsePayload.data(), responsePayload.size());
        return false;
    }
    if (hdr.cmd != RSP_DONE || std::find(resultSeen.begin(), resultSeen.end(), false) != resultSeen.end()) {
        output += "The device server returned an invalid batch install result.";
        return false;
    }
    if (progress) progress(totalBytes, totalBytes);
    return allOk;
}

bool DeviceClient::restoreAppBackup(const std::string& serial, const std::string& backupPath, const AppBackupOptions& options,
                                    std::string& output, BackupProgressCallback progress) {
    output.clear();
    if (serial.empty() || backupPath.empty()) {
        output = "No device or backup was provided.";
        return false;
    }
    if (!(m_connected && useRoot()) && !isRootAvailable(serial)) {
        output = "Root access is required for app data restore.";
        return false;
    }

    std::filesystem::path dir = pathutil::toFsPath(backupPath);
    std::ifstream mf(dir / "backup.json", std::ios::binary);
    if (!mf) {
        output = "Backup metadata is missing.";
        return false;
    }
    nlohmann::json meta = nlohmann::json::parse(mf, nullptr, false);
    if (meta.is_discarded()) {
        output = "Backup metadata is not valid JSON.";
        return false;
    }
    const std::string packageName = meta.value("packageName", "");
    if (packageName.empty()) {
        output = "Backup metadata has no package name.";
        return false;
    }

    auto paths = meta["paths"];
    auto has = meta["has"];
    int stageTotal = 0;
    if (options.includeApk && has.value("apk", false)) stageTotal++;
    if (options.includeData && has.value("data", false)) stageTotal++;
    if (options.includeDeviceProtectedData && has.value("data_de", false)) stageTotal++;
    if (options.includeExternalData && has.value("external", false)) stageTotal++;
    if (options.includeObb && has.value("obb", false)) stageTotal++;
    if (options.includeMedia && has.value("media", false)) stageTotal++;
    if (options.grantRuntimePermissions && meta.contains("permissions")) stageTotal++;
    if (stageTotal <= 0) stageTotal = 1;
    int stageIndex = 0;
    auto emitProgress = [&](const std::string& label, int stage, uint64_t done = 0, uint64_t total = 0) {
        if (!progress) return true;
        AppBackupProgress p;
        p.packageName = packageName;
        p.stageLabel = label;
        p.stageIndex = stage;
        p.stageCount = stageTotal;
        p.bytesTransferred = done;
        p.bytesTotal = total;
        return progress(p);
    };

    if (options.includeApk && meta["has"].value("apk", false)) {
        stageIndex++;
        emitProgress("Installing APK", stageIndex);
        std::string args = "-s " + serial + " install";
        if (options.reinstall) args += " -r";
        if (options.grantRuntimePermissions) args += " -g";
        if (options.allowDowngrade) args += " -d";
        auto apkFiles = meta.value("apkFiles", nlohmann::json::array());
        if (apkFiles.size() > 1) args = "-s " + serial + " install-multiple" + (options.reinstall ? " -r" : "") +
            (options.grantRuntimePermissions ? " -g" : "") + (options.allowDowngrade ? " -d" : "");
        for (auto& apk : apkFiles) {
            std::filesystem::path p = dir / apk.value("name", "");
            args += " " + quoteCmdArg(pathutil::pathToUtf8(p));
        }
        const std::string installOut = runAdbCommand(args, 10 * 60 * 1000);
        output += installOut + "\n";
        if (!isAdbSuccess(installOut)) return false;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    runAdbCommand("-s " + serial + " shell su -c " +
        quoteCmdArg("am force-stop --user 0 " + packageName + " 2>/dev/null"));

    auto rootPathExists = [&](const std::string& path) {
        const std::string out = runAdbCommand("-s " + serial + " shell su -c " +
            quoteCmdArg("test -d " + path + " && echo yes"));
        return out.find("yes") != std::string::npos;
    };
    auto ownerContext = [&](const std::string& path) {
        const std::string out = runAdbCommand("-s " + serial + " shell su -c " +
            quoteCmdArg("stat -c '%u:%g %C' " + path + " 2>/dev/null"));
        return sanitizeAdbOutput(out);
    };
    auto restoreTar = [&](const char* label, const char* filename, const std::string& target,
                          bool enabled, bool present) -> bool {
        if (!enabled || !present) return true;
        stageIndex++;
        std::filesystem::path archive = dir / filename;
        if (!std::filesystem::exists(archive)) {
            output += std::string(label) + " archive is missing.\n";
            return false;
        }
        std::error_code archiveEc;
        uint64_t archiveSize = std::filesystem::file_size(archive, archiveEc);
        if (archiveEc) archiveSize = 0;
        emitProgress(std::string(label) + ": pushing archive", stageIndex, 0, archiveSize);
        std::string uidgidcon;
        const bool existed = rootPathExists(target);
        if (existed) uidgidcon = ownerContext(target);
        const std::string remote = "/data/local/tmp/afm-" + sanitizePathPart(packageName) + "-" + filename;
        if (m_connected) {
            auto transferProgress = [&](uint64_t done, uint64_t total) {
                return emitProgress(std::string(label) + ": pushing archive", stageIndex, done, total);
            };
            if (!extractRemoteArchive(pathutil::pathToUtf8(archive), target, transferProgress)) {
                output += std::string(label) + " extract failed: " + m_lastError + "\n";
                return false;
            }
            emitProgress(std::string(label) + ": extracting archive", stageIndex, archiveSize, archiveSize);
        } else {
            const std::string pushOut = runAdbCommand("-s " + serial + " push " +
                quoteCmdArg(pathutil::pathToUtf8(archive)) + " " + remote, 30 * 60 * 1000);
            output += pushOut + "\n";
            if (pushOut.find("error") != std::string::npos || pushOut.find("failed") != std::string::npos)
                return false;
            const std::string cmd =
                "mkdir -p " + target +
                " && rm -rf " + target + "/*" +
                " && tar -xpf " + remote + " -C " + target +
                " && rm -f " + remote;
            const std::string out = runAdbCommand("-s " + serial + " shell su -c " +
                quoteCmdArg(cmd), 30 * 60 * 1000);
            output += out + "\n";
            std::string low = out;
            std::transform(low.begin(), low.end(), low.begin(), ::tolower);
            if (low.find("error") != std::string::npos || low.find("failed") != std::string::npos ||
                low.find("cannot") != std::string::npos)
                return false;
        }
        if (!uidgidcon.empty()) {
            std::istringstream iss(uidgidcon);
            std::string owner, context;
            iss >> owner >> context;
            if (!owner.empty()) {
                std::string fix = "chown -R " + owner + " " + target + " ; restorecon -RF " + target + " 2>/dev/null";
                if (!context.empty() && context != "?")
                    fix += " ; chcon -R -h " + context + " " + target + " 2>/dev/null";
                output += runAdbCommand("-s " + serial + " shell su -c " + quoteCmdArg(fix),
                                        30 * 60 * 1000) + "\n";
            }
        }
        emitProgress(std::string(label) + ": restored", stageIndex, archiveSize, archiveSize);
        return true;
    };
    bool ok = true;
    ok = restoreTar("App data", "data.tar", paths.value("data", "/data/data/" + packageName),
                    options.includeData, has.value("data", false)) && ok;
    ok = restoreTar("Device protected data", "data_de.tar",
                    paths.value("data_de", "/data/user_de/0/" + packageName),
                    options.includeDeviceProtectedData, has.value("data_de", false)) && ok;
    ok = restoreTar("External data", "external.tar",
                    paths.value("external", "/sdcard/Android/data/" + packageName),
                    options.includeExternalData, has.value("external", false)) && ok;
    ok = restoreTar("OBB data", "obb.tar", paths.value("obb", "/sdcard/Android/obb/" + packageName),
                    options.includeObb, has.value("obb", false)) && ok;
    ok = restoreTar("Media data", "media.tar", paths.value("media", "/sdcard/Android/media/" + packageName),
                    options.includeMedia, has.value("media", false)) && ok;

    if (options.grantRuntimePermissions && meta.contains("permissions")) {
        stageIndex++;
        emitProgress("Restoring permissions", stageIndex);
        std::filesystem::path permissionList = std::filesystem::temp_directory_path() /
            ("afm-permissions-" + sanitizePathPart(packageName) + ".txt");
        std::ofstream permissionFile(permissionList, std::ios::binary | std::ios::trunc);
        int permissionCount = 0;
        if (!permissionFile) {
            LOG_ERROR("BackupEngine", "Could not create runtime permission list for " + packageName);
        } else {
            for (auto& p : meta["permissions"]) {
                if (!p.is_string()) continue;
                permissionFile << p.get<std::string>() << '\n';
                permissionCount++;
            }
            permissionFile.close();
        }
        if (permissionCount > 0) {
            const std::string remoteList = "/data/local/tmp/afm-permissions-" +
                sanitizePathPart(packageName) + ".txt";
            const std::string pushOut = runAdbCommand("-s " + serial + " push " +
                quoteCmdArg(pathutil::pathToUtf8(permissionList)) + " " + remoteList);
            if (pushOut.find("error") == std::string::npos && pushOut.find("failed") == std::string::npos) {
                const std::string grantScript = "while IFS= read -r permission; do pm grant --user 0 " +
                    shellQuote(packageName) + " \"$permission\" 2>/dev/null; done < " +
                    shellQuote(remoteList) + "; rm -f " + shellQuote(remoteList);
                runAdbCommand("-s " + serial + " shell su -c " + quoteCmdArg(grantScript));
                LOG_INFO("BackupEngine", "Restored " + std::to_string(permissionCount) +
                    " runtime permissions in one root shell for " + packageName);
            } else {
                LOG_ERROR("BackupEngine", "Could not push runtime permission list for " + packageName + ": " +
                    sanitizeAdbOutput(pushOut));
            }
            std::error_code permissionEc;
            std::filesystem::remove(permissionList, permissionEc);
        }
    }

    if (ok) output += "Restore complete.";
    emitProgress("Complete", stageTotal, 0, 0);
    return ok;
}

bool DeviceClient::restoreStagedBackupArchive(const std::string& remoteArchive,
                                              const std::vector<ArchiveRestoreSelection>& selections,
                                              const AppBackupOptions& options,
                                              std::vector<bool>& restored,
                                              std::string& output,
                                              InstallProgressCallback progress) {
    restored.assign(selections.size(), false);
    output.clear();
    if (!m_connected || selections.empty()) {
        m_lastError = m_connected ? "No archive restore selections were provided" : "Not connected";
        output = m_lastError;
        return false;
    }
    const auto restoreStarted = std::chrono::steady_clock::now();
    nlohmann::json request;
    request["archive"] = remoteArchive;
    request["includeApk"] = options.includeApk;
    request["includeData"] = options.includeData;
    request["includeDeviceProtectedData"] = options.includeDeviceProtectedData;
    request["includeExternalData"] = options.includeExternalData;
    request["includeObb"] = options.includeObb;
    request["includeMedia"] = options.includeMedia;
    request["grantRuntimePermissions"] = options.grantRuntimePermissions;
    request["reinstall"] = options.reinstall;
    request["allowDowngrade"] = options.allowDowngrade;
    request["parallelism"] = 4;
    request["apps"] = nlohmann::json::array();
    std::vector<std::string> restoreAppNames;
    std::vector<std::string> restorePackageNames;
    restoreAppNames.reserve(selections.size());
    restorePackageNames.reserve(selections.size());
    for (const auto& selection : selections) {
        nlohmann::json metadata = nlohmann::json::parse(selection.metadataJson, nullptr, false);
        if (metadata.is_discarded()) {
            output = "A selected app has invalid backup metadata";
            return false;
        }
        restoreAppNames.push_back(metadata.value("label", ""));
        restorePackageNames.push_back(metadata.value("packageName", ""));
        request["apps"].push_back({{"prefix", selection.prefix}, {"metadata", std::move(metadata)}});
    }
    const std::string encoded = request.dump();

    std::lock_guard<std::mutex> lock(m_mutex);
    const uintptr_t sock = m_socket;
    if (sock == ~(uintptr_t)0) {
        output = "Not connected";
        return false;
    }
    MsgHeader header{};
    std::vector<char> payload;
    const bool received = sendMsgOn(sock, CMD_RESTORE_BACKUP_ARCHIVE, encoded.data(),
                                    static_cast<uint32_t>(encoded.size())) &&
        recvMsgOn(sock, header, payload, kBackupOpMs);
    if (!received || header.cmd != RSP_OK) {
        if (received && header.cmd == RSP_ERROR) output.assign(payload.begin(), payload.end());
        else output = m_lastError.empty() ? "The device did not accept the restore archive" : m_lastError;
        return false;
    }
    const double stagingSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - restoreStarted).count();
    LOG_INFO("BackupManager", "Android restore staging completed in " +
        std::to_string(stagingSeconds) + " s, starting " +
        std::to_string(selections.size()) + " app restore(s) with 4 workers");

    bool protocolOk = false;
    while (recvMsgOn(sock, header, payload, kBackupOpMs)) {
        if (header.cmd == RSP_DONE) {
            protocolOk = true;
            break;
        }
        if (header.cmd == RSP_ERROR) {
            output.append(payload.begin(), payload.end());
            break;
        }
        if (header.cmd != RSP_DATA) break;
        nlohmann::json event = nlohmann::json::parse(payload.begin(), payload.end(), nullptr, false);
        if (event.is_discarded()) continue;
        const size_t index = event.value("index", selections.size());
        if (index >= selections.size()) continue;
        const std::string state = event.value("state", "");
        if (state == "complete") {
            restored[index] = event.value("success", false);
            if (!restored[index]) {
                output += friendlyRestoreFailure(event.value("output", ""), restoreAppNames[index],
                    restorePackageNames[index], options.allowDowngrade) + "\n\n";
            }
        }
        if (progress) {
            AppInstallProgress update;
            update.packageName = event.value("packageName", "");
            update.stageLabel = event.value("stage", state);
            update.backupIndex = static_cast<int>(index);
            update.completedApps = event.value("completed", 0);
            update.totalApps = static_cast<int>(selections.size());
            update.appProgress = event.value("progress", state == "complete" ? 1.0f : 0.0f);
            progress(update);
        }
    }
    if (!protocolOk && output.empty()) output = "The device returned an incomplete archive restore result";
    const double totalSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - restoreStarted).count();
    LOG_INFO("BackupManager", "Android archive restore processing finished in " +
        std::to_string(totalSeconds) + " s");
    return protocolOk && std::all_of(restored.begin(), restored.end(), [](bool value) { return value; });
}
