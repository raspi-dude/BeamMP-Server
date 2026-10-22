// BeamMP, the BeamNG.drive multiplayer mod.
// Copyright (C) 2024 BeamMP Ltd., BeamMP team and contributors.
//
// BeamMP Ltd. can be contacted by electronic mail via contact@beammp.com.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "TResourceManager.h"
#include "Common.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fmt/core.h>
#include <ios>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

namespace fs = std::filesystem;

namespace {

std::string HumanSize(size_t Bytes) {
    constexpr const char* Units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    constexpr size_t UnitCount = sizeof(Units) / sizeof(Units[0]);
    double Value = static_cast<double>(Bytes);
    size_t Unit = 0;
    while (Value >= 1024.0 && Unit + 1 < UnitCount) {
        Value /= 1024.0;
        ++Unit;
    }
    if (Unit == 0) {
        return fmt::format("{} {}", Bytes, Units[Unit]);
    }
    return fmt::format("{:.1f} {}", Value, Units[Unit]);
}

/// A plain ASCII bar so it renders the same in a terminal, a log file and the
/// Pterodactyl console.
std::string ProgressBar(double Fraction, size_t Width) {
    Fraction = std::clamp(Fraction, 0.0, 1.0);
    const auto Filled = static_cast<size_t>(Fraction * static_cast<double>(Width) + 0.5);
    std::string Bar(Width, '-');
    for (size_t i = 0; i < Filled && i < Width; ++i) {
        Bar[i] = '#';
    }
    return Bar;
}

} // namespace

TResourceManager::TResourceManager() {
    Application::SetSubsystemStatus("ResourceManager", Application::Status::Starting);
    std::string Path = Application::Settings.getAsString(Settings::Key::General_ResourceFolder) + "/Client";
    if (!fs::exists(Path))
        fs::create_directories(Path);
    for (const auto& entry : fs::directory_iterator(Path)) {
        std::string File(entry.path().string());
        if (auto pos = File.find(".zip"); pos != std::string::npos) {
            if (File.length() - pos == 4) {
                std::replace(File.begin(), File.end(), '\\', '/');
                mFileList += File + ';';
                if (auto i = File.find_last_of('/'); i != std::string::npos) {
                    ++i;
                    File = File.substr(i, pos - i);
                }
                mTrimmedList += "/" + fs::path(File).filename().string() + ';';
                mFileSizes += std::to_string(size_t(fs::file_size(entry.path()))) + ';';
                mMaxModSize += size_t(fs::file_size(entry.path()));
                mModsLoaded++;
            }
        }
    }

    if (mModsLoaded) {
        beammp_infof("Found {} mod(s) ({}); checking them next - large files are hashed once and then cached",
            mModsLoaded, HumanSize(mMaxModSize));
    }

    Application::SetSubsystemStatus("ResourceManager", Application::Status::Good);
}

void TResourceManager::RefreshFiles() {
    mMods.clear();
    std::unique_lock Lock(mModsMutex);

    std::string Path = Application::Settings.getAsString(Settings::Key::General_ResourceFolder) + "/Client";
    const auto StartTime = std::chrono::steady_clock::now();
    size_t HashedCount = 0;
    size_t CachedCount = 0;

    beammp_info("Checking mods: unchanged files are loaded from mods.json, new or modified ones are hashed "
                "(a large mod can take a while - the server is busy, not frozen)");
    nlohmann::json modsDB;

    if (std::filesystem::exists(Path + "/mods.json")) {
        try {
            std::ifstream stream(Path + "/mods.json");

            stream >> modsDB;

            stream.close();
        } catch (const std::exception& e) {
            beammp_errorf("Failed to load mods.json: {}", e.what());
        }
    }

    for (const auto& entry : fs::directory_iterator(Path)) {
        std::string File(entry.path().string());

        if (entry.path().filename().string() == "mods.json") {
            continue;
        }

        if (entry.path().extension() != ".zip" || std::filesystem::is_directory(entry.path())) {
            beammp_warnf("'{}' is not a ZIP file and will be ignored", File);
            continue;
        }

        if (modsDB.contains(entry.path().filename().string())) {
            auto& dbEntry = modsDB[entry.path().filename().string()];
            if (entry.last_write_time().time_since_epoch().count() > dbEntry["lastwrite"] || std::filesystem::file_size(File) != dbEntry["filesize"].get<size_t>()) {
                beammp_infof("File '{}' has been modified, rehashing", File);
            } else {
                dbEntry["exists"] = true;

                mMods.push_back(nlohmann::json {
                    { "file_name", std::filesystem::path(File).filename() },
                    { "file_size", std::filesystem::file_size(File) },
                    { "hash_algorithm", "sha256" },
                    { "hash", dbEntry["hash"] },
                    { "protected", dbEntry["protected"] } });

                beammp_debugf("Mod '{}' loaded from cache", File);
                ++CachedCount;

                continue;
            }
        }

        try {
            EVP_MD_CTX* mdctx;
            const EVP_MD* md;
            uint8_t sha256_value[EVP_MAX_MD_SIZE];
            md = EVP_sha256();
            if (md == nullptr) {
                throw std::runtime_error("EVP_sha256() failed");
            }

            mdctx = EVP_MD_CTX_new();
            if (mdctx == nullptr) {
                throw std::runtime_error("EVP_MD_CTX_new() failed");
            }
            if (!EVP_DigestInit_ex2(mdctx, md, NULL)) {
                EVP_MD_CTX_free(mdctx);
                throw std::runtime_error("EVP_DigestInit_ex2() failed");
            }

            std::ifstream stream(File, std::ios::binary);

            const size_t FileSize = std::filesystem::file_size(File);
            const std::string ModName = entry.path().filename().string();
            beammp_infof("Hashing '{}' ({}) ...", ModName, HumanSize(FileSize));
            const auto HashStarted = std::chrono::steady_clock::now();
            auto LastReport = HashStarted;
            constexpr size_t ReadBufferSize = 1024 * 1024; // larger sequential reads keep HDDs busy
            size_t Read = 0;
            std::vector<char> Data(std::min<size_t>(FileSize, ReadBufferSize));
            while (Read < FileSize) {
                stream.read(Data.data(), std::streamsize(Data.size()));
                const size_t RealDataSize = size_t(stream.gcount());
                if (RealDataSize == 0) {
                    break;
                }
                if (!EVP_DigestUpdate(mdctx, Data.data(), RealDataSize)) {
                    EVP_MD_CTX_free(mdctx);
                    throw std::runtime_error("EVP_DigestUpdate() failed");
                }
                Read += RealDataSize;
                // One line per second at most: a progress bar in a log file is
                // useful, a bar per megabyte is noise.
                if (const auto Now = std::chrono::steady_clock::now();
                    Read < FileSize && Now - LastReport >= std::chrono::seconds(1)) {
                    LastReport = Now;
                    // Only read by the log line below, which compiles to nothing
                    // in builds without a console (the test target).
                    [[maybe_unused]] const double Fraction = static_cast<double>(Read) / static_cast<double>(FileSize);
                    beammp_infof("  '{}' [{}] {:3.0f}%", ModName, ProgressBar(Fraction, 24), Fraction * 100.0);
                }
            }
            unsigned int sha256_len = 0;
            if (!EVP_DigestFinal_ex(mdctx, sha256_value, &sha256_len)) {
                EVP_MD_CTX_free(mdctx);
                throw std::runtime_error("EVP_DigestFinal_ex() failed");
            }
            EVP_MD_CTX_free(mdctx);

            stream.close();

            std::string result;
            for (size_t i = 0; i < sha256_len; i++) {
                result += fmt::format("{:02x}", sha256_value[i]);
            }
            beammp_debugf("sha256('{}'): {}", File, result);
            beammp_infof("  '{}' hashed in {:.2f}s", ModName,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - HashStarted).count());
            ++HashedCount;
            mMods.push_back(nlohmann::json {
                { "file_name", std::filesystem::path(File).filename() },
                { "file_size", std::filesystem::file_size(File) },
                { "hash_algorithm", "sha256" },
                { "hash", result },
                { "protected", false } });

            modsDB[std::filesystem::path(File).filename().string()] = {
                { "lastwrite", entry.last_write_time().time_since_epoch().count() },
                { "hash", result },
                { "filesize", std::filesystem::file_size(File) },
                { "protected", false },
                { "exists", true }
            };

        } catch (const std::exception& e) {
            beammp_errorf("Sha256 hashing of '{}' failed: {}", File, e.what());
        }
    }

    for (auto it = modsDB.begin(); it != modsDB.end();) {
        if (!it.value().contains("exists")) {
            it = modsDB.erase(it);
        } else {
            it.value().erase("exists");
            ++it;
        }
    }

    try {
        std::ofstream stream(Path + "/mods.json");

        stream << modsDB.dump(4);

        stream.close();
    } catch (std::exception& e) {
        beammp_error("Failed to update mod DB: " + std::string(e.what()));
    }

    if (HashedCount > 0 || CachedCount > 0) {
        beammp_infof("Mod check finished: {} hashed, {} from cache, {:.1f}s total", HashedCount, CachedCount,
            std::chrono::duration<double>(std::chrono::steady_clock::now() - StartTime).count());
    }
}

TEST_CASE("ResourceManager::HumanSize") {
    CHECK(HumanSize(0) == "0 B");
    CHECK(HumanSize(1023) == "1023 B");
    CHECK(HumanSize(1024) == "1.0 KiB");
    CHECK(HumanSize(1536) == "1.5 KiB");
    CHECK(HumanSize(5 * 1024 * 1024) == "5.0 MiB");
}

TEST_CASE("ResourceManager::ProgressBar") {
    CHECK(ProgressBar(0.0, 10) == "----------");
    CHECK(ProgressBar(0.5, 10) == "#####-----");
    CHECK(ProgressBar(1.0, 10) == "##########");
    // out of range input is clamped instead of producing a wrong-width bar
    CHECK(ProgressBar(-1.0, 10) == "----------");
    CHECK(ProgressBar(2.0, 10) == "##########");
}

void TResourceManager::SetProtected(const std::string& ModName, bool Protected) {
    std::unique_lock Lock(mModsMutex);

    for (auto& mod : mMods) {
        if (mod["file_name"].get<std::string>() == ModName) {
            mod["protected"] = Protected;
            break;
        }
    }

    auto modsDBPath = Application::Settings.getAsString(Settings::Key::General_ResourceFolder) + "/Client/mods.json";

    if (std::filesystem::exists(modsDBPath)) {
        try {
            nlohmann::json modsDB;

            std::fstream stream(modsDBPath);

            stream >> modsDB;

            if (modsDB.contains(ModName)) {
                modsDB[ModName]["protected"] = Protected;
            }

            stream.clear();
            stream.seekp(0, std::ios::beg);

            stream << modsDB.dump(4);

            stream.close();
        } catch (const std::exception& e) {
            beammp_errorf("Failed to update mods.json: {}", e.what());
        }
    }
}
