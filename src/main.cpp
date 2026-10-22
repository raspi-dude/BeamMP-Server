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

#include "ArgsParser.h"
#include "Client.h"
#include "Common.h"
#include "Http.h"
#include "LuaAPI.h"
#include "Settings.h"
#include "SignalHandling.h"
#include "TConfig.h"
#include "TDiscordBridge.h"
#include "THeartbeatThread.h"
#include "TModeration.h"
#include "TLuaEngine.h"
#include "TNetwork.h"
#include "TPPSMonitor.h"
#include "TPluginMonitor.h"
#include "TResourceManager.h"
#include "TRaspiHostManager.h"
#include "TServer.h"

#include <cstdint>
#include <iostream>
#include <thread>

static const std::string sCommandlineArguments = R"(
USAGE:
    BeamMP-Server [arguments]

ARGUMENTS:
    --help
                        Displays this help and exits.
    --port=1234
                        Sets the server's listening TCP and
                        UDP port. Overrides ENV and ServerConfig.
    --config=/path/to/ServerConfig.toml
                        Absolute or relative path to the
                        Server Config file, including the
                        filename. For paths and filenames with
                        spaces, put quotes around the path.
    --working-directory=/path/to/folder
                        Sets the working directory of the Server.
                        All paths are considered relative to this,
                        including the path given in --config.
    --version
                        Prints version info and exits.
    --free | --eco | --performance
                        Raspi Host plan this server runs on. Free shuts
                        an empty server down after 15 minutes, eco after
                        60 minutes, performance never. Without a flag the
                        server runs as free and says so in the log.

EXAMPLES:
    BeamMP-Server --config=../MyWestCoastServerConfig.toml
        Runs the BeamMP-Server and uses the server config file
        which is one directory above it and is named
        'MyWestCoastServerConfig.toml'.
)";

struct MainArguments {
    int argc {};
    char** argv {};
    std::vector<std::string_view> List;
    std::string InvokedAs;
};

int BeamMPServerMain(MainArguments Arguments);

int main(int argc, char** argv) {
    MainArguments Args { argc, argv, {}, argv[0] };
    Args.List.reserve(size_t(argc));
    for (int i = 1; i < argc; ++i) {
        Args.List.push_back(argv[i]);
    }
    int MainRet = 0;
    try {
        MainRet = BeamMPServerMain(std::move(Args));
    } catch (const std::exception& e) {
        beammp_error("A fatal exception has occurred and the server is forcefully shutting down.");
        beammp_error(e.what());
        MainRet = -1;
    }
    std::exit(MainRet);
}

int BeamMPServerMain(MainArguments Arguments) {
    setlocale(LC_ALL, "C");
    ArgsParser Parser;
    Parser.RegisterArgument({ "help" }, ArgsParser::NONE);
    Parser.RegisterArgument({ "version" }, ArgsParser::NONE);
    Parser.RegisterArgument({ "config" }, ArgsParser::HAS_VALUE);
    Parser.RegisterArgument({ "port" }, ArgsParser::HAS_VALUE);
    Parser.RegisterArgument({ "working-directory" }, ArgsParser::HAS_VALUE);
    Parser.RegisterArgument({ "free" }, ArgsParser::NONE);
    Parser.RegisterArgument({ "eco" }, ArgsParser::NONE);
    Parser.RegisterArgument({ "performance" }, ArgsParser::NONE);
    Parser.Parse(Arguments.List);
    if (!Parser.Verify()) {
        return 1;
    }
    if (Parser.FoundArgument({ "help" })) {
        Application::Console().WriteRaw(sCommandlineArguments);
        return 0;
    }
    if (Parser.FoundArgument({ "version" })) {
        Application::Console().WriteRaw("BeamMP-Server v" + Application::ServerVersionString());
        return 0;
    }

    // Exactly one plan flag, so an egg cannot silently run in the wrong tier.
    const bool FreeFlag = Parser.FoundArgument({ "free" });
    const bool EcoFlag = Parser.FoundArgument({ "eco" });
    const bool PerformanceFlag = Parser.FoundArgument({ "performance" });
    const int TierFlags = int(FreeFlag) + int(EcoFlag) + int(PerformanceFlag);
    if (TierFlags > 1) {
        beammp_error("Pass only one of --free, --eco or --performance.");
        return 1;
    }
    TRaspiHostManager::ETier Tier = TRaspiHostManager::ETier::Free;
    if (EcoFlag) {
        Tier = TRaspiHostManager::ETier::Eco;
    } else if (PerformanceFlag) {
        Tier = TRaspiHostManager::ETier::Performance;
    }

    std::string ConfigPath = "ServerConfig.toml";
    if (Parser.FoundArgument({ "config" })) {
        auto MaybeConfigPath = Parser.GetValueOfArgument({ "config" });
        if (MaybeConfigPath.has_value()) {
            ConfigPath = MaybeConfigPath.value();
            beammp_info("Custom config requested via commandline arguments: '" + ConfigPath + "'");
        }
    }
    if (Parser.FoundArgument({ "working-directory" })) {
        auto MaybeWorkingDirectory = Parser.GetValueOfArgument({ "working-directory" });
        if (MaybeWorkingDirectory.has_value()) {
            beammp_info("Custom working directory requested via commandline arguments: '" + MaybeWorkingDirectory.value() + "'");
            try {
                fs::current_path(fs::path(MaybeWorkingDirectory.value()));
            } catch (const std::exception& e) {
                beammp_errorf("Could not set working directory to '{}': {}", MaybeWorkingDirectory.value(), e.what());
            }
        }
    }

    TConfig Config(ConfigPath);

    if (Config.Failed()) {
        beammp_info("Closing in 10 seconds");
        // loop to make it possible to ctrl+c instead
        for (size_t i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return 1;
    }

    // override port if provided via arguments
    if (Parser.FoundArgument({ "port" })) {
        auto Port = Parser.GetValueOfArgument({ "port" });
        if (Port.has_value()) {
            auto P = int(std::strtoul(Port.value().c_str(), nullptr, 10));
            if (P == 0 || P < 0 || P > UINT16_MAX) {
                beammp_errorf("Custom port requested via --port is invalid: '{}'", Port.value());
                return 1;
            } else {
                Application::Settings.set(Settings::Key::General_Port, P);
                beammp_info("Custom port requested via commandline arguments: " + Port.value());
            }
        }
    }

    Config.PrintDebug();

    // Built-in moderation lives in its own config file next to ServerConfig.toml,
    // so it gets generated and loaded before any client can connect.
    Application::Moderation().Initialize(fs::path(ConfigPath).parent_path() / "RaspiConfig.toml");

    Application::InitializeConsole();
    Application::Console().StartLoggingToFile();

    if (TierFlags == 0) {
        beammp_warn("[Raspi Host] No plan flag given; running as free (15 minute inactivity shutdown). "
                    "Add --free, --eco or --performance to this server's startup command.");
    }

    // The old manager was a Lua plugin; its stats file is migrated here so the
    // access time the cleanup relies on survives the move out of Resources/Server.
    TRaspiHostManager RaspiHostManager;
    RaspiHostManager.Initialize(Tier, fs::path(ConfigPath).parent_path(),
        fs::path(Application::Settings.getAsString(Settings::Key::General_ResourceFolder)) / "Server" / "RaspiHostUtilsLogs" / "DO-NOT-DELETE");
    Application::RegisterShutdownHandler([&RaspiHostManager] {
        RaspiHostManager.SaveStats();
    });

    Application::SetSubsystemStatus("Main", Application::Status::Starting);

    SetupSignalHandlers();

    Settings settings {};
    beammp_infof("Server name set in new impl: {}", settings.getAsString(Settings::Key::General_Name));

    bool Shutdown = false;
    Application::RegisterShutdownHandler([&Shutdown] {
        beammp_info("If this takes too long, you can press Ctrl+C repeatedly to force a shutdown.");
        Application::SetSubsystemStatus("Main", Application::Status::ShuttingDown);
        Shutdown = true;
    });

    TServer Server(Arguments.List);

    RegisterThread("Main");

    beammp_trace("Running in debug mode on a debug build");
    TResourceManager ResourceManager;
    ResourceManager.RefreshFiles();
    TPPSMonitor PPSMonitor(Server);
    THeartbeatThread Heartbeat(ResourceManager, Server);
    TNetwork Network(Server, PPSMonitor, ResourceManager);

    // The Discord bridge only allocates its thread and queue if enabled in
    // RaspiConfig.toml; otherwise this is a no-op.
    Application::Discord().Configure(Application::Moderation().Config().Discord);
    Application::Discord().Start(Network);
    Application::RegisterShutdownHandler([] {
        Application::Discord().Shutdown();
    });

    auto LuaEngine = std::make_shared<TLuaEngine>();
    LuaEngine->SetServer(&Server);
    Application::Console().InitializeLuaConsole(*LuaEngine);
    LuaEngine->SetNetwork(&Network);
    PPSMonitor.SetNetwork(Network);
    Application::CheckForUpdates();

    TPluginMonitor PluginMonitor(fs::path(Application::Settings.getAsString(Settings::Key::General_ResourceFolder)) / "Server", LuaEngine);

    Application::RegisterShutdownHandler([] {
        auto Futures = LuaAPI::MP::Engine->TriggerEvent("onShutdown", "");
        TLuaEngine::WaitForAll(Futures, std::chrono::seconds(5));
    });
    Application::RegisterShutdownHandler([&Server, &Network] {
        beammp_debug("Kicking all players due to shutdown");
        Server.ForEachClient([&Network](std::weak_ptr<TClient> client) -> bool {
            if (!client.expired()) {
                Network.ClientKick(*client.lock(), "Server shutdown");
            }
            return true;
        });
    });

    RegisterThread("Main(Waiting)");

    std::set<std::string> IgnoreSubsystems {
        "UpdateCheck" // Ignore as not to confuse users (non-vital system)
    };

    bool FullyStarted = false;
    while (!Shutdown) {
        if (!FullyStarted) {
            FullyStarted = true;
            bool WithErrors = false;
            std::string SystemsBadList {};
            auto Statuses = Application::GetSubsystemStatuses();
            for (const auto& NameStatusPair : Statuses) {
                if (NameStatusPair.first == "Main") {
                    continue;
                }

                if (IgnoreSubsystems.count(NameStatusPair.first) > 0) {
                    continue; // ignore
                }
                if (NameStatusPair.second == Application::Status::Starting) {
                    FullyStarted = false;
                } else if (NameStatusPair.second == Application::Status::Bad) {
                    SystemsBadList += NameStatusPair.first + ", ";
                    WithErrors = true;
                }
            }
            // remove ", "
            SystemsBadList = SystemsBadList.substr(0, SystemsBadList.size() - 2);
            if (FullyStarted) {
                Application::SetSubsystemStatus("Main", Application::Status::Good);

                if (!WithErrors) {
                    beammp_info("ALL SYSTEMS STARTED SUCCESSFULLY, EVERYTHING IS OKAY");
                } else {
                    beammp_error("STARTUP NOT SUCCESSFUL, SYSTEMS " + SystemsBadList + " HAD ERRORS. THIS MAY OR MAY NOT CAUSE ISSUES.");
                }
            }
        }
        if (RaspiHostManager.Tick(Server.ClientCount())) {
            Application::GracefullyShutdown();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    Application::SetSubsystemStatus("Main", Application::Status::Shutdown);
    beammp_info("Shutdown.");
    return 0;
}
