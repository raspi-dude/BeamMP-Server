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
#include <string_view>
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

/// A mute that outlives the session it was given in. The BeamMP account id is
/// what makes that work: it stays the same across reconnects and restarts,
/// while the client id is handed out per connection. The name is a fallback for
/// players whose identity is not known.
struct TModerationMute {
    std::string Name;
    std::string Identity;
    std::string Reason;
    std::string MutedBy;
    bool Permanent { true };
    std::chrono::system_clock::time_point Expires {};

    [[nodiscard]] bool Expired(std::chrono::system_clock::time_point Now) const;
    /// "12m 30s" for a timed mute, "permanent" for a permanent one.
    [[nodiscard]] std::string RemainingText(std::chrono::system_clock::time_point Now) const;
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

    /// Result of Reload(): whether the file could be read, what changed, and
    /// whether the server has to restart for [Discord].
    struct TReloadReport {
        bool Success { false };
        std::string Error; ///< set when Success is false; the live config is untouched
        std::vector<Moderation::TConfigChange> Changes;
        /// How many of Changes were applied: everything except [Discord].
        size_t AppliedCount { 0 };
        /// True when [Discord] was edited. The bridge is built at startup, so
        /// those settings are left alone.
        bool DiscordRestartRequired { false };
    };

    /// Re-reads RaspiConfig.toml and applies it to the running server, along
    /// with the ban and mute files. [Discord] is skipped, since the bridge is
    /// only built at startup; those changes need a restart.
    TReloadReport Reload();

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

    /// Remaining mute time if this player is muted, looked up by identity first
    /// and by name otherwise. Identity is what makes a mute survive a reconnect
    /// and a server restart.
    [[nodiscard]] std::optional<std::string> MuteRemaining(
        const std::string& Name, const std::unordered_map<std::string, std::string>& Identifiers) const;
    /// Applies (or replaces) a mute and writes it to disk. A duration of zero or
    /// less means permanent. Used by the "mute" command and the chat filter.
    void ApplyMute(const std::string& Name, const std::string& Identity, std::chrono::seconds Duration,
        const std::string& Reason, const std::string& MutedBy);
    /// Removes every mute matching this name or identity, and writes the result
    /// back to disk. Returns how many were removed.
    int RemoveMutes(const std::string& Name, const std::string& Identity);

    /// The BeamMP account id when the client has one, otherwise empty.
    [[nodiscard]] static std::string IdentityFromIdentifiers(
        const std::unordered_map<std::string, std::string>& Identifiers);
    [[nodiscard]] static bool MuteMatches(
        const TModerationMute& Mute, const std::string& Name, const std::string& Identity);
    /// The on-disk format of the mute list. Kept as plain functions so the
    /// format can be tested without touching a file or the singleton.
    [[nodiscard]] static std::vector<TModerationMute> ParseMutes(std::string_view Json);
    [[nodiscard]] static std::string SerializeMutes(const std::vector<TModerationMute>& Mutes);

    /// Activity tracking for AFK enforcement.
    void NoteChat(int ClientID);
    /// Position packets only count as activity when they actually change.
    void NotePosition(int ClientID, const std::string& Payload);
    void TickAfk(TServer& Server, TNetwork& Network);

    /// Handles the configured rules command ("!rules") for every player, not
    /// just operators. Returns true when the message was consumed, in which case
    /// it must not be broadcast.
    bool HandleRulesCommand(TClient& Sender, const std::string& Message, TNetwork& Network);

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
    struct TActivityState {
        std::chrono::steady_clock::time_point LastActivity {};
        std::string LastPosition;
        bool Warned { false };
    };

    [[nodiscard]] std::filesystem::path BanFilePath() const;
    /// "RaspiMutes.json" next to the ban list, so relocating the ban file moves
    /// the mutes with it.
    [[nodiscard]] std::filesystem::path MuteFilePath() const;
    void LoadBans();
    void SaveBans() const;
    void LoadMutes();
    void SaveMutes() const;
    void PruneBansLocked(std::chrono::system_clock::time_point Now);
    void PruneMutesLocked(std::chrono::system_clock::time_point Now);
    void AddBanLocked(const TModerationBan& Ban);
    void AddMuteLocked(const TModerationMute& Mute);
    int RemoveBansLocked(const std::string& Query);

    Moderation::TRaspiConfig mConfig;
    std::filesystem::path mConfigPath;
    std::vector<TModerationBan> mBans;
    std::vector<TModerationMute> mMutes;
    std::unordered_map<int, TActivityState> mActivity;
    mutable std::mutex mMutex;
    /// Serializes reloads. Only Reload() takes it, so it can be held for the
    /// whole call.
    std::mutex mReloadMutex;
    bool mInitialized { false };
};
