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

#include "TRaspiHostManager.h"

#include "Common.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

namespace {

/// Longest jump Tick() accepts in one call, in seconds. The main loop runs
/// every 100 ms; anything much longer means the process was stalled.
constexpr int64_t MaxTickJump = 5;

int64_t NowSeconds() {
    return static_cast<int64_t>(std::time(nullptr));
}

std::string TrimLine(std::string Text) {
    while (!Text.empty() && (Text.back() == '\r' || Text.back() == '\n' || Text.back() == ' ')) {
        Text.pop_back();
    }
    size_t Begin = 0;
    while (Begin < Text.size() && (Text[Begin] == ' ' || Text[Begin] == '\t')) {
        ++Begin;
    }
    return Text.substr(Begin);
}

/// One line of the file: KEY=value. Comments and blank lines are skipped.
bool SplitKeyValue(const std::string& Line, std::string& Key, std::string& Value) {
    if (Line.empty() || Line[0] == '-' || Line[0] == '#') {
        return false;
    }
    const auto Assign = Line.find('=');
    if (Assign == std::string::npos) {
        return false;
    }
    Key = TrimLine(Line.substr(0, Assign));
    Value = TrimLine(Line.substr(Assign + 1));
    return !Key.empty();
}

/// Reads a stats file into memory. Returns false when it cannot be opened, so
/// an unreadable file never stops the server from starting.
bool ReadFile(const std::filesystem::path& Path, std::string& Out) {
    std::ifstream File(Path, std::ios::binary);
    if (!File) {
        return false;
    }
    std::ostringstream Content;
    Content << File.rdbuf();
    Out = Content.str();
    return true;
}

/// Servers that ran the old Lua plugin still have it in Resources/Server, and it
/// would keep counting and shutting down next to this manager. Point it out
/// instead of deleting somebody's file.
void WarnAboutLegacyPlugin(const std::filesystem::path& ServerResourceDir) {
    std::error_code Ec;
    if (ServerResourceDir.empty() || !std::filesystem::is_directory(ServerResourceDir, Ec)) {
        return;
    }
    for (auto It = std::filesystem::directory_iterator(ServerResourceDir, Ec);
         !Ec && It != std::filesystem::directory_iterator(); It.increment(Ec)) {
        if (!It->is_regular_file(Ec) || It->path().extension() != ".lua") {
            continue;
        }
        std::string Content;
        if (!ReadFile(It->path(), Content)) {
            continue;
        }
        if (Content.find("RH Server Manager") != std::string::npos || Content.find("RaspiHostUtilsLogs") != std::string::npos) {
            beammp_warnf("[Raspi Host] the old manager plugin {} is still installed and keeps running its own timer; delete it so only the built-in manager acts", It->path().string());
        }
    }
}

int64_t AsInt64(const std::string& Text, int64_t Fallback) {
    try {
        return static_cast<int64_t>(std::stoll(Text));
    } catch (const std::exception&) {
        return Fallback;
    }
}

} // namespace

std::string_view TRaspiHostManager::TierName(ETier Tier) {
    switch (Tier) {
    case ETier::Free:
        return "Free";
    case ETier::Eco:
        return "Eco";
    case ETier::Performance:
        return "Performance";
    }
    return "Free";
}

int TRaspiHostManager::ShutdownSecondsFor(ETier Tier) {
    switch (Tier) {
    case ETier::Free:
        return 15 * 60;
    case ETier::Eco:
        return 60 * 60;
    case ETier::Performance:
        return 0; // disabled
    }
    return 15 * 60;
}

bool TRaspiHostManager::WantsUpsell(ETier Tier) {
    return Tier == ETier::Free;
}

TRaspiHostManager::TStats TRaspiHostManager::ParseStats(std::string_view Content) {
    TStats Stats;
    std::istringstream Stream { std::string(Content) };
    std::string Line;
    while (std::getline(Stream, Line)) {
        std::string Key;
        std::string Value;
        if (!SplitKeyValue(TrimLine(Line), Key, Value)) {
            continue;
        }
        if (Key == "STATS_ENABLED") {
            Stats.Enabled = Value != "false";
        } else if (Key == "TIMES_STARTED") {
            Stats.TimesStarted = static_cast<uint64_t>((std::max)(int64_t { 0 }, AsInt64(Value, 0)));
        } else if (Key == "TOTAL_MINS_ONLINE") {
            Stats.TotalMinsOnline = static_cast<uint64_t>((std::max)(int64_t { 0 }, AsInt64(Value, 0)));
        } else if (Key == "LAST_ONLINE") {
            Stats.LastOnline = AsInt64(Value, 0);
        } else if (Key == "FIRST_ONLINE") {
            Stats.FirstOnline = AsInt64(Value, 0);
        }
        // TIER, LAST_UPDATED and anything newer are written by us, not read.
    }
    return Stats;
}

std::string TRaspiHostManager::SerializeStats(const TStats& Stats, ETier Tier, int64_t Now) {
    std::ostringstream Out;
    Out << "-- DO NOT DELETE THIS FILE!! This file is used to track the last time this server was accessed,\n"
           "-- and if deleted, your server is at risk of being purged. We also track certain statistics\n"
           "-- related to server performance, but if you would like to disable it please change the setting below.\n"
           "\n"
           "STATS_ENABLED="
        << (Stats.Enabled ? "true" : "false") << "\n"
        << "\n";

    if (Stats.Enabled) {
        Out << "-- Server Statistics --\n"
            << "TIMES_STARTED=" << Stats.TimesStarted << "\n"
            << "TOTAL_MINS_ONLINE=" << Stats.TotalMinsOnline << "\n"
            << "LAST_ONLINE=" << Stats.LastOnline << "\n"
            << "FIRST_ONLINE=" << Stats.FirstOnline << "\n";
    } else {
        Out << "-- Statistics tracking is disabled --\n";
    }

    Out << "\n-- Plan this server runs on: free, eco or performance --\n"
        << "TIER=" << (Tier == ETier::Free ? "free" : Tier == ETier::Eco ? "eco" : "performance") << "\n"
        << "\n"
        << "LAST_UPDATED=" << Now << "\n";
    return Out.str();
}

std::string TRaspiHostManager::FormatTimestamp(int64_t Timestamp, int64_t Now) {
    if (Timestamp <= 0) {
        return "Never";
    }
    const int64_t Diff = (std::max)(int64_t { 0 }, Now - Timestamp);
    const int64_t Days = Diff / 86400;
    const int64_t Hours = (Diff % 86400) / 3600;
    const int64_t Minutes = (Diff % 3600) / 60;
    if (Days > 0) {
        return std::to_string(Days) + " day(s) ago";
    }
    if (Hours > 0) {
        return std::to_string(Hours) + " hour(s) ago";
    }
    if (Minutes > 0) {
        return std::to_string(Minutes) + " minute(s) ago";
    }
    return "Just now";
}

std::string TRaspiHostManager::ShutdownDescription() const {
    if (mShutdownSeconds <= 0) {
        return "off (this plan never shuts down)";
    }
    return std::to_string(mShutdownSeconds / 60) + " min of an empty server";
}

void TRaspiHostManager::PrintBanner(int64_t PreviousLastOnline) const {
    std::ostringstream Out;
    Out << "########################################################################\n"
        << "#                    ——— Raspi Host server manager ———                  #\n"
        << "#  Plan: " << TierName(mTier) << "\n"
        << "#  Inactivity shutdown: " << ShutdownDescription() << "\n"
        << "#  Last accessed: " << FormatTimestamp(PreviousLastOnline, NowSeconds()) << "\n";
    if (WantsUpsell(mTier)) {
        Out << "#  Want 24/7 uptime and better performance? Consider one of our paid plans!\n";
    } else {
        Out << "#  Thank you for choosing Raspi Host!\n";
    }
    Out << "########################################################################";
    beammp_info(Out.str());
}

void TRaspiHostManager::Initialize(ETier Tier, const std::filesystem::path& ConfigDir,
    const std::filesystem::path& LegacyStatsFile) {
    mTier = Tier;
    mShutdownSeconds = ShutdownSecondsFor(Tier);
    mStatsPath = ConfigDir / "RaspiHostStats";

    bool Migrated = false;
    std::error_code Ec;

    std::string Content;
    if (std::filesystem::exists(mStatsPath, Ec)) {
        if (ReadFile(mStatsPath, Content)) {
            mStats = ParseStats(Content);
        } else {
            beammp_warnf("[Raspi Host] could not read {}, starting with empty stats", mStatsPath.string());
        }
    } else if (!LegacyStatsFile.empty() && std::filesystem::exists(LegacyStatsFile, Ec)) {
        // The old plugin kept this inside Resources/Server, where a server owner
        // could delete it (and with it the access time the cleanup uses). Move it
        // next to the config instead of leaving two files behind.
        if (ReadFile(LegacyStatsFile, Content)) {
            mStats = ParseStats(Content);
            Migrated = true;
        } else {
            beammp_warnf("[Raspi Host] could not read legacy stats {}", LegacyStatsFile.string());
        }
    }

    const int64_t PreviousLastOnline = mStats.LastOnline;
    const int64_t Now = NowSeconds();
    ++mStats.TimesStarted;
    if (mStats.FirstOnline == 0) {
        mStats.FirstOnline = Now;
    }
    mStats.LastOnline = Now;

    PrintBanner(PreviousLastOnline);
    SaveStats();

    // Resources/Server is the folder that held the plugin and its log folder.
    WarnAboutLegacyPlugin(LegacyStatsFile.parent_path().parent_path());

    if (Migrated) {
        std::error_code RemoveEc;
        std::filesystem::remove(LegacyStatsFile, RemoveEc);
        // Only tidy up the plugin's own directory; never touch Resources/Server.
        std::filesystem::remove(LegacyStatsFile.parent_path(), RemoveEc);
        beammp_infof("[Raspi Host] Moved the old stats file to {}", mStatsPath.string());
    }

    std::error_code AbsoluteEc;
    const auto AbsoluteStatsPath = std::filesystem::absolute(mStatsPath, AbsoluteEc);
    beammp_infof("[Raspi Host] Plan '{}'; inactivity shutdown: {}", TierName(mTier), ShutdownDescription());
    beammp_infof("[Raspi Host] Usage data: {} (stats tracking {})",
        (AbsoluteEc ? mStatsPath : AbsoluteStatsPath).string(), mStats.Enabled ? "ENABLED" : "DISABLED");

    mInitialized = true;
}

void TRaspiHostManager::SaveStats() {
    if (mStatsPath.empty()) {
        return;
    }
    std::error_code Ec;
    if (!mStatsPath.parent_path().empty()) {
        std::filesystem::create_directories(mStatsPath.parent_path(), Ec);
    }

    // Write a sibling file and rename it, so a crash or a full disk cannot leave
    // a half-written LAST_ONLINE behind (the cleanup would read it as inactive).
    auto TempPath = mStatsPath;
    TempPath += ".tmp";
    {
        std::ofstream File(TempPath, std::ios::trunc);
        if (!File) {
            beammp_errorf("[Raspi Host] ERROR - could not write stats file {}", TempPath.string());
            return;
        }
        File << SerializeStats(mStats, mTier, NowSeconds());
        File.flush();
        if (!File.good()) {
            beammp_errorf("[Raspi Host] ERROR - failed while writing stats file {}", TempPath.string());
            return;
        }
    }
    std::filesystem::remove(mStatsPath, Ec); // rename does not overwrite on Windows
    std::filesystem::rename(TempPath, mStatsPath, Ec);
    if (Ec) {
        beammp_errorf("[Raspi Host] ERROR - could not replace stats file {}: {}", mStatsPath.string(), Ec.message());
    }
}

bool TRaspiHostManager::Tick(size_t PlayersOnline) {
    if (!mInitialized) {
        return false;
    }
    const auto Now = std::chrono::steady_clock::now();
    if (!mTickStarted) {
        // Count from the first tick of the main loop, not from Initialize():
        // loading mods can take minutes and must not count as idle time.
        mLastTick = Now;
        mTickStarted = true;
        return false;
    }
    auto Elapsed = std::chrono::duration_cast<std::chrono::seconds>(Now - mLastTick).count();
    if (Elapsed < 1) {
        return false;
    }
    if (Elapsed > MaxTickJump) {
        // A stalled main loop is not the server sitting empty for hours.
        Elapsed = MaxTickJump;
        mLastTick = Now;
    } else {
        mLastTick += std::chrono::seconds(Elapsed);
    }
    return TickWithElapsed(PlayersOnline, static_cast<int>(Elapsed));
}

bool TRaspiHostManager::TickWithElapsed(size_t PlayersOnline, int ElapsedSeconds) {
    if (!mInitialized || ElapsedSeconds <= 0) {
        return false;
    }

    mStats.LastOnline = NowSeconds();

    if (mStats.Enabled) {
        mTicksSinceSave += ElapsedSeconds;
        while (mTicksSinceSave >= 60) {
            mTicksSinceSave -= 60;
            ++mStats.TotalMinsOnline;
        }
        if (mTicksSinceSave < ElapsedSeconds) {
            SaveStats(); // at least one full minute passed
        }
    }

    if (mShutdownSeconds <= 0 || PlayersOnline > 0) {
        mIdleSeconds = 0; // performance plan, or somebody is playing
        return false;
    }

    mIdleSeconds = (std::min)(mShutdownSeconds, mIdleSeconds + ElapsedSeconds);
    const int Remaining = mShutdownSeconds - mIdleSeconds;
    if (Remaining > 0 && (mIdleSeconds % 120 == 0 || (Remaining <= 60 && mIdleSeconds % 10 == 0))) {
        beammp_infof("[Raspi Host] Server empty, shutdown in {}s", Remaining);
    }
    if (mIdleSeconds < mShutdownSeconds) {
        return false;
    }


    beammp_infof("[Raspi Host] Time limit reached ({} min empty). Shutting down...", mShutdownSeconds / 60);
    SaveStats();
    return true;
}

TEST_CASE("TRaspiHostManager::ShutdownSecondsFor") {
    CHECK(TRaspiHostManager::ShutdownSecondsFor(TRaspiHostManager::ETier::Free) == 900);
    CHECK(TRaspiHostManager::ShutdownSecondsFor(TRaspiHostManager::ETier::Eco) == 3600);
    // performance never shuts down on inactivity
    CHECK(TRaspiHostManager::ShutdownSecondsFor(TRaspiHostManager::ETier::Performance) == 0);
}

TEST_CASE("TRaspiHostManager::inactivity shutdown per tier") {
    const auto Dir = std::filesystem::temp_directory_path() / "rh-host-manager-test";
    std::error_code Ec;
    std::filesystem::remove_all(Dir, Ec);
    std::filesystem::create_directories(Dir, Ec);

    // Each tier gets its own directory: the stats file is cumulative across
    // restarts on purpose, so sharing one would mix the three counters.
    {
        TRaspiHostManager Free;
        Free.Initialize(TRaspiHostManager::ETier::Free, Dir / "free", "");

        // 14 minutes 59 seconds empty is not enough...
        CHECK_FALSE(Free.TickWithElapsed(0, 899));
        CHECK(Free.IdleSeconds() == 899);
        // ...a single player resets the countdown entirely, even that late.
        CHECK_FALSE(Free.TickWithElapsed(1, 1));
        CHECK(Free.IdleSeconds() == 0);
        // ...and the 900th empty second requests the shutdown.
        CHECK_FALSE(Free.TickWithElapsed(0, 899));
        CHECK(Free.TickWithElapsed(0, 1));
    }

    {
        TRaspiHostManager Eco;
        Eco.Initialize(TRaspiHostManager::ETier::Eco, Dir / "eco", "");
        // an hour is fine for eco, and the stats accrue while it waits
        CHECK_FALSE(Eco.TickWithElapsed(0, 3599));
        CHECK(Eco.IdleSeconds() == 3599);
        CHECK(Eco.Stats().TotalMinsOnline == 59);
        CHECK(Eco.TickWithElapsed(0, 1));
    }

    {
        TRaspiHostManager Performance;
        Performance.Initialize(TRaspiHostManager::ETier::Performance, Dir / "performance", "");
        CHECK_FALSE(Performance.TickWithElapsed(0, 100000));
        CHECK(Performance.IdleSeconds() == 0);
        CHECK(Performance.TickWithElapsed(1, 100000) == false);
    }

    std::filesystem::remove_all(Dir, Ec);
}

TEST_CASE("TRaspiHostManager::stats accrue while running") {
    const auto Dir = std::filesystem::temp_directory_path() / "rh-host-manager-stats";
    std::error_code Ec;
    std::filesystem::remove_all(Dir, Ec);
    std::filesystem::create_directories(Dir, Ec);

    TRaspiHostManager Manager;
    Manager.Initialize(TRaspiHostManager::ETier::Free, Dir, "");
    CHECK(Manager.Stats().TimesStarted == 1);
    CHECK(Manager.Stats().TotalMinsOnline == 0);

    Manager.TickWithElapsed(4, 60);
    CHECK(Manager.Stats().TotalMinsOnline == 1);
    Manager.TickWithElapsed(4, 120);
    CHECK(Manager.Stats().TotalMinsOnline == 3);

    // the file the panel reads carries the running totals and the tier
    std::ifstream File(Manager.StatsPath());
    std::ostringstream Content;
    Content << File.rdbuf();
    CHECK(Content.str().find("TOTAL_MINS_ONLINE=3") != std::string::npos);
    CHECK(Content.str().find("TIER=free") != std::string::npos);

    std::filesystem::remove_all(Dir, Ec);
}

TEST_CASE("TRaspiHostManager::WantsUpsell") {
    CHECK(TRaspiHostManager::WantsUpsell(TRaspiHostManager::ETier::Free));
    CHECK_FALSE(TRaspiHostManager::WantsUpsell(TRaspiHostManager::ETier::Eco));
    CHECK_FALSE(TRaspiHostManager::WantsUpsell(TRaspiHostManager::ETier::Performance));
}

TEST_CASE("TRaspiHostManager::FormatTimestamp") {
    const auto FormatTimestamp = [](int64_t Timestamp, int64_t Now) {
        return TRaspiHostManager::FormatTimestamp(Timestamp, Now);
    };
    const int64_t Now = 1'700'000'000;

    CHECK(FormatTimestamp(0, Now) == "Never");
    CHECK(FormatTimestamp(Now, Now) == "Just now");
    CHECK(FormatTimestamp(Now - 59, Now) == "Just now");
    CHECK(FormatTimestamp(Now - 60, Now) == "1 minute(s) ago");
    CHECK(FormatTimestamp(Now - 3 * 3600, Now) == "3 hour(s) ago");
    CHECK(FormatTimestamp(Now - 2 * 86400, Now) == "2 day(s) ago");
    // a clock that went backwards must not produce a negative age
    CHECK(FormatTimestamp(Now + 500, Now) == "Just now");
}

TEST_CASE("TRaspiHostManager::stats file round trip") {
    using Resource = TRaspiHostManager;

    Resource::TStats Stats;
    Stats.TimesStarted = 42;
    Stats.TotalMinsOnline = 1337;
    Stats.LastOnline = 1'700'000'000;
    Stats.FirstOnline = 1'600'000'000;

    const auto Text = Resource::SerializeStats(Stats, Resource::ETier::Eco, 1'700'000'123);
    CHECK(Text.find("TIER=eco") != std::string::npos);
    CHECK(Text.find("LAST_UPDATED=1700000123") != std::string::npos);
    CHECK(Text.find("DO NOT DELETE") != std::string::npos);

    const auto Back = Resource::ParseStats(Text);
    CHECK(Back.Enabled);
    CHECK(Back.TimesStarted == 42);
    CHECK(Back.TotalMinsOnline == 1337);
    CHECK(Back.LastOnline == 1'700'000'000);
    CHECK(Back.FirstOnline == 1'600'000'000);

    // the tiers are named the way the panel expects
    CHECK(Resource::SerializeStats(Stats, Resource::ETier::Free, 0).find("TIER=free") != std::string::npos);
    CHECK(Resource::SerializeStats(Stats, Resource::ETier::Performance, 0).find("TIER=performance") != std::string::npos);
}

TEST_CASE("TRaspiHostManager::stats file tolerates old and unknown content") {
    using Resource = TRaspiHostManager;

    // A file written by the old Lua plugin: no TIER, Windows line endings.
    const auto Old = Resource::ParseStats("STATS_ENABLED=true\r\n\n-- Server Statistics --\r\n"
                                "TIMES_STARTED=7\r\nTOTAL_MINS_ONLINE=90\r\nLAST_ONLINE=1700000000\r\nFIRST_ONLINE=1600000000\r\n"
                                "LAST_UPDATED=1700000000\r\n");
    CHECK(Old.Enabled);
    CHECK(Old.TimesStarted == 7);
    CHECK(Old.TotalMinsOnline == 90);
    CHECK(Old.LastOnline == 1'700'000'000);
    CHECK(Old.FirstOnline == 1'600'000'000);

    // Tracking turned off by the server owner.
    CHECK_FALSE(Resource::ParseStats("STATS_ENABLED=false\n").Enabled);

    // Garbage values and unknown keys must not break the load.
    const auto Weird = Resource::ParseStats("TIMES_STARTED=not-a-number\nSOMETHING_NEW=1\nLAST_ONLINE=-5\n");
    CHECK(Weird.TimesStarted == 0);
    CHECK(Weird.LastOnline == -5);

    // An empty file is a valid file (first ever start).
    const auto Empty = Resource::ParseStats("");
    CHECK(Empty.Enabled);
    CHECK(Empty.TimesStarted == 0);
    CHECK(Empty.LastOnline == 0);
}
