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

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

/// Raspi Host server manager: the tier decides whether an empty server shuts
/// itself down, and every tier records the usage data the panel uses to find
/// servers nobody touches.
///
/// This replaced a Lua plugin that lived in Resources/Server. Keeping it in the
/// binary means it still runs if that folder is cleared, and it can take part in
/// the real (graceful) shutdown instead of calling exit() from a script.
class TRaspiHostManager {
public:
    enum class ETier {
        Free, ///< 15 minutes of an empty server, then shutdown
        Eco, ///< 60 minutes of an empty server, then shutdown
        Performance, ///< never shuts down on inactivity
    };

    /// Statistics persisted next to the server config, so clearing
    /// Resources/Server cannot take them (and the server's access time) with it.
    struct TStats {
        bool Enabled { true };
        uint64_t TimesStarted { 0 };
        uint64_t TotalMinsOnline { 0 };
        int64_t LastOnline { 0 }; ///< unix seconds, updated while running
        int64_t FirstOnline { 0 }; ///< unix seconds of the very first start
    };

    static std::string_view TierName(ETier Tier);
    /// 900 for free, 3600 for eco, 0 for performance.
    static int ShutdownSecondsFor(ETier Tier);
    /// The colour of the boot banner: an upsell on free, a thank-you otherwise.
    static bool WantsUpsell(ETier Tier);

    /// "3 minute(s) ago", "Just now", "Never".
    static std::string FormatTimestamp(int64_t Timestamp, int64_t Now);

    /// Parses the key=value stats file. Unknown keys are ignored so older and
    /// newer files both load.
    static TStats ParseStats(std::string_view Content);
    /// The exact file format the panel reads, now including TIER.
    static std::string SerializeStats(const TStats& Stats, ETier Tier, int64_t Now);

    /// Loads the stats file, moves an old plugin file out of Resources/Server if
    /// that is the only copy, records this start and prints the banner.
    ///
    /// @param ConfigDir directory holding ServerConfig.toml
    /// @param LegacyStatsFile the Lua plugin's file, migrated when present
    void Initialize(ETier Tier, const std::filesystem::path& ConfigDir,
        const std::filesystem::path& LegacyStatsFile);

    /// Called from the main loop; only does work about once per second.
    /// Returns true when the server has to shut down now.
    bool Tick(size_t PlayersOnline);

    /// One step of the timer, for Callers that know how much time passed (and
    /// for tests, which cannot wait 15 minutes). Tick() calls this once a second.
    /// Returns true when the server has to shut down now.
    bool TickWithElapsed(size_t PlayersOnline, int ElapsedSeconds);

    /// Writes the stats file (creating the directory if needed).
    void SaveStats();

    [[nodiscard]] ETier Tier() const { return mTier; }
    [[nodiscard]] int IdleSeconds() const { return mIdleSeconds; }
    [[nodiscard]] const TStats& Stats() const { return mStats; }
    [[nodiscard]] const std::filesystem::path& StatsPath() const { return mStatsPath; }

private:
    /// "15 min of an empty server", or "off (this plan never shuts down)".
    std::string ShutdownDescription() const;
    void PrintBanner(int64_t PreviousLastOnline) const;

    ETier mTier { ETier::Free };
    TStats mStats {};
    std::filesystem::path mStatsPath {};
    int mShutdownSeconds { 0 };
    int mIdleSeconds { 0 };
    int mTicksSinceSave { 0 };
    std::chrono::steady_clock::time_point mLastTick {};
    bool mTickStarted { false };
    bool mInitialized { false };
};
