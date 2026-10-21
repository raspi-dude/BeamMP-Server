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

#include "TDiscordBridge.h"

#include "Common.h"
#include "Http.h"
#include "TNetwork.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

// The game chat packet limit is 500 bytes including the "C:<sender>: " prefix.
constexpr size_t MaxRelayedLength = 400;
constexpr int MaxBackoffMs = 30000;
// The shared Http helpers use a 30s timeout, so the long-poll must ask for
// less than that or curl would cut every idle poll short and we would spin.
constexpr int MaxPollWaitSeconds = 20;

std::string SanitizeLine(std::string Text) {
    for (char& Character : Text) {
        if (Character == '\n' || Character == '\r' || Character == '\t') {
            Character = ' ';
        }
    }
    if (Text.size() > MaxRelayedLength) {
        Text.resize(MaxRelayedLength);
    }
    const auto Begin = Text.find_first_not_of(' ');
    if (Begin == std::string::npos) {
        return {};
    }
    const auto End = Text.find_last_not_of(' ');
    return Text.substr(Begin, End - Begin + 1);
}

/// Strips the markdown characters the relay would otherwise interpret, so a
/// player cannot fake formatting (or look like a system event) in Discord.
std::string SanitizeName(std::string Name) {
    for (char& Character : Name) {
        if (Character == '\n' || Character == '\r' || Character == '\t') {
            Character = ' ';
        }
    }
    Name.erase(std::remove_if(Name.begin(), Name.end(),
                   [](char Character) { return Character == '*' || Character == '_' || Character == '~' || Character == '`'; }),
        Name.end());
    Name = SanitizeLine(Name);
    if (Name.empty()) {
        return "Player";
    }
    if (Name.size() > 32) {
        Name.resize(32);
    }
    return Name;
}

std::string Endpoint(const std::string& BaseUrl, const std::string& Path) {
    std::string Url = BaseUrl;
    while (!Url.empty() && Url.back() == '/') {
        Url.pop_back();
    }
    return Url + Path;
}

std::string Trim(std::string Text) {
    const auto Begin = Text.find_first_not_of(" \t\r\n");
    if (Begin == std::string::npos) {
        return {};
    }
    const auto End = Text.find_last_not_of(" \t\r\n");
    return Text.substr(Begin, End - Begin + 1);
}

/// Turns a relay error response into something a server owner can act on.
std::string DescribeRelayError(unsigned int Status, const std::string& Body) {
    std::string RelayMessage;
    if (!Body.empty() && Body != Http::ErrorString) {
        const auto Doc = nlohmann::json::parse(Body, nullptr, false);
        if (!Doc.is_discarded() && Doc.is_object()) {
            RelayMessage = Doc.value("message", std::string { });
            if (RelayMessage.empty()) {
                RelayMessage = Doc.value("error", std::string { });
            }
        } else {
            RelayMessage = SanitizeLine(Body);
        }
    }
    RelayMessage = Trim(RelayMessage);

    switch (Status) {
    case 401:
        return "the relay rejected this server's pairing code (undefined or revoked); run /pair in Discord and copy the new code into RaspiConfig.toml";
    case 403:
        return "the relay is locked to specific IPs and this server is not on the list (add it to allowedIps in the relay config)"
            + (RelayMessage.empty() ? std::string { } : ": " + RelayMessage);
    case 404:
        return "the relay URL is wrong: RelayUrl must point at the relay root, not at /v1/relay/... (got 404)";
    case 429:
        return "the relay rate limit was hit, messages are being throttled (raise rateLimit in the relay config if this is legitimate traffic)"
            + (RelayMessage.empty() ? std::string { } : ": " + RelayMessage);
    case 502:
        return "the relay could not post to Discord (is the bot still in that channel and allowed to send messages?)";
    case 0:
        return "the relay could not be reached at all (check RelayUrl, DNS, firewall and that the relay process is running)";
    default:
        return fmt::format("unexpected relay response {} ({})", Status, Http::Status::ToString(static_cast<int>(Status)));
    }
}

int BackoffFor(int Failures) {
    // Parenthesised to dodge the Windows min/max macros.
    const int Shift = (std::min)(5, (std::max)(0, Failures - 1));
    return (std::min)(MaxBackoffMs, 1000 << Shift);
}

} // namespace

TDiscordBridge& Application::Discord() {
    static TDiscordBridge Instance;
    return Instance;
}

/// All runtime state lives here so that a disabled bridge allocates nothing.
struct TDiscordBridge::TState {
    struct TItem {
        std::string Name; ///< empty for system lines
        std::string Text;
        bool System { false };
    };

    std::thread Sender;
    std::thread Poller;
    mutable std::mutex Mutex;
    std::condition_variable CV;
    std::deque<TItem> Outgoing;
    std::atomic<bool> Stop { false };
    TNetwork* Network { nullptr };
    uint64_t Cursor { 0 };
    uint64_t Sent { 0 };
    uint64_t Received { 0 };
    uint64_t Dropped { 0 };
    int SendFailures { 0 };
    int PollFailures { 0 };
    std::string LastError;
};

void TDiscordBridge::Configure(const Moderation::TRaspiConfig::TDiscordSettings& Settings) {
    mSettings = Settings;
    mConfigured = true;

    if (!Settings.Enabled) {
        // Nothing is allocated here on purpose: disabled means zero footprint.
        beammp_info("Discord bridge disabled");
        return;
    }
    if (Settings.RelayUrl.empty()) {
        beammp_warn("Discord bridge is enabled but RelayUrl is empty; the bridge will not start");
        return;
    }
    if (Settings.PairingCode.empty()) {
        beammp_warn("Discord bridge is enabled but PairingCode is empty; run /pair in Discord first");
        return;
    }
    beammp_info("Discord bridge configured; it starts once the server network is up");
}

void TDiscordBridge::Start(TNetwork& Network) {
    if (mState || !mConfigured || !mSettings.Enabled) {
        return;
    }
    if (mSettings.RelayUrl.empty() || mSettings.PairingCode.empty()) {
        beammp_warn("Discord bridge not started: missing RelayUrl or PairingCode");
        return;
    }
    mState = std::make_unique<TState>();
    mState->Network = &Network;
    mState->Sender = std::thread(&TDiscordBridge::SenderWorker, this);
    if (mSettings.RelayDiscordChat) {
        mState->Poller = std::thread(&TDiscordBridge::PollWorker, this);
    } else {
        beammp_info("Discord -> game relay is off (RelayDiscordChat = false); not polling");
    }
    beammp_info("Discord bridge started");
    AnnounceServerStarted();
}

void TDiscordBridge::Shutdown() {
    if (!mState) {
        return;
    }
    auto& State = *mState;
    if (mSettings.RelayEvents) {
        EnqueueSystem("**Server stopped**");
        // Give the sender a moment to get the farewell out before stopping. It
        // is best effort: shutdown must never hang on the relay.
        std::unique_lock Lock(State.Mutex);
        State.CV.wait_for(Lock, std::chrono::seconds(3), [&State] { return State.Outgoing.empty(); });
    }
    State.Stop.store(true, std::memory_order_release);
    State.CV.notify_all();
    // The poll is aborted inside curl, so this returns in about a second
    // instead of waiting out the whole poll window.
    if (State.Sender.joinable()) {
        State.Sender.join();
    }
    if (State.Poller.joinable()) {
        State.Poller.join();
    }
    mState.reset();
    beammp_debug("Discord bridge stopped");
}

void TDiscordBridge::Enqueue(std::string Name, std::string Text, bool System) {
    if (!mState || Text.empty()) {
        return;
    }
    auto& State = *mState;
    const size_t Capacity = mSettings.MaxQueue > 0 ? static_cast<size_t>(mSettings.MaxQueue) : size_t { 64 };
    {
        std::lock_guard Lock(State.Mutex);
        if (State.Outgoing.size() >= Capacity) {
            State.Outgoing.pop_front();
            ++State.Dropped;
        }
        State.Outgoing.push_back(TState::TItem { std::move(Name), std::move(Text), System });
    }
    State.CV.notify_one();
}

void TDiscordBridge::EnqueueChat(const std::string& PlayerName, const std::string& Message) {
    const std::string CleanMessage = SanitizeLine(Message);
    if (CleanMessage.empty()) {
        return;
    }
    Enqueue(SanitizeName(PlayerName), CleanMessage, false);
}

void TDiscordBridge::EnqueueSystem(std::string Text) {
    Enqueue(std::string { }, SanitizeLine(std::move(Text)), true);
}

void TDiscordBridge::OnChat(const std::string& PlayerName, const std::string& Message) {
    // Fast path: a single pointer (and one bool) check when the bridge is off.
    if (!mState || !mSettings.RelayServerChat) {
        return;
    }
    EnqueueChat(PlayerName, Message);
}

void TDiscordBridge::AnnounceServerStarted() {
    if (!mState || !mSettings.RelayEvents) {
        return;
    }
    EnqueueSystem("**Server started**");
}

void TDiscordBridge::AnnouncePlayerJoin(const std::string& PlayerName) {
    if (!mState || !mSettings.RelayEvents) {
        return;
    }
    EnqueueSystem(fmt::format("**{}** joined the server", SanitizeName(PlayerName)));
}

void TDiscordBridge::AnnouncePlayerLeave(const std::string& PlayerName) {
    if (!mState || !mSettings.RelayEvents) {
        return;
    }
    EnqueueSystem(fmt::format("**{}** left the server", SanitizeName(PlayerName)));
}

std::string TDiscordBridge::Status() const {
    if (!mConfigured) {
        return "not configured";
    }
    if (!mSettings.Enabled) {
        return "disabled ([Discord] Enabled = false)";
    }
    if (!mState) {
        if (mSettings.RelayUrl.empty() || mSettings.PairingCode.empty()) {
            return "enabled but not running: RelayUrl or PairingCode is empty";
        }
        return "enabled but not started";
    }
    std::lock_guard Lock(mState->Mutex);
    std::string Result = fmt::format("running: {} sent, {} received, {} dropped, {} queued (cursor {})",
        mState->Sent, mState->Received, mState->Dropped, mState->Outgoing.size(), mState->Cursor);
    if (!mSettings.RelayDiscordChat) {
        Result += ", Discord -> game off";
    }
    if (!mSettings.RelayEvents) {
        Result += ", event announcements off";
    }
    if (!mState->LastError.empty()) {
        Result += "; last error: " + mState->LastError;
    }
    return Result;
}

void TDiscordBridge::SenderWorker() {
    RegisterThread("DiscordBridgeSender");
    auto& State = *mState;
    const std::string SendUrl = Endpoint(mSettings.RelayUrl, "/v1/relay/send");
    const int FreeSlots = static_cast<int>(mSettings.MaxQueue > 0 ? mSettings.MaxQueue : 64);

    while (!State.Stop.load(std::memory_order_acquire)) {
        TState::TItem Item;
        {
            std::unique_lock Lock(State.Mutex);
            State.CV.wait_for(Lock, std::chrono::seconds(1), [&State] {
                return State.Stop.load(std::memory_order_acquire) || !State.Outgoing.empty();
            });
            if (State.Outgoing.empty()) {
                continue;
            }
            Item = std::move(State.Outgoing.front());
            State.Outgoing.pop_front();
        }

        nlohmann::json Body {
            { "code", mSettings.PairingCode },
            { "message", Item.Text },
        };
        if (Item.System) {
            Body["system"] = true;
        } else {
            Body["name"] = Item.Name;
        }

        unsigned int HttpStatus = 0;
        const std::string Response = Http::POST(SendUrl, Body.dump(), "application/json", &HttpStatus, {}, &State.Stop);
        beammp_ignore(Response);

        if (HttpStatus == 200) {
            {
                std::lock_guard Lock(State.Mutex);
                ++State.Sent;
                State.SendFailures = 0;
                State.LastError.clear();
            }
            // Wakes Shutdown(), which waits for the queue to drain so the
            // "server stopped" line actually goes out.
            State.CV.notify_all();
            continue;
        }

        // Keep the message rather than losing it, but never grow forever.
        bool GiveUp = State.Stop.load(std::memory_order_acquire);
        int Failures = 0;
        {
            std::lock_guard Lock(State.Mutex);
            if (!GiveUp && static_cast<int>(State.Outgoing.size()) < FreeSlots) {
                State.Outgoing.push_front(std::move(Item));
            } else if (!GiveUp) {
                ++State.Dropped;
            }
            ++State.SendFailures;
            Failures = State.SendFailures;
            State.LastError = DescribeRelayError(HttpStatus, Response);
        }
        State.CV.notify_all();
        if (GiveUp) {
            break;
        }
        const int BackoffMs = BackoffFor(Failures);
        if (Failures == 1 || Failures % 10 == 0) {
            beammp_warnf("Discord relay: {} ({} failure(s) in a row, retrying in {}ms)", State.LastError, Failures, BackoffMs);
        }
        std::unique_lock Lock(State.Mutex);
        State.CV.wait_for(Lock, std::chrono::milliseconds(BackoffMs), [&State] {
            return State.Stop.load(std::memory_order_acquire);
        });
    }
}

void TDiscordBridge::PollWorker() {
    RegisterThread("DiscordBridgePoll");
    auto& State = *mState;
    const std::string PollUrl = Endpoint(mSettings.RelayUrl, "/v1/relay/poll");
    const int PollWait = std::clamp(mSettings.PollWaitSeconds, 1, MaxPollWaitSeconds);

    while (!State.Stop.load(std::memory_order_acquire)) {
        nlohmann::json Body {
            { "code", mSettings.PairingCode },
            { "cursor", State.Cursor },
            { "wait", PollWait },
        };
        unsigned int HttpStatus = 0;
        const std::string Response = Http::POST(PollUrl, Body.dump(), "application/json", &HttpStatus, {}, &State.Stop);

        if (State.Stop.load(std::memory_order_acquire)) {
            break; // cancelled mid-poll by shutdown
        }

        if (HttpStatus == 200 && !Response.empty() && Response != Http::ErrorString) {
            const auto Doc = nlohmann::json::parse(Response, nullptr, false);
            if (!Doc.is_discarded() && Doc.is_object()) {
                std::vector<std::pair<std::string, std::string>> Incoming;
                {
                    std::lock_guard Lock(State.Mutex);
                    State.Cursor = Doc.value("cursor", State.Cursor);
                    State.PollFailures = 0;
                    State.LastError.clear();
                }
                if (Doc.contains("messages") && Doc["messages"].is_array()) {
                    for (const auto& Element : Doc["messages"]) {
                        if (!Element.is_object()) {
                            continue;
                        }
                        const std::string Name = Element.value("name", std::string("Discord"));
                        const std::string Text = SanitizeLine(Element.value("message", std::string()));
                        if (!Text.empty()) {
                            Incoming.emplace_back(Name, Text);
                        }
                    }
                }
                for (const auto& [Name, Text] : Incoming) {
                    // "Discord" is the sender so a Discord user cannot pretend
                    // to be a player (or the server) in-game.
                    const std::string Packet = "C:Discord: " + Name + ": " + Text;
                    State.Network->SendToAll(nullptr, StringToVector(Packet), true, true);
                    // Mirror it into the server console, like normal chat.
                    beammp_infof("[Discord] {}: {}", Name, Text);
                    std::lock_guard Lock(State.Mutex);
                    ++State.Received;
                }
                continue;
            }
        }

        int Failures = 0;
        std::string Error;
        {
            std::lock_guard Lock(State.Mutex);
            ++State.PollFailures;
            Failures = State.PollFailures;
            State.LastError = DescribeRelayError(HttpStatus, Response);
            Error = State.LastError;
        }
        const int BackoffMs = BackoffFor(Failures);
        if (Failures == 1 || Failures % 10 == 0) {
            beammp_warnf("Discord relay: {} ({} failure(s) in a row, retrying in {}ms)", Error, Failures, BackoffMs);
        }
        std::unique_lock Lock(State.Mutex);
        State.CV.wait_for(Lock, std::chrono::milliseconds(BackoffMs), [&State] {
            return State.Stop.load(std::memory_order_acquire);
        });
    }
}
