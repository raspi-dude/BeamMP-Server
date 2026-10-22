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

/// True when this zip has to be hashed: a new file, or one whose mtime/size no
/// longer match what mods.json recorded. RefreshFiles uses the same check twice,
/// once to decide whether to print the "checking mods" lines at all.
bool ModNeedsHash(const std::filesystem::directory_entry& Entry, const nlohmann::json& ModsDB) {
    const auto It = ModsDB.find(Entry.path().filename().string());
    if (It == ModsDB.end() || !It->contains("lastwrite") || !It->contains("filesize")) {
        return true;
    }
    try {
        return Entry.last_write_time().time_since_epoch().count() > It->at("lastwrite").get<int64_t>()
            || std::filesystem::file_size(Entry.path()) != It->at("filesize").get<size_t>();
    } catch (const std::exception&) {
        return true; // unreadable or malformed entry: hash it again
    }
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

    Application::SetSubsystemStatus("ResourceManager", Application::Status::Good);
}

void TResourceManager::RefreshFiles() {
    mMods.clear();
    std::unique_lock Lock(mModsMutex);

    std::string Path = Application::Settings.getAsString(Settings::Key::General_ResourceFolder) + "/Client";
    const auto StartTime = std::chrono::steady_clock::now();
    size_t HashedCount = 0;
    size_t CachedCount = 0;

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

    // A restart with unchanged mods is the normal case; it must not print three
    // lines about hashing. Only changes get the detailed output.
    bool Changes = false;
    for (const auto& entry : fs::directory_iterator(Path)) {
        if (entry.path().filename().string() == "mods.json"
            || entry.path().extension() != ".zip" || std::filesystem::is_directory(entry.path())) {
            continue;
        }
        if (ModNeedsHash(entry, modsDB)) {
            Changes = true;
            break;
        }
    }

    if (Changes) {
        beammp_infof("Found {} mod(s) ({}); checking them next - large files are hashed once and then cached",
            mModsLoaded, HumanSize(mMaxModSize));
        beammp_info("Checking mods: unchanged files are loaded from mods.json, new or modified ones are hashed "
                    "(a large mod can take a while - the server is busy, not frozen)");
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

        if (ModNeedsHash(entry, modsDB)) {
            if (modsDB.contains(entry.path().filename().string())) {
                beammp_infof("File '{}' has been modified, rehashing", File);
            }
        } else {
            // unchanged: reuse the hash from mods.json instead of reading the zip
            auto& dbEntry = modsDB[entry.path().filename().string()];
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

    if (Changes) {
        beammp_infof("Mod check finished: {} hashed, {} from cache, {:.1f}s total", HashedCount, CachedCount,
            std::chrono::duration<double>(std::chrono::steady_clock::now() - StartTime).count());
    } else if (mModsLoaded > 0) {
        beammp_infof("Loaded {} mod(s) ({}) from cache - unchanged since the last start", mModsLoaded,
            HumanSize(mMaxModSize));
    }
}

TEST_CASE("ResourceManager::ModNeedsHash") {
    const auto Dir = fs::temp_directory_path() / "rh-mod-needs-hash";
    std::error_code Ec;
    fs::remove_all(Dir, Ec);
    fs::create_directories(Dir, Ec);
    {
        std::ofstream(Dir / "a.zip") << "x";
    }
    const fs::directory_entry Entry(Dir / "a.zip");

    nlohmann::json ModsDB = nlohmann::json::object();
    // not in mods.json yet
    CHECK(ModNeedsHash(Entry, ModsDB));

    ModsDB["a.zip"] = {
        { "lastwrite", Entry.last_write_time().time_since_epoch().count() },
        { "filesize", 1 },
    };
    CHECK_FALSE(ModNeedsHash(Entry, ModsDB));

    // a different size means the file was replaced
    ModsDB["a.zip"]["filesize"] = 2;
    CHECK(ModNeedsHash(Entry, ModsDB));

    // an older mtime than the file means it was modified
    ModsDB["a.zip"]["filesize"] = 1;
    ModsDB["a.zip"]["lastwrite"] = int64_t(0);
    CHECK(ModNeedsHash(Entry, ModsDB));

    // malformed entries are re-hashed instead of throwing
    ModsDB["a.zip"] = { { "lastwrite", "not-a-number" }, { "filesize", 1 } };
    CHECK(ModNeedsHash(Entry, ModsDB));
    // an entry missing one of the fields counts as changed too
    ModsDB["a.zip"] = { { "filesize", 1 } };
    CHECK(ModNeedsHash(Entry, ModsDB));

    fs::remove_all(Dir, Ec);
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
