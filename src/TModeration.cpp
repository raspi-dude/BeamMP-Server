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

#include "TModeration.h"

#include "Client.h"
#include "Common.h"
#include "TServer.h"
#include "TNetwork.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <unordered_set>

namespace {

using TSteadyClock = std::chrono::steady_clock;

std::string JoinArgs(const std::vector<std::string>& Args, size_t From) {
    std::string Out;
    for (size_t i = From; i < Args.size(); ++i) {
        if (!Out.empty()) {
            Out.push_back(' ');
        }
        Out += Args[i];
    }
    return Out;
}

std::vector<std::string> SplitArgs(const std::string& Input) {
    std::vector<std::string> Args;
    std::istringstream Stream(Input);
    std::string Token;
    while (Stream >> Token) {
        Args.push_back(Token);
    }
    return Args;
}

std::string IpOf(const TClient& Client) {
    const auto& Identifiers = Client.GetIdentifiers();
    const auto It = Identifiers.find("ip");
    return It == Identifiers.end() ? std::string { } : It->second;
}

std::string IdentityOf(const TClient& Client) {
    const auto& Identifiers = Client.GetIdentifiers();
    if (const auto It = Identifiers.find("beammp"); It != Identifiers.end() && !It->second.empty()) {
        return It->second;
    }
    return Client.GetName();
}

// Exact case-insensitive match wins, otherwise a unique prefix match is used.
std::shared_ptr<TClient> FindClientByName(TServer& Server, const std::string& Name) {
    const std::string LowerName = Moderation::ToLower(Name);
    std::shared_ptr<TClient> Exact;
    std::shared_ptr<TClient> Prefix;
    Server.ForEachClient([&](const std::weak_ptr<TClient>& ClientPtr) -> bool {
        auto Client = ClientPtr.lock();
        if (!Client || Client->IsDisconnected()) {
            return true;
        }
        const std::string LowerClient = Moderation::ToLower(Client->GetName());
        if (LowerClient == LowerName) {
            if (!Exact) {
                Exact = Client;
            }
        } else if (!Prefix && LowerClient.rfind(LowerName, 0) == 0) {
            Prefix = Client;
        }
        return true;
    });
    return Exact ? Exact : Prefix;
}

void SendChatTo(TNetwork& Network, TClient& Client, const std::string& Message) {
    beammp_ignore(Network.Respond(Client, StringToVector("C:Server: " + Message), true));
}

bool IsKnownCommand(const std::string& Sub) {
    static const std::vector<std::string> Commands { "ban", "ipban", "unban", "banlist", "mute", "unmute", "kick", "help" };
    return std::find(Commands.begin(), Commands.end(), Sub) != Commands.end();
}

} // namespace

TModeration& Application::Moderation() {
    static TModeration Instance;
    return Instance;
}

bool TModerationBan::Expired(std::chrono::system_clock::time_point Now) const {
    return !Permanent && Expires <= Now;
}

std::string TModerationBan::KickReason(std::chrono::system_clock::time_point Now) const {
    if (Permanent) {
        return fmt::format("You are banned from this server. Reason: {}", Reason.empty() ? "unspecified" : Reason);
    }
    const auto Remaining = std::chrono::duration_cast<std::chrono::seconds>(Expires - Now);
    return fmt::format("You are banned from this server. Reason: {} ({} remaining)",
        Reason.empty() ? "unspecified" : Reason, Moderation::FormatDuration(Remaining));
}

void TModeration::Initialize(const std::filesystem::path& ConfigPath) {
    Application::SetSubsystemStatus("Moderation", Application::Status::Starting);
    mConfigPath = ConfigPath;
    mConfig = Moderation::TRaspiConfig::Load(ConfigPath);
    LoadBans();
    mInitialized = true;

    if (!mConfig.Enabled) {
        beammp_info("Built-in moderation is disabled in '" + ConfigPath.string() + "'");
    } else {
        beammp_infof("Built-in moderation loaded: {} operator(s), {} filter word(s), AFK enforcement {}",
            mConfig.Operators.size(), mConfig.ChatFilter.Words.size(), mConfig.Afk.Enabled ? "on" : "off");
        if (mConfig.Operators.empty() && !mConfig.ChatFilter.Enabled && !mConfig.Afk.Enabled) {
            beammp_info("Moderation has no operators, no chat filter and no AFK enforcement configured");
        }
    }
    // The rules command is independent of [Moderation], so it is reported
    // outside that branch.
    if (!mConfig.Rules.Lines.empty() && !mConfig.Rules.Command.empty()) {
        beammp_infof("Rules command '{}' will show {} line(s) to anyone who asks", mConfig.Rules.Command, mConfig.Rules.Lines.size());
    } else {
        beammp_debug("Rules command not configured (set [Rules] Command and Lines to enable it)");
    }
    Application::SetSubsystemStatus("Moderation", Application::Status::Good);
}

bool TModeration::Enabled() const {
    std::lock_guard Lock(mMutex);
    return mInitialized && mConfig.Enabled;
}

bool TModeration::IsOperator(const std::string& Name, const std::string& IP) const {
    std::lock_guard Lock(mMutex);
    if (!mConfig.Enabled) {
        return false;
    }
    return std::any_of(mConfig.Operators.begin(), mConfig.Operators.end(),
        [&Name, &IP](const Moderation::TRaspiConfig::TOperator& Operator) {
            return Moderation::MatchesOperator(Name, IP, Operator.Name, Operator.IP);
        });
}

std::optional<std::string> TModeration::IpBanReason(const std::string& IP) const {
    const std::string TrimmedIp = std::string(Moderation::Trim(IP));
    if (TrimmedIp.empty()) {
        return std::nullopt;
    }
    std::lock_guard Lock(mMutex);
    if (!mConfig.Enabled || !mConfig.Bans.Enabled) {
        return std::nullopt;
    }
    const auto Now = std::chrono::system_clock::now();
    for (const auto& Ban : mBans) {
        if (Ban.Expired(Now) || Ban.IP.empty()) {
            continue;
        }
        if (std::string(Moderation::Trim(Ban.IP)) == TrimmedIp) {
            return Ban.KickReason(Now);
        }
    }
    return std::nullopt;
}

std::optional<std::string> TModeration::IdentityBanReason(
    const std::string& Name, const std::unordered_map<std::string, std::string>& Identifiers) const {
    std::string Identity;
    if (const auto It = Identifiers.find("beammp"); It != Identifiers.end()) {
        Identity = It->second;
    }
    const std::string LowerName = Moderation::ToLower(Name);

    std::lock_guard Lock(mMutex);
    if (!mConfig.Enabled || !mConfig.Bans.Enabled) {
        return std::nullopt;
    }
    const auto Now = std::chrono::system_clock::now();
    for (const auto& Ban : mBans) {
        if (Ban.Expired(Now)) {
            continue;
        }
        if (!Ban.Identity.empty() && !Identity.empty() && Ban.Identity == Identity) {
            return Ban.KickReason(Now);
        }
        if (!Ban.Name.empty() && Moderation::ToLower(Ban.Name) == LowerName) {
            return Ban.KickReason(Now);
        }
    }
    return std::nullopt;
}

TModerationFilterResult TModeration::FilterMessage(const std::string& Message) const {
    TModerationFilterResult Result;
    Result.Text = Message;

    std::lock_guard Lock(mMutex);
    if (!mConfig.Enabled || !mConfig.ChatFilter.Enabled || mConfig.ChatFilter.Words.empty()) {
        return Result;
    }
    const std::string Matched = Moderation::FindFilteredWord(Message, mConfig.ChatFilter.Words);
    if (Matched.empty()) {
        return Result;
    }
    Result.MatchedWord = Matched;

    const std::string& Action = mConfig.ChatFilter.Action;
    if (Action == "block") {
        Result.Action = EModerationFilterAction::Block;
    } else if (Action == "kick") {
        Result.Action = EModerationFilterAction::Kick;
    } else if (Action == "mute") {
        Result.Action = EModerationFilterAction::Mute;
    } else {
        Result.Action = EModerationFilterAction::Censor;
        Result.Text = Moderation::CensorText(Message, mConfig.ChatFilter.Words, mConfig.ChatFilter.CensorChar);
    }
    return Result;
}

std::optional<std::string> TModeration::MuteRemaining(int ClientID) const {
    std::lock_guard Lock(mMutex);
    const auto It = mMutes.find(ClientID);
    if (It == mMutes.end()) {
        return std::nullopt;
    }
    const auto Now = TSteadyClock::now();
    if (It->second.Expires <= Now) {
        return std::nullopt;
    }
    return Moderation::FormatDuration(std::chrono::duration_cast<std::chrono::seconds>(It->second.Expires - Now));
}

void TModeration::ApplyMute(int ClientID, const std::string& Name, std::chrono::seconds Duration, const std::string& Reason) {
    std::lock_guard Lock(mMutex);
    auto& Mute = mMutes[ClientID];
    Mute.Name = Name;
    Mute.Reason = Reason;
    Mute.Expires = Duration.count() <= 0 ? TSteadyClock::time_point::max() : TSteadyClock::now() + Duration;
}

void TModeration::NoteChat(int ClientID) {
    std::lock_guard Lock(mMutex);
    auto& State = mActivity[ClientID];
    State.LastActivity = TSteadyClock::now();
    State.Warned = false;
}

void TModeration::NotePosition(int ClientID, const std::string& Payload) {
    std::lock_guard Lock(mMutex);
    auto& State = mActivity[ClientID];
    const auto Now = TSteadyClock::now();
    if (State.LastActivity == TSteadyClock::time_point { }) {
        State.LastActivity = Now;
    }
    if (State.LastPosition != Payload) {
        State.LastPosition = Payload;
        State.LastActivity = Now;
        State.Warned = false;
    }
}

void TModeration::TickAfk(TServer& Server, TNetwork& Network) {
    Moderation::TRaspiConfig::TAfkSettings Afk;
    {
        std::lock_guard Lock(mMutex);
        if (!mInitialized || !mConfig.Enabled || !mConfig.Afk.Enabled) {
            return;
        }
        Afk = mConfig.Afk;
    }

    struct TSnapshot {
        std::shared_ptr<TClient> Client;
        std::string Name;
        std::string IP;
    };
    std::vector<TSnapshot> Clients;
    Server.ForEachClient([&Clients](const std::weak_ptr<TClient>& ClientPtr) -> bool {
        auto Client = ClientPtr.lock();
        if (!Client || Client->IsDisconnected()) {
            return true;
        }
        Clients.push_back({ Client, Client->GetName(), IpOf(*Client) });
        return true;
    });
    if (Clients.empty()) {
        std::lock_guard Lock(mMutex);
        mActivity.clear();
        mMutes.clear();
        return;
    }

    std::vector<std::shared_ptr<TClient>> ToWarn;
    std::vector<std::shared_ptr<TClient>> ToKick;
    const std::string WarnText = fmt::format(
        "You are AFK. Move your vehicle or send a chat message within {} or you will be kicked.",
        Moderation::FormatDuration(std::chrono::seconds(Afk.WarnGraceSeconds)));

    {
        std::lock_guard Lock(mMutex);
        const auto Now = TSteadyClock::now();
        const auto Timeout = std::chrono::minutes(Afk.TimeoutMinutes);
        const auto Grace = std::chrono::seconds(Afk.WarnGraceSeconds);

        std::unordered_set<int> LiveIDs;
        for (const auto& Snapshot : Clients) {
            LiveIDs.insert(Snapshot.Client->GetID());
        }
        for (auto It = mMutes.begin(); It != mMutes.end();) {
            if (!LiveIDs.contains(It->first) || It->second.Expires <= Now) {
                It = mMutes.erase(It);
            } else {
                ++It;
            }
        }
        for (auto It = mActivity.begin(); It != mActivity.end();) {
            It = LiveIDs.contains(It->first) ? std::next(It) : mActivity.erase(It);
        }

        for (const auto& Snapshot : Clients) {
            if (Afk.IgnoreOperators
                && std::any_of(mConfig.Operators.begin(), mConfig.Operators.end(),
                    [&Snapshot](const Moderation::TRaspiConfig::TOperator& Operator) {
                        return Moderation::MatchesOperator(Snapshot.Name, Snapshot.IP, Operator.Name, Operator.IP);
                    })) {
                continue;
            }
            auto& State = mActivity[Snapshot.Client->GetID()];
            if (State.LastActivity == TSteadyClock::time_point { }) {
                State.LastActivity = Now;
            }
            const auto Idle = Now - State.LastActivity;
            if (Idle >= Timeout + Grace) {
                ToKick.push_back(Snapshot.Client);
            } else if (Idle >= Timeout && !State.Warned) {
                State.Warned = true;
                ToWarn.push_back(Snapshot.Client);
            }
        }
    }

    for (const auto& Client : ToWarn) {
        beammp_debugf("AFK warning sent to '{}' ({})", Client->GetName(), Client->GetID());
        SendChatTo(Network, *Client, WarnText);
    }
    for (const auto& Client : ToKick) {
        beammp_infof("Kicking AFK player '{}' ({})", Client->GetName(), Client->GetID());
        Network.ClientKick(*Client, Afk.KickReason);
    }
}

bool TModeration::HandleRulesCommand(TClient& Sender, const std::string& Message, TNetwork& Network) {
    std::string Command;
    std::vector<std::string> Lines;
    {
        std::lock_guard Lock(mMutex);
        Command = mConfig.Rules.Command;
        Lines = mConfig.Rules.Lines;
    }
    if (Lines.empty() || !Moderation::MatchesCommand(Message, Command)) {
        return false;
    }

    // Only the player who asked sees the rules, so a curious player cannot
    // spam the whole server with them.
    for (const auto& Line : Moderation::FormatRuleLines(Lines)) {
        if (Sender.IsDisconnected()) {
            break;
        }
        SendChatTo(Network, Sender, Line);
    }
    beammp_debugf("Sent {} rule line(s) to '{}' ({})", Lines.size(), Sender.GetName(), Sender.GetID());
    return true;
}

bool TModeration::HandleChatCommand(TClient& Sender, const std::string& Message, TServer& Server, TNetwork& Network) {
    std::string Prefix;
    {
        std::lock_guard Lock(mMutex);
        if (!mInitialized || !mConfig.Enabled) {
            return false;
        }
        Prefix = mConfig.CommandPrefix;
    }
    if (Prefix.empty() || Message.rfind(Prefix, 0) != 0 || Message.size() <= Prefix.size()) {
        return false;
    }

    const auto Args = SplitArgs(Message.substr(Prefix.size()));
    if (Args.empty()) {
        return false;
    }
    const std::string Sub = Moderation::ToLower(Args[0]);
    if (!IsKnownCommand(Sub)) {
        return false; // ordinary chat that happens to start with the prefix
    }

    const std::string IP = IpOf(Sender);
    if (!IsOperator(Sender.GetName(), IP)) {
        SendChatTo(Network, Sender,
            fmt::format("You are not an operator on this server. (command '{}' ignored)", Sub));
        beammp_warnf("Non-operator '{}' ({}) tried to use moderation command '{}'", Sender.GetName(), IP, Sub);
        return true;
    }

    beammp_infof("Operator '{}' ({}) used command '{}'", Sender.GetName(), IP, Sub);
    ExecuteCommand(Args, Sender.GetName(), Server, Network,
        [&Sender, &Network](const std::string& ReplyText) {
            // The sender may have been kicked or banned by their own command.
            if (Sender.IsDisconnected()) {
                return;
            }
            SendChatTo(Network, Sender, ReplyText);
        });
    return true;
}

void TModeration::ExecuteCommand(const std::vector<std::string>& Args, const std::string& Actor, TServer& Server,
    TNetwork& Network, const TReply& Reply) {
    if (Args.empty()) {
        return;
    }
    const std::string Sub = Moderation::ToLower(Args[0]);

    if (Sub == "help") {
        Reply(HelpString());
        return;
    }
    if (Sub == "banlist") {
        Reply(BanListString());
        return;
    }
    if (!IsKnownCommand(Sub)) {
        Reply(fmt::format("Unknown moderation command '{}'. Try 'help'.", Sub));
        return;
    }
    if (Sub == "unban") {
        if (Args.size() < 2) {
            Reply("Usage: unban <name|ip|identity|all>");
            return;
        }
        int Removed = 0;
        {
            std::lock_guard Lock(mMutex);
            Removed = RemoveBansLocked(Args[1]);
        }
        if (Removed > 0) {
            SaveBans();
            Reply(fmt::format("Removed {} ban(s) matching '{}'.", Removed, Args[1]));
        } else {
            Reply(fmt::format("No ban matched '{}'.", Args[1]));
        }
        return;
    }

    if (Args.size() < 2) {
        Reply(fmt::format("Usage: {} <name> [duration] [reason]", Sub));
        return;
    }

    auto Target = FindClientByName(Server, Args[1]);
    if (!Target) {
        Reply(fmt::format("No connected player matches '{}'.", Args[1]));
        return;
    }

    Moderation::TRaspiConfig::TBanSettings BanSettings;
    Moderation::TRaspiConfig::TChatFilterSettings FilterSettings;
    std::string DefaultMuteDuration;
    {
        std::lock_guard Lock(mMutex);
        BanSettings = mConfig.Bans;
        FilterSettings = mConfig.ChatFilter;
        DefaultMuteDuration = mConfig.DefaultMuteDuration;
    }

    // Args[2], if present, is the duration whenever it parses as one; the rest
    // of the arguments form the reason.
    const bool BanCommand = (Sub == "ban" || Sub == "ipban");
    const std::string DefaultDurationText = BanCommand ? BanSettings.DefaultDuration : DefaultMuteDuration;
    Moderation::TDuration Duration = Moderation::ParseDuration(DefaultDurationText);
    size_t ReasonStart = 2;
    if (Args.size() > 2) {
        const auto MaybeDuration = Moderation::ParseDuration(Args[2]);
        if (MaybeDuration.Valid()) {
            Duration = MaybeDuration;
            ReasonStart = 3;
        } else if (!Args[2].empty() && std::isdigit(static_cast<unsigned char>(Args[2].front())) != 0) {
            // Looks like a duration but isn't one: reject instead of silently
            // treating the typo as part of the reason.
            Reply(fmt::format("'{}' is not a valid duration. Use 30s, 10m, 2h, 7d, 1w or permanent.", Args[2]));
            return;
        }
    }
    std::string Reason = BanSettings.DefaultReason;
    if (!BanCommand) {
        Reason = fmt::format("{} by {}", Sub == "kick" ? "Kicked" : "Muted", Actor);
    }
    if (Args.size() > ReasonStart) {
        Reason = JoinArgs(Args, ReasonStart);
    }

    if (BanCommand) {
        if (!BanSettings.Enabled) {
            Reply("Banning is disabled in RaspiConfig.toml ([Moderation.Bans] Enabled = false).");
            return;
        }
        TModerationBan Ban;
        Ban.Name = Target->GetName();
        Ban.Identity = IdentityOf(*Target);
        if (Sub == "ipban") {
            Ban.IP = IpOf(*Target);
            if (Ban.IP.empty()) {
                Reply("This player has no known IP address; banning by identity only.");
            }
        }
        Ban.Reason = Reason;
        Ban.BannedBy = Actor;
        Ban.Permanent = Duration.IsPermanent();
        if (!Ban.Permanent) {
            Ban.Expires = std::chrono::system_clock::now() + Duration.Seconds;
        }
        {
            std::lock_guard Lock(mMutex);
            AddBanLocked(Ban);
        }
        SaveBans();

        const auto Now = std::chrono::system_clock::now();
        Network.ClientKick(*Target, Ban.KickReason(Now));
        Reply(fmt::format("Banned{} '{}' for {}. Reason: {}",
            Sub == "ipban" ? " (including IP)" : "",
            Ban.Name,
            Ban.Permanent ? "permanent" : Moderation::FormatDuration(Duration.Seconds),
            Reason));
        return;
    }

    if (Sub == "kick") {
        Network.ClientKick(*Target, Reason);
        Reply(fmt::format("Kicked '{}'. Reason: {}", Target->GetName(), Reason));
        return;
    }

    if (Sub == "mute") {
        {
            std::lock_guard Lock(mMutex);
            auto& Mute = mMutes[Target->GetID()];
            Mute.Name = Target->GetName();
            Mute.Reason = Reason;
            Mute.Expires = Duration.IsPermanent()
                ? TSteadyClock::time_point::max()
                : TSteadyClock::now() + Duration.Seconds;
        }
        Reply(fmt::format("Muted '{}' for {}. Reason: {}", Target->GetName(),
            Duration.IsPermanent() ? "permanent" : Moderation::FormatDuration(Duration.Seconds), Reason));
        return;
    }

    if (Sub == "unmute") {
        bool WasMuted = false;
        {
            std::lock_guard Lock(mMutex);
            WasMuted = mMutes.erase(Target->GetID()) > 0;
        }
        Reply(WasMuted ? fmt::format("Unmuted '{}'.", Target->GetName())
                       : fmt::format("'{}' was not muted.", Target->GetName()));
        return;
    }

    Reply(fmt::format("Unknown moderation command '{}'. Try 'help'.", Sub));
}

std::string TModeration::BanListString() const {
    std::lock_guard Lock(mMutex);
    const auto Now = std::chrono::system_clock::now();
    std::vector<const TModerationBan*> Active;
    for (const auto& Ban : mBans) {
        if (!Ban.Expired(Now)) {
            Active.push_back(&Ban);
        }
    }
    if (Active.empty()) {
        return "No active bans.";
    }
    std::ostringstream Out;
    Out << "Active bans (" << Active.size() << "):";
    for (const auto* Ban : Active) {
        Out << "\n  " << Ban->Name;
        if (!Ban->IP.empty()) {
            Out << " [" << Ban->IP << "]";
        }
        Out << " | ";
        if (Ban->Permanent) {
            Out << "permanent";
        } else {
            Out << Moderation::FormatDuration(std::chrono::duration_cast<std::chrono::seconds>(Ban->Expires - Now)) << " left";
        }
        Out << " | " << (Ban->Reason.empty() ? "no reason" : Ban->Reason);
        if (!Ban->BannedBy.empty()) {
            Out << " | by " << Ban->BannedBy;
        }
    }
    return Out.str();
}

std::string TModeration::HelpString() {
    return "Moderation commands:"
           "\n  ban <name> [duration] [reason]    ban a player by account"
           "\n  ipban <name> [duration] [reason]  ban a player and their IP"
           "\n  unban <name|ip|all>               remove matching bans"
           "\n  banlist                           list active bans"
           "\n  mute <name> [duration] [reason]   stop a player from chatting"
           "\n  unmute <name>                     allow a player to chat again"
           "\n  kick <name> [reason]              disconnect a player"
           "\n  durations: 30s, 10m, 2h, 7d, 1w, a bare number of minutes, or permanent";
}

std::filesystem::path TModeration::BanFilePath() const {
    const std::filesystem::path Configured(mConfig.Bans.File);
    if (Configured.is_absolute() || Configured.parent_path().empty()) {
        return mConfigPath.parent_path() / Configured;
    }
    return Configured;
}

void TModeration::LoadBans() {
    if (!mConfig.Bans.Persist) {
        return;
    }
    const auto Path = BanFilePath();
    if (!std::filesystem::exists(Path)) {
        return;
    }
    try {
        std::ifstream File(Path);
        nlohmann::json Doc;
        File >> Doc;
        std::lock_guard Lock(mMutex);
        mBans.clear();
        for (const auto& Entry : Doc.value("bans", nlohmann::json::array())) {
            TModerationBan Ban;
            Ban.Name = Entry.value("name", std::string { });
            Ban.Identity = Entry.value("identity", std::string { });
            Ban.IP = Entry.value("ip", std::string { });
            Ban.Reason = Entry.value("reason", std::string { });
            Ban.BannedBy = Entry.value("banned_by", std::string { });
            Ban.Permanent = Entry.value("permanent", true);
            if (!Ban.Permanent) {
                Ban.Expires = std::chrono::system_clock::time_point(std::chrono::seconds(Entry.value("expires", int64_t { 0 })));
            }
            mBans.push_back(std::move(Ban));
        }
        beammp_infof("Loaded {} ban(s) from '{}'", mBans.size(), Path.string());
    } catch (const std::exception& e) {
        beammp_error("Failed to load ban list '" + Path.string() + "': " + e.what());
    }
}

void TModeration::SaveBans() const {
    if (!mConfig.Bans.Persist) {
        return;
    }
    const auto Path = BanFilePath();
    nlohmann::json Doc;
    Doc["bans"] = nlohmann::json::array();
    {
        std::lock_guard Lock(mMutex);
        const auto Now = std::chrono::system_clock::now();
        for (const auto& Ban : mBans) {
            if (Ban.Expired(Now)) {
                continue;
            }
            nlohmann::json Entry {
                { "name", Ban.Name },
                { "identity", Ban.Identity },
                { "ip", Ban.IP },
                { "reason", Ban.Reason },
                { "banned_by", Ban.BannedBy },
                { "permanent", Ban.Permanent },
            };
            if (!Ban.Permanent) {
                Entry["expires"] = std::chrono::duration_cast<std::chrono::seconds>(Ban.Expires.time_since_epoch()).count();
            }
            Doc["bans"].push_back(std::move(Entry));
        }
    }
    try {
        std::ofstream File(Path, std::ios::trunc);
        if (!File) {
            beammp_error("Failed to open ban list for writing: '" + Path.string() + "'");
            return;
        }
        File << Doc.dump(4);
    } catch (const std::exception& e) {
        beammp_error("Failed to write ban list '" + Path.string() + "': " + e.what());
    }
}

void TModeration::PruneBansLocked(std::chrono::system_clock::time_point Now) {
    std::erase_if(mBans, [Now](const TModerationBan& Ban) { return Ban.Expired(Now); });
}

void TModeration::AddBanLocked(const TModerationBan& Ban) {
    PruneBansLocked(std::chrono::system_clock::now());
    // Replace an existing ban for the same player or IP instead of stacking them.
    std::erase_if(mBans, [&Ban](const TModerationBan& Existing) {
        if (!Ban.Identity.empty() && Existing.Identity == Ban.Identity) {
            return true;
        }
        if (!Ban.IP.empty() && Existing.IP == Ban.IP) {
            return true;
        }
        return !Ban.Name.empty() && Moderation::ToLower(Existing.Name) == Moderation::ToLower(Ban.Name);
    });
    mBans.push_back(Ban);
}

int TModeration::RemoveBansLocked(const std::string& Query) {
    const std::string LowerQuery = Moderation::ToLower(Moderation::Trim(Query));
    const bool All = LowerQuery == "all";
    const auto Before = mBans.size();
    std::erase_if(mBans, [&LowerQuery, All](const TModerationBan& Ban) {
        if (All) {
            return true;
        }
        return Moderation::ToLower(Ban.Name) == LowerQuery
            || Moderation::ToLower(Ban.Identity) == LowerQuery
            || Moderation::ToLower(Ban.IP) == LowerQuery;
    });
    return static_cast<int>(Before - mBans.size());
}
