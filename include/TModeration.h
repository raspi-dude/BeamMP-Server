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

#include "TRaspiConfig.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class TServer;
class TNetwork;
class TClient;

/// One entry of the persisted ban list. A ban can carry an identity (BeamMP
/// account id or name) and/or an IP address; either match rejects the player.
struct TModerationBan {
    std::string Name;
    std::string Identity;
    std::string IP;
    std::string Reason;
    std::string BannedBy;
    bool Permanent { true };
    std::chrono::system_clock::time_point Expires {};

    [[nodiscard]] bool Expired(std::chrono::system_clock::time_point Now) const;
    /// Message sent to a rejected client, including remaining time.
    [[nodiscard]] std::string KickReason(std::chrono::system_clock::time_point Now) const;
};

enum class EModerationFilterAction {
    Allow,
    Censor,
    Block,
    Kick,
    Mute
};

struct TModerationFilterResult {
    EModerationFilterAction Action { EModerationFilterAction::Allow };
    std::string Text; ///< censored message when Action == Censor
    std::string MatchedWord;
};

/// Built-in moderation: bans (identity + IP), ban durations, custom reasons,
/// chat filtering and AFK enforcement. Configured by RaspiConfig.toml.
///
/// Thread safety: all public methods are safe to call from multiple threads.
/// The class never kicks or sends by itself; callers own the networking so
/// that this logic stays testable.
class TModeration {
public:
    using TReply = std::function<void(const std::string&)>;

    /// Loads RaspiConfig.toml (creating it when missing) and the ban list.
    void Initialize(const std::filesystem::path& ConfigPath);

    [[nodiscard]] bool Enabled() const;
    [[nodiscard]] const Moderation::TRaspiConfig& Config() const { return mConfig; }
    [[nodiscard]] bool IsOperator(const std::string& Name, const std::string& IP) const;

    /// Kick reason when the IP matches a live IP ban, otherwise nullopt.
    [[nodiscard]] std::optional<std::string> IpBanReason(const std::string& IP) const;
    /// Kick reason when the player matches a live identity/name ban.
    [[nodiscard]] std::optional<std::string> IdentityBanReason(
        const std::string& Name, const std::unordered_map<std::string, std::string>& Identifiers) const;

    /// Applies the configured chat filter. Side-effect free.
    [[nodiscard]] TModerationFilterResult FilterMessage(const std::string& Message) const;

    /// Remaining mute time if the client is muted.
    [[nodiscard]] std::optional<std::string> MuteRemaining(int ClientID) const;
    /// Applies a mute directly, used by the chat filter's "mute" action.
    void ApplyMute(int ClientID, const std::string& Name, std::chrono::seconds Duration, const std::string& Reason);

    /// Activity tracking for AFK enforcement.
    void NoteChat(int ClientID);
    /// Position packets only count as activity when they actually change.
    void NotePosition(int ClientID, const std::string& Payload);
    void TickAfk(TServer& Server, TNetwork& Network);

    /// Handles an in-game command ("!ban ..."). Returns true when the message
    /// was a command, in which case it must not be broadcast.
    bool HandleChatCommand(TClient& Sender, const std::string& Message, TServer& Server, TNetwork& Network);

    /// Shared command dispatch used by the console and by in-game commands.
    /// Args[0] is the subcommand, e.g. {"ban", "name", "1h", "reason"}.
    void ExecuteCommand(const std::vector<std::string>& Args, const std::string& Actor, TServer& Server,
        TNetwork& Network, const TReply& Reply);

    [[nodiscard]] std::string BanListString() const;

    [[nodiscard]] static std::string HelpString();

private:
    struct TMute {
        std::string Name;
        std::string Reason;
        std::chrono::steady_clock::time_point Expires {};
    };

    struct TActivityState {
        std::chrono::steady_clock::time_point LastActivity {};
        std::string LastPosition;
        bool Warned { false };
    };

    [[nodiscard]] std::filesystem::path BanFilePath() const;
    void LoadBans();
    void SaveBans() const;
    void PruneBansLocked(std::chrono::system_clock::time_point Now);
    void AddBanLocked(const TModerationBan& Ban);
    int RemoveBansLocked(const std::string& Query);

    Moderation::TRaspiConfig mConfig;
    std::filesystem::path mConfigPath;
    std::vector<TModerationBan> mBans;
    std::unordered_map<int, TMute> mMutes;
    std::unordered_map<int, TActivityState> mActivity;
    mutable std::mutex mMutex;
    bool mInitialized { false };
};
