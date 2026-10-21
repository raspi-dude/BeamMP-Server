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
constexpr size_t WarnAtFailures = 1;
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

std::string Endpoint(const std::string& BaseUrl, const std::string& Path) {
    std::string Url = BaseUrl;
    while (!Url.empty() && Url.back() == '/') {
        Url.pop_back();
    }
    return Url + Path;
}

} // namespace

TDiscordBridge& Application::Discord() {
    static TDiscordBridge Instance;
    return Instance;
}

/// All runtime state lives here so that a disabled bridge allocates nothing.
struct TDiscordBridge::TState {
    std::thread Thread;
    mutable std::mutex Mutex;
    std::condition_variable CV;
    std::deque<std::pair<std::string, std::string>> Outgoing;
    std::atomic<bool> Stop { false };
    TNetwork* Network { nullptr };
    uint64_t Cursor { 0 };
    uint64_t Sent { 0 };
    uint64_t Received { 0 };
    uint64_t Dropped { 0 };
    int ConsecutiveFailures { 0 };
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
    mState->Thread = std::thread(&TDiscordBridge::Worker, this);
    beammp_info("Discord bridge started");
}

void TDiscordBridge::Shutdown() {
    if (!mState) {
        return;
    }
    mState->Stop.store(true, std::memory_order_release);
    mState->CV.notify_all();
    if (mState->Thread.joinable()) {
        mState->Thread.join();
    }
    mState.reset();
    beammp_debug("Discord bridge stopped");
}

void TDiscordBridge::OnChat(const std::string& PlayerName, const std::string& Message) {
    // Fast path: a single pointer check when the bridge is off. No allocation,
    // no locking, no formatting.
    if (!mState) {
        return;
    }
    auto& State = *mState;
    const std::string CleanMessage = SanitizeLine(Message);
    if (CleanMessage.empty()) {
        return;
    }
    std::string CleanName = SanitizeLine(PlayerName);
    if (CleanName.empty()) {
        CleanName = "Player";
    }

    const size_t Capacity = mSettings.MaxQueue > 0 ? static_cast<size_t>(mSettings.MaxQueue) : size_t { 64 };
    {
        std::lock_guard Lock(State.Mutex);
        if (State.Outgoing.size() >= Capacity) {
            State.Outgoing.pop_front();
            ++State.Dropped;
        }
        State.Outgoing.emplace_back(std::move(CleanName), CleanMessage);
    }
    State.CV.notify_one();
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
    return fmt::format("running: {} sent, {} received, {} dropped, {} queued (cursor {})",
        mState->Sent, mState->Received, mState->Dropped, mState->Outgoing.size(), mState->Cursor);
}

void TDiscordBridge::Worker() {
    RegisterThread("DiscordBridge");
    auto& State = *mState;

    const std::string SendUrl = Endpoint(mSettings.RelayUrl, "/v1/relay/send");
    const std::string PollUrl = Endpoint(mSettings.RelayUrl, "/v1/relay/poll");
    const int PollWait = std::clamp(mSettings.PollWaitSeconds, 1, MaxPollWaitSeconds);

    while (!State.Stop.load(std::memory_order_acquire)) {
        bool DidSomething = false;
        bool Failed = false;
        unsigned int LastStatus = 0;

        // 1) Flush one queued game message to Discord.
        std::pair<std::string, std::string> Item;
        bool HaveItem = false;
        {
            std::lock_guard Lock(State.Mutex);
            if (!State.Outgoing.empty()) {
                Item = std::move(State.Outgoing.front());
                State.Outgoing.pop_front();
                HaveItem = true;
            }
        }
        if (HaveItem) {
            const nlohmann::json Body {
                { "code", mSettings.PairingCode },
                { "name", Item.first },
                { "message", Item.second },
            };
            unsigned int HttpStatus = 0;
            const std::string Response = Http::POST(SendUrl, Body.dump(), "application/json", &HttpStatus);
            beammp_ignore(Response);
            LastStatus = HttpStatus;
            if (HttpStatus == 200) {
                std::lock_guard Lock(State.Mutex);
                ++State.Sent;
                DidSomething = true;
            } else {
                // Keep the message rather than losing it, but never grow forever.
                Failed = true;
                std::lock_guard Lock(State.Mutex);
                const size_t Capacity = mSettings.MaxQueue > 0 ? static_cast<size_t>(mSettings.MaxQueue) : size_t { 64 };
                if (State.Outgoing.size() < Capacity) {
                    State.Outgoing.push_front(std::move(Item));
                } else {
                    ++State.Dropped;
                }
            }
        }

        // 2) Long-poll for Discord -> game messages, but only when nothing is
        // waiting to be sent: a 20s poll would otherwise delay outgoing chat.
        bool QueueEmpty = true;
        {
            std::lock_guard Lock(State.Mutex);
            QueueEmpty = State.Outgoing.empty();
        }
        if (mSettings.RelayDiscordChat && QueueEmpty && !State.Stop.load(std::memory_order_acquire)) {
            const nlohmann::json Body {
                { "code", mSettings.PairingCode },
                { "cursor", State.Cursor },
                { "wait", PollWait },
            };
            unsigned int HttpStatus = 0;
            const std::string Response = Http::POST(PollUrl, Body.dump(), "application/json", &HttpStatus);
            LastStatus = HttpStatus;
            if (HttpStatus == 200 && !Response.empty() && Response != Http::ErrorString) {
                auto Doc = nlohmann::json::parse(Response, nullptr, false);
                if (!Doc.is_discarded() && Doc.is_object()) {
                    State.Cursor = Doc.value("cursor", State.Cursor);
                    if (Doc.contains("messages") && Doc["messages"].is_array()) {
                        for (const auto& Element : Doc["messages"]) {
                            if (!Element.is_object()) {
                                continue;
                            }
                            const std::string Name = Element.value("name", std::string("Discord"));
                            const std::string Text = SanitizeLine(Element.value("message", std::string()));
                            if (Text.empty()) {
                                continue;
                            }
                            // "Discord" is the sender so a Discord user cannot
                            // impersonate an in-game player.
                            const std::string Packet = "C:Discord: " + Name + ": " + Text;
                            State.Network->SendToAll(nullptr, StringToVector(Packet), true, true);
                            std::lock_guard Lock(State.Mutex);
                            ++State.Received;
                        }
                    }
                    DidSomething = true;
                } else {
                    Failed = true;
                }
            } else {
                // Covers HTTP errors and curl failures (status 0), so a dead
                // relay backs off instead of being polled every second.
                Failed = true;
            }
        }

        int BackoffMs = 0;
        if (Failed) {
            std::lock_guard Lock(State.Mutex);
            ++State.ConsecutiveFailures;
            const int Failures = State.ConsecutiveFailures;
            // Parenthesised to dodge the Windows min/max macros.
            const int Shift = (std::min)(5, Failures - 1);
            BackoffMs = (std::min)(MaxBackoffMs, 1000 << Shift);
            if (Failures == WarnAtFailures || Failures % 10 == 0) {
                beammp_warnf("Discord relay unreachable ({} consecutive failure(s), last HTTP status {}), retrying in {}ms",
                    Failures, LastStatus, BackoffMs);
            }
        } else {
            std::lock_guard Lock(State.Mutex);
            if (State.ConsecutiveFailures > 0) {
                beammp_info("Discord relay reachable again");
                State.ConsecutiveFailures = 0;
            }
        }

        if (!DidSomething) {
            std::unique_lock Lock(State.Mutex);
            const auto WaitFor = std::chrono::milliseconds(BackoffMs > 0 ? BackoffMs : 1000);
            State.CV.wait_for(Lock, WaitFor, [&State] { return State.Stop.load(std::memory_order_acquire); });
        }
    }
}
