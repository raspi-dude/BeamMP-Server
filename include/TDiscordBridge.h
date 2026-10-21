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

#include <memory>
#include <string>

class TNetwork;

/// Client for the central Raspi Host Discord relay.
///
/// The game server never talks to Discord directly and never holds a bot
/// token: it only knows a pairing code for one Discord channel and speaks a
/// tiny HTTP protocol to the relay. This keeps every customer on one shared
/// bot connection instead of one Gateway session (and one token) per server.
///
/// Two threads are used on purpose. The relay answers Discord -> game with a
/// long-poll that can block for the whole poll window, so sending and polling
/// must not share a thread: otherwise a chat line waits for the poll to return
/// before it is sent, which is what made game -> Discord feel delayed.
///
/// Memory: when disabled, all of the runtime state (threads, mutex, queue,
/// condition variable) is absent because `mState` stays null. The only cost is
/// the few bytes of config strings. `OnChat` then costs one null check.
class TDiscordBridge {
public:
    TDiscordBridge() = default;

    /// Stores settings. Never starts a thread and never allocates the runtime
    /// state: call Start() once the network is ready.
    void Configure(const Moderation::TRaspiConfig::TDiscordSettings& Settings);

    /// Starts the worker threads if (and only if) the bridge is enabled and
    /// has a relay URL plus a pairing code.
    void Start(TNetwork& Network);

    /// Stops the threads and releases all runtime state. Idempotent.
    void Shutdown();

    [[nodiscard]] bool Enabled() const { return mState != nullptr; }

    /// Called from the chat path for every broadcast player message. Must stay
    /// cheap when the bridge is off.
    void OnChat(const std::string& PlayerName, const std::string& Message);

    /// Events forwarded to Discord when [Discord] RelayEvents is on. All of
    /// them are no-ops while the bridge is disabled.
    void AnnounceServerStarted();
    void AnnouncePlayerJoin(const std::string& PlayerName);
    void AnnouncePlayerLeave(const std::string& PlayerName);

    /// Human readable status, used by the console `discord` command.
    [[nodiscard]] std::string Status() const;

    struct TState;

private:
    void SenderWorker();
    void PollWorker();

    /// Queues a player chat line (`Name`: text).
    void EnqueueChat(const std::string& PlayerName, const std::string& Message);
    /// Queues a relay-written line that has no player as its author.
    void EnqueueSystem(std::string Text);
    void Enqueue(std::string Name, std::string Text, bool System);

    Moderation::TRaspiConfig::TDiscordSettings mSettings;
    std::unique_ptr<TState> mState; ///< null whenever the bridge is disabled
    bool mConfigured { false };
};
