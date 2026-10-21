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
#include <filesystem>
#include <fmt/core.h>
#include <ios>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

namespace fs = std::filesystem;

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
        beammp_info("Loaded " + std::to_string(mModsLoaded) + " Mods");
    }

    Application::SetSubsystemStatus("ResourceManager", Application::Status::Good);
}

void TResourceManager::RefreshFiles() {
    mMods.clear();
    std::unique_lock Lock(mModsMutex);

    std::string Path = Application::Settings.getAsString(Settings::Key::General_ResourceFolder) + "/Client";

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
