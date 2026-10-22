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

#include "TRaspiConfig.h"

#include "Common.h"
#include "TConfig.h" // brings in toml11 the same way the server config does

#include <fstream>
#include <sstream>

namespace Moderation {

namespace {

    const toml::value* FindValue(const toml::value& Table, const std::string& Key) {
        if (!Table.is_table()) {
            return nullptr;
        }
        const auto& AsTable = Table.as_table();
        const auto It = AsTable.find(Key);
        if (It == AsTable.end()) {
            return nullptr;
        }
        return &It->second;
    }

    bool GetBool(const toml::value& Table, const std::string& Key, bool Default) {
        const auto* Value = FindValue(Table, Key);
        if (Value == nullptr) {
            return Default;
        }
        if (!Value->is_boolean()) {
            beammp_warnf("RaspiConfig: '{}' must be a boolean, keeping default", Key);
            return Default;
        }
        return Value->as_boolean();
    }

    int GetInt(const toml::value& Table, const std::string& Key, int Default) {
        const auto* Value = FindValue(Table, Key);
        if (Value == nullptr) {
            return Default;
        }
        if (!Value->is_integer()) {
            beammp_warnf("RaspiConfig: '{}' must be an integer, keeping default", Key);
            return Default;
        }
        const auto Raw = Value->as_integer();
        if (Raw < 0 || Raw > std::numeric_limits<int>::max()) {
            beammp_warnf("RaspiConfig: '{}' is out of range, keeping default", Key);
            return Default;
        }
        return static_cast<int>(Raw);
    }

    std::string GetString(const toml::value& Table, const std::string& Key, const std::string& Default) {
        const auto* Value = FindValue(Table, Key);
        if (Value == nullptr) {
            return Default;
        }
        if (!Value->is_string()) {
            beammp_warnf("RaspiConfig: '{}' must be a string, keeping default", Key);
            return Default;
        }
        return Value->as_string();
    }

    std::string Quote(const std::string& Input) {
        std::string Out = "\"";
        for (const char c : Input) {
            if (c == '"' || c == '\\') {
                Out.push_back('\\');
                Out.push_back(c);
            } else if (c == '\n') {
                Out += "\\n";
            } else {
                Out.push_back(c);
            }
        }
        Out.push_back('"');
        return Out;
    }

} // namespace

TRaspiConfig TRaspiConfig::Load(const std::filesystem::path& Path) {
    if (!std::filesystem::exists(Path)) {
        TRaspiConfig Config;
        beammp_info("No moderation config found, generating '" + Path.string() + "'");
        Config.FlushToFile(Path);
        return Config;
    }

    auto Result = TryLoad(Path);
    if (!Result.Ok) {
        beammp_error("RaspiConfig: failed to parse '" + Path.string() + "': " + Result.Error);
        beammp_error("RaspiConfig: continuing with built-in moderation defaults");
    }
    return Result.Config;
}

TLoadResult TRaspiConfig::TryLoad(const std::filesystem::path& Path) {
    TLoadResult Result;
    TRaspiConfig Config;

    if (!std::filesystem::exists(Path)) {
        Result.Error = "the file does not exist";
        return Result;
    }

    try {
        const toml::value Data = toml::parse(Path.string());

        // [Discord] is a top-level table, independent of [Moderation].
        if (const auto* Discord = FindValue(Data, "Discord"); Discord != nullptr && Discord->is_table()) {
            Config.Discord.Enabled = GetBool(*Discord, "Enabled", Config.Discord.Enabled);
            Config.Discord.RelayUrl = GetString(*Discord, "RelayUrl", Config.Discord.RelayUrl);
            Config.Discord.PairingCode = GetString(*Discord, "PairingCode", Config.Discord.PairingCode);
            Config.Discord.RelayServerChat = GetBool(*Discord, "RelayServerChat", Config.Discord.RelayServerChat);
            Config.Discord.RelayDiscordChat = GetBool(*Discord, "RelayDiscordChat", Config.Discord.RelayDiscordChat);
            Config.Discord.RelayEvents = GetBool(*Discord, "RelayEvents", Config.Discord.RelayEvents);
            Config.Discord.PollWaitSeconds = GetInt(*Discord, "PollWaitSeconds", Config.Discord.PollWaitSeconds);
            Config.Discord.MaxQueue = GetInt(*Discord, "MaxQueue", Config.Discord.MaxQueue);

            // The shared Http helpers time out at 30s, so a longer poll would be
            // cut off by curl every time and the loop would spin.
            Config.Discord.PollWaitSeconds = std::clamp(Config.Discord.PollWaitSeconds, 1, 20);
            Config.Discord.MaxQueue = std::clamp(Config.Discord.MaxQueue, 1, 1000);
            if (Config.Discord.Enabled && Config.Discord.PairingCode.empty()) {
                beammp_warn("RaspiConfig: [Discord] is enabled but PairingCode is empty; get one with /pair in Discord");
            }
        }

        // [Rules] is also top-level and works without moderation enabled: any
        // player can ask for the server rules in chat.
        if (const auto* Rules = FindValue(Data, "Rules"); Rules != nullptr && Rules->is_table()) {
            Config.Rules.Command = std::string(Trim(GetString(*Rules, "Command", Config.Rules.Command)));
            if (const auto* Lines = FindValue(*Rules, "Lines"); Lines != nullptr) {
                if (!Lines->is_array()) {
                    beammp_warn("RaspiConfig: 'Rules.Lines' must be an array of strings, ignoring it");
                } else {
                    for (const auto& Line : Lines->as_array()) {
                        if (!Line.is_string()) {
                            beammp_warn("RaspiConfig: every [Rules] Lines entry must be a string, ignoring one");
                            continue;
                        }
                        const std::string Text(Trim(Line.as_string()));
                        if (!Text.empty()) {
                            Config.Rules.Lines.push_back(Text);
                        }
                    }
                }
            }
        }

        const auto* Mod = FindValue(Data, "Moderation");
        if (Mod == nullptr) {
            beammp_warn("RaspiConfig: no [Moderation] table found, using defaults");
            Result.Config = std::move(Config);
            Result.Ok = true;
            return Result;
        }

        Config.Enabled = GetBool(*Mod, "Enabled", Config.Enabled);
        Config.CommandPrefix = GetString(*Mod, "CommandPrefix", Config.CommandPrefix);
        Config.DefaultMuteDuration = GetString(*Mod, "DefaultMuteDuration", Config.DefaultMuteDuration);
        if (Config.CommandPrefix.empty()) {
            beammp_warn("RaspiConfig: CommandPrefix is empty, falling back to '!'");
            Config.CommandPrefix = "!";
        }

        if (const auto* Operators = FindValue(*Mod, "Operators"); Operators != nullptr && Operators->is_array()) {
            for (const auto& Entry : Operators->as_array()) {
                if (!Entry.is_table()) {
                    continue;
                }
                TOperator Op;
                Op.Name = GetString(Entry, "Name", "");
                Op.IP = GetString(Entry, "IP", "");
                if (Op.Name.empty()) {
                    beammp_warn("RaspiConfig: an operator entry has no Name, ignoring it");
                    continue;
                }
                Config.Operators.push_back(Op);
            }
        }

        if (const auto* Bans = FindValue(*Mod, "Bans"); Bans != nullptr && Bans->is_table()) {
            Config.Bans.Enabled = GetBool(*Bans, "Enabled", Config.Bans.Enabled);
            Config.Bans.DefaultReason = GetString(*Bans, "DefaultReason", Config.Bans.DefaultReason);
            Config.Bans.DefaultDuration = GetString(*Bans, "DefaultDuration", Config.Bans.DefaultDuration);
            Config.Bans.Persist = GetBool(*Bans, "PersistToFile", Config.Bans.Persist);
            Config.Bans.File = GetString(*Bans, "File", Config.Bans.File);
        }

        if (const auto* Filter = FindValue(*Mod, "ChatFilter"); Filter != nullptr && Filter->is_table()) {
            Config.ChatFilter.Enabled = GetBool(*Filter, "Enabled", Config.ChatFilter.Enabled);
            Config.ChatFilter.Action = ToLower(GetString(*Filter, "Action", Config.ChatFilter.Action));
            Config.ChatFilter.MuteMinutes = GetInt(*Filter, "MuteMinutes", Config.ChatFilter.MuteMinutes);
            const auto CensorChar = GetString(*Filter, "CensorChar", std::string(1, Config.ChatFilter.CensorChar));
            if (!CensorChar.empty()) {
                Config.ChatFilter.CensorChar = CensorChar.front();
            }
            if (const auto* Words = FindValue(*Filter, "Words"); Words != nullptr && Words->is_array()) {
                for (const auto& Word : Words->as_array()) {
                    if (Word.is_string() && !Word.as_string().empty()) {
                        Config.ChatFilter.Words.push_back(Word.as_string());
                    }
                }
            }
            if (Config.ChatFilter.Action != "censor" && Config.ChatFilter.Action != "block"
                && Config.ChatFilter.Action != "kick" && Config.ChatFilter.Action != "mute") {
                beammp_warnf("RaspiConfig: unknown ChatFilter action '{}', using 'censor'", Config.ChatFilter.Action);
                Config.ChatFilter.Action = "censor";
            }
            if (Config.ChatFilter.MuteMinutes <= 0) {
                Config.ChatFilter.MuteMinutes = 5;
            }
        }

        if (const auto* Afk = FindValue(*Mod, "Afk"); Afk != nullptr && Afk->is_table()) {
            Config.Afk.Enabled = GetBool(*Afk, "Enabled", Config.Afk.Enabled);
            Config.Afk.TimeoutMinutes = GetInt(*Afk, "TimeoutMinutes", Config.Afk.TimeoutMinutes);
            Config.Afk.WarnGraceSeconds = GetInt(*Afk, "WarnGraceSeconds", Config.Afk.WarnGraceSeconds);
            Config.Afk.KickReason = GetString(*Afk, "KickReason", Config.Afk.KickReason);
            Config.Afk.IgnoreOperators = GetBool(*Afk, "IgnoreOperators", Config.Afk.IgnoreOperators);
            if (Config.Afk.Enabled && Config.Afk.TimeoutMinutes <= 0) {
                beammp_warn("RaspiConfig: Afk.TimeoutMinutes must be greater than 0, disabling AFK enforcement");
                Config.Afk.Enabled = false;
            }
            if (Config.Afk.WarnGraceSeconds < 0) {
                Config.Afk.WarnGraceSeconds = 0;
            }
        }

        if (!ParseDuration(Config.Bans.DefaultDuration).Valid()) {
            beammp_warnf("RaspiConfig: Bans.DefaultDuration '{}' is not a valid duration, using 'permanent'",
                Config.Bans.DefaultDuration);
            Config.Bans.DefaultDuration = "permanent";
        }
        if (!ParseDuration(Config.DefaultMuteDuration).Valid()) {
            beammp_warnf("RaspiConfig: DefaultMuteDuration '{}' is not a valid duration, using '10m'",
                Config.DefaultMuteDuration);
            Config.DefaultMuteDuration = "10m";
        }
        Result.Ok = true;
    } catch (const std::exception& e) {
        Result.Error = e.what();
        // Same as before: an unreadable file leaves the built-in defaults.
        Config = TRaspiConfig { };
    }

    Result.Config = std::move(Config);
    return Result;
}

void TRaspiConfig::FlushToFile(const std::filesystem::path& Path) const {
    std::ostringstream Out;
    Out << "# RaspiConfig.toml - built-in moderation for this server.\n"
           "# This file is independent of ServerConfig.toml and is safe to delete;\n"
           "# it will be regenerated with these defaults on the next start.\n"
           "#\n"
           "# Durations accept \"30s\", \"10m\", \"2h\", \"7d\", \"1w\", a bare number\n"
           "# (minutes) or \"permanent\".\n"
           "\n"
           "[Moderation]\n"
           "Enabled = "
        << (Enabled ? "true" : "false") << "\n"
        << "# Prefix used for in-game operator commands, e.g. \"!ban name 1h spam\".\n"
           "CommandPrefix = "
        << Quote(CommandPrefix) << "\n"
        << "# Duration applied when !mute/ban is used without one.\n"
           "DefaultMuteDuration = "
        << Quote(DefaultMuteDuration) << "\n"
        << "\n"
           "# Players allowed to run in-game moderation commands.\n"
           "# Set IP = \"\" to allow the name from any IP, or pin it to one address so\n"
           "# a stolen account key cannot be used to moderate from elsewhere.\n";

    if (Operators.empty()) {
        Out << "\n# [[Moderation.Operators]]\n"
               "# Name = \"YourBeamMPName\"\n"
               "# IP = \"\"\n";
    } else {
        for (const auto& Operator : Operators) {
            Out << "\n[[Moderation.Operators]]\n"
                   "Name = "
                << Quote(Operator.Name) << "\n"
                << "IP = " << Quote(Operator.IP) << "\n";
        }
    }

    Out << "\n[Moderation.Bans]\n"
           "Enabled = "
        << (Bans.Enabled ? "true" : "false") << "\n"
        << "DefaultReason = " << Quote(Bans.DefaultReason) << "\n"
        << "DefaultDuration = " << Quote(Bans.DefaultDuration) << "\n"
        << "# Bans are written to File so they survive a restart.\n"
           "PersistToFile = "
        << (Bans.Persist ? "true" : "false") << "\n"
        << "File = " << Quote(Bans.File) << "\n"
        << "\n[Moderation.ChatFilter]\n"
           "Enabled = "
        << (ChatFilter.Enabled ? "true" : "false") << "\n"
        << "# One of: censor, block, kick, mute\n"
           "Action = "
        << Quote(ChatFilter.Action) << "\n"
        << "CensorChar = " << Quote(std::string(1, ChatFilter.CensorChar)) << "\n"
        << "# Only used when Action = \"mute\".\n"
           "MuteMinutes = "
        << ChatFilter.MuteMinutes << "\n"
        << "Words = [";
    for (size_t i = 0; i < ChatFilter.Words.size(); ++i) {
        if (i != 0) {
            Out << ", ";
        }
        Out << Quote(ChatFilter.Words[i]);
    }
    Out << "]\n"
           "\n[Moderation.Afk]\n"
           "# Warns idle players, then kicks them once the grace period runs out.\n"
           "Enabled = "
        << (Afk.Enabled ? "true" : "false") << "\n"
        << "TimeoutMinutes = " << Afk.TimeoutMinutes << "\n"
        << "WarnGraceSeconds = " << Afk.WarnGraceSeconds << "\n"
        << "KickReason = " << Quote(Afk.KickReason) << "\n"
        << "# Operators are never treated as AFK.\n"
           "IgnoreOperators = "
        << (Afk.IgnoreOperators ? "true" : "false") << "\n";

    Out << "\n[Rules]\n"
           "# In-game command that shows the rules. Prefix included; empty disables it.\n"
           "Command = "
        << Quote(Rules.Command) << "\n"
        <<           "# One chat line per entry, sent only to the player who asked.\n"
           "# Example: Lines = [\"No ramming\", \"Keep chat friendly\"]\n"
           "# Leave the list empty to disable the command.\n"
           "Lines = [";
    for (size_t i = 0; i < Rules.Lines.size(); ++i) {
        if (i != 0) {
            Out << ", ";
        }
        Out << Quote(Rules.Lines[i]);
    }
    Out << "]\n";

    Out << "\n[Discord]\n"
           "# Chat bridge to the public Raspi Host Discord bot.\n"
           "# Disabled means no thread, socket or queue is created at all.\n"
           "# Only read at startup. The console 'reload' command picks up the rest\n"
           "# of this file while the server is running.\n"
           "Enabled = "
        << (Discord.Enabled ? "true" : "false") << "\n"
        << "# Address of the relay that the bot runs on. Leave empty until paired.\n"
           "RelayUrl = "
        << Quote(Discord.RelayUrl) << "\n"
        << "# Run /pair in the Discord channel, then paste the code here.\n"
           "# Anyone with this code can read and write that channel's relay, so\n"
           "# treat it like a password.\n"
           "PairingCode = "
        << Quote(Discord.PairingCode) << "\n"
        << "RelayServerChat = " << (Discord.RelayServerChat ? "true" : "false") << "\n"
        << "RelayDiscordChat = " << (Discord.RelayDiscordChat ? "true" : "false") << "\n"
        << "# Announce server start/stop and player join/leave in Discord.\n"
           "RelayEvents = "
        << (Discord.RelayEvents ? "true" : "false") << "\n"
        << "# Long-poll window in seconds (1-20; the HTTP client times out at 30).\n"
           "PollWaitSeconds = "
        << Discord.PollWaitSeconds << "\n"
        << "# Bounded outgoing queue; oldest messages are dropped when full.\n"
           "MaxQueue = "
        << Discord.MaxQueue << "\n";

    std::ofstream File(Path, std::ios::trunc);
    if (!File) {
        beammp_error("RaspiConfig: failed to write '" + Path.string() + "'");
        return;
    }
    File << Out.str();
    if (!File.good()) {
        beammp_error("RaspiConfig: failed while writing '" + Path.string() + "'");
    }
}

} // namespace Moderation

TEST_CASE("Moderation::ParseDuration") {
    using namespace Moderation;

    CHECK_FALSE(ParseDuration("").Valid());
    CHECK_FALSE(ParseDuration("abc").Valid());
    CHECK_FALSE(ParseDuration("5x").Valid());
    CHECK_FALSE(ParseDuration("-5m").Valid());

    CHECK(ParseDuration("permanent").IsPermanent());
    CHECK(ParseDuration("perm").IsPermanent());
    CHECK(ParseDuration("0").IsPermanent());

    CHECK(ParseDuration("30s").Seconds == std::chrono::seconds(30));
    CHECK(ParseDuration("10m").Seconds == std::chrono::minutes(10));
    CHECK(ParseDuration("2h").Seconds == std::chrono::hours(2));
    CHECK(ParseDuration("7d").Seconds == std::chrono::hours(24 * 7));
    CHECK(ParseDuration("1w").Seconds == std::chrono::hours(24 * 7));
    // a bare number means minutes
    CHECK(ParseDuration("90").Seconds == std::chrono::minutes(90));
    // surrounding whitespace is tolerated
    CHECK(ParseDuration(" 15m ").Seconds == std::chrono::minutes(15));
    // uppercase units work too
    CHECK(ParseDuration("3H").Seconds == std::chrono::hours(3));
}

TEST_CASE("Moderation::FormatDuration") {
    using namespace Moderation;
    CHECK(FormatDuration(std::chrono::seconds(0)) == "0s");
    CHECK(FormatDuration(std::chrono::seconds(45)) == "45s");
    CHECK(FormatDuration(std::chrono::minutes(3)) == "3m");
    CHECK(FormatDuration(std::chrono::hours(2) + std::chrono::minutes(5)) == "2h 5m");
    CHECK(FormatDuration(std::chrono::hours(24 * 2) + std::chrono::seconds(30)) == "2d 30s");
}

TEST_CASE("Moderation::CensorText") {
    using namespace Moderation;
    const std::vector<std::string> Words { "bad", "worse" };

    CHECK(CensorText("this is bad", Words, '*') == "this is ***");
    CHECK(CensorText("BAD and Worse", Words, '#') == "### and #####");
    CHECK(CensorText("nothing here", Words, '*') == "nothing here");
    // every occurrence, and the length is preserved
    const auto Censored = CensorText("bad bad bad", Words, '*');
    CHECK(Censored == "*** *** ***");
    CHECK(Censored.size() == std::string("bad bad bad").size());

    CHECK(FindFilteredWord("this is WORSE", Words) == "worse");
    CHECK(FindFilteredWord("clean text", Words).empty());
}

TEST_CASE("Moderation::MatchesOperator") {
    using namespace Moderation;

    // exact name, any IP
    CHECK(MatchesOperator("Raspi", "1.2.3.4", "raspi", ""));
    // case insensitive name
    CHECK(MatchesOperator("RASPI", "1.2.3.4", "Raspi", ""));
    // pinned IP must match
    CHECK(MatchesOperator("Raspi", "1.2.3.4", "Raspi", "1.2.3.4"));
    CHECK_FALSE(MatchesOperator("Raspi", "5.6.7.8", "Raspi", "1.2.3.4"));
    // no prefix matching: "/" style admin lookalikes must not pass
    CHECK_FALSE(MatchesOperator("Raspi-other", "1.2.3.4", "Raspi", ""));
    CHECK_FALSE(MatchesOperator("ras", "1.2.3.4", "Raspi", ""));
    // an empty operator name never matches
    CHECK_FALSE(MatchesOperator("", "1.2.3.4", "", ""));
}

TEST_CASE("Moderation::TRaspiConfig defaults") {
    const Moderation::TRaspiConfig Defaults;
    CHECK(Defaults.Enabled);
    CHECK(Defaults.CommandPrefix == "!");
    CHECK(Defaults.Bans.Enabled);
    CHECK(Defaults.Bans.DefaultDuration == "permanent");
    CHECK_FALSE(Defaults.ChatFilter.Enabled);
    CHECK(Defaults.ChatFilter.Action == "censor");
    CHECK_FALSE(Defaults.Afk.Enabled);
    CHECK(Defaults.Afk.TimeoutMinutes > 0);
    // a fresh config must not contain half-configured operators
    CHECK(Defaults.Operators.empty());

    // the Discord bridge is off by default and must not carry a pairing code
    CHECK_FALSE(Defaults.Discord.Enabled);
    CHECK(Defaults.Discord.RelayUrl.empty());
    CHECK(Defaults.Discord.PairingCode.empty());
    // join/leave/start/stop announcements are on once the bridge is enabled
    CHECK(Defaults.Discord.RelayEvents);
    CHECK(Defaults.Discord.RelayServerChat);
    CHECK(Defaults.Discord.RelayDiscordChat);
    // must stay below the 30s HTTP timeout or long-polls get cut short
    CHECK(Defaults.Discord.PollWaitSeconds > 0);
    CHECK(Defaults.Discord.PollWaitSeconds <= 20);
    CHECK(Defaults.Discord.MaxQueue > 0);

    // the rules command ships enabled but silent: no placeholder lines
    CHECK(Defaults.Rules.Command == "!rules");
    CHECK(Defaults.Rules.Lines.empty());
}

TEST_CASE("Moderation::MatchesCommand") {
    using namespace Moderation;

    CHECK(MatchesCommand("!rules", "!rules"));
    CHECK(MatchesCommand("  !rules \n", "!rules"));
    CHECK(MatchesCommand("!RULES", "!rules"));
    CHECK(MatchesCommand("!rules", " !rules "));
    // extra words are ordinary chat, not the command
    CHECK_FALSE(MatchesCommand("!rules please", "!rules"));
    CHECK_FALSE(MatchesCommand("rules", "!rules"));
    // an empty command means "off"
    CHECK_FALSE(MatchesCommand("!rules", ""));
    CHECK_FALSE(MatchesCommand("", "!rules"));
}

TEST_CASE("Moderation::FormatRuleLines") {
    using namespace Moderation;

    CHECK(FormatRuleLines({}).empty());

    // Empty entries are dropped; over-long ones are cut to fit a chat packet.
    const auto Out = FormatRuleLines({ "No ramming", "", std::string(500, 'x') });
    REQUIRE(Out.size() == 2u);
    CHECK(Out[0] == "No ramming");
    CHECK(Out[1].size() == 400u);
}

TEST_CASE("Moderation::TRaspiConfig loads the rules command") {
    using namespace Moderation;

    const auto Path = std::filesystem::temp_directory_path() / "rh-rules-test.toml";
    {
        std::ofstream Out(Path);
        Out << "[Rules]\n"
               "Command = \"!regeln\"\n"
               "Lines = [\"No ramming\", \"  Be nice  \", \"\", \"Report bugs on Discord\"]\n"
               "\n[Moderation]\n"
               "Enabled = true\n";
    }

    const auto Config = TRaspiConfig::Load(Path);
    std::filesystem::remove(Path);

    CHECK(Config.Rules.Command == "!regeln");
    // empty entries are dropped, surrounding whitespace is trimmed
    REQUIRE(Config.Rules.Lines.size() == 3u);
    CHECK(Config.Rules.Lines[0] == "No ramming");
    CHECK(Config.Rules.Lines[1] == "Be nice");
    CHECK(Config.Rules.Lines[2] == "Report bugs on Discord");
    CHECK(MatchesCommand("!regeln", Config.Rules.Command));
}

TEST_CASE("Moderation::TRaspiConfig loads several operators") {
    using namespace Moderation;

    const auto Path = std::filesystem::temp_directory_path() / "rh-moderation-operators-test.toml";
    {
        std::ofstream Out(Path);
        Out << "[Moderation]\n"
               "Enabled = true\n"
               "CommandPrefix = \"!\"\n"
               "\n[[Moderation.Operators]]\n"
               "Name = \"Raspi\"\n"
               "IP = \"\"\n"
               "\n[[Moderation.Operators]]\n"
               "Name = \"SecondAdmin\"\n"
               "IP = \"203.0.113.7\"\n"
               "\n[[Moderation.Operators]]\n"
               "Name = \"SecondAdmin\"\n"
               "IP = \"198.51.100.9\"\n"
               "\n[[Moderation.Operators]]\n"
               "IP = \"192.0.2.1\"\n";
    }

    const auto Config = TRaspiConfig::Load(Path);
    std::filesystem::remove(Path);

    // The nameless entry is dropped; the repeated name is kept once per IP.
    REQUIRE(Config.Operators.size() == 3u);
    CHECK(Config.Operators[0].Name == "Raspi");
    CHECK(Config.Operators[0].IP.empty());
    CHECK(Config.Operators[1].Name == "SecondAdmin");
    CHECK(Config.Operators[1].IP == "203.0.113.7");
    CHECK(Config.Operators[2].IP == "198.51.100.9");

    // Either of the pinned addresses is accepted for that one name.
    const auto IsOperator = [&Config](const std::string& Name, const std::string& Ip) {
        return std::any_of(Config.Operators.begin(), Config.Operators.end(),
            [&Name, &Ip](const TRaspiConfig::TOperator& Op) { return MatchesOperator(Name, Ip, Op.Name, Op.IP); });
    };
    CHECK(IsOperator("SecondAdmin", "203.0.113.7"));
    CHECK(IsOperator("SecondAdmin", "198.51.100.9"));
    CHECK_FALSE(IsOperator("SecondAdmin", "192.0.2.1"));
    CHECK(IsOperator("raspi", "10.0.0.1"));
}

TEST_CASE("Moderation::TRaspiConfig::TryLoad") {
    using namespace Moderation;
    namespace fs = std::filesystem;
    const auto Dir = fs::temp_directory_path() / "rh-tryload-test";
    std::error_code Ec;
    fs::remove_all(Dir, Ec);
    fs::create_directories(Dir);

    // A missing file is an error, and reading must not create it.
    const auto MissingPath = Dir / "missing.toml";
    const auto Missing = TRaspiConfig::TryLoad(MissingPath);
    CHECK_FALSE(Missing.Ok);
    CHECK_FALSE(Missing.Error.empty());
    CHECK_FALSE(fs::exists(MissingPath));

    const auto ValidPath = Dir / "valid.toml";
    {
        std::ofstream Out(ValidPath);
        REQUIRE(Out.good());
        Out << "[Moderation]\n"
               "Enabled = false\n"
               "[Moderation.Bans]\n"
               "DefaultDuration = \"1h\"\n";
    }
    const auto Valid = TRaspiConfig::TryLoad(ValidPath);
    CHECK(Valid.Ok);
    CHECK(Valid.Error.empty());
    CHECK_FALSE(Valid.Config.Enabled);
    CHECK(Valid.Config.Bans.DefaultDuration == "1h");

    // A broken file reports the parse error and falls back to the defaults, so
    // the caller can keep the config it already has.
    const auto BrokenPath = Dir / "broken.toml";
    {
        std::ofstream Out(BrokenPath);
        REQUIRE(Out.good());
        Out << "[Moderation\nEnabled = \"unterminated\n";
    }
    const auto Broken = TRaspiConfig::TryLoad(BrokenPath);
    CHECK_FALSE(Broken.Ok);
    CHECK_FALSE(Broken.Error.empty());
    CHECK(Broken.Config.Enabled);
    CHECK(Broken.Config.Bans.DefaultDuration == "permanent");

    fs::remove_all(Dir, Ec);
}

TEST_CASE("Moderation::DiffConfigs") {
    using namespace Moderation;
    const TRaspiConfig Base;

    CHECK(DiffConfigs(Base, Base).empty());

    {
        TRaspiConfig Changed = Base;
        Changed.Bans.Enabled = false;
        Changed.ChatFilter.Enabled = true;
        Changed.ChatFilter.MuteMinutes = 15;
        const auto Changes = DiffConfigs(Base, Changed);
        REQUIRE(Changes.size() == 3u);
        CHECK(Changes[0].Section == "Moderation.Bans");
        CHECK(Changes[0].Text == "Moderation.Bans.Enabled: true -> false");
        CHECK(Changes[1].Text == "Moderation.ChatFilter.Enabled: false -> true");
        CHECK(Changes[2].Text == "Moderation.ChatFilter.MuteMinutes: 5 -> 15");
    }

    {
        TRaspiConfig Changed = Base;
        Changed.ChatFilter.Words = { "one", "two" };
        Changed.Rules.Lines = { "be nice" };
        const auto Changes = DiffConfigs(Base, Changed);
        REQUIRE(Changes.size() == 2u);
        CHECK(Changes[0].Text == "Moderation.ChatFilter.Words: 0 -> 2");
        CHECK(Changes[1].Text == "Rules.Lines: 0 -> 1");
    }

    {
        TRaspiConfig Changed = Base;
        Changed.Operators.push_back(TRaspiConfig::TOperator { "Raspi", "" });
        auto Changes = DiffConfigs(Base, Changed);
        REQUIRE(Changes.size() == 1u);
        CHECK(Changes[0].Text == "Moderation.Operators: none -> Raspi");

        Changed.Operators.push_back(TRaspiConfig::TOperator { "Bob", "203.0.113.7" });
        Changes = DiffConfigs(Base, Changed);
        REQUIRE(Changes.size() == 1u);
        CHECK(Changes[0].Text == "Moderation.Operators: none -> Raspi, Bob (203.0.113.7)");
    }

    {
        // The pairing code is a credential: it must never be printed, only the
        // fact that it changed.
        TRaspiConfig Changed = Base;
        Changed.Discord.PairingCode = "SECRET-CODE";
        const auto Changes = DiffConfigs(Base, Changed);
        REQUIRE(Changes.size() == 1u);
        CHECK(Changes[0].Section == "Discord");
        CHECK(Changes[0].Text == "Discord.PairingCode: changed");
        CHECK(Changes[0].Text.find("SECRET-CODE") == std::string::npos);
    }

    {
        // Clearing or filling a value has to be readable in the log too.
        TRaspiConfig Changed = Base;
        Changed.Discord.RelayUrl = "https://relay.example";
        auto Changes = DiffConfigs(Base, Changed);
        REQUIRE(Changes.size() == 1u);
        CHECK(Changes[0].Text == "Discord.RelayUrl: (empty) -> https://relay.example");

        Changes = DiffConfigs(Changed, Base);
        REQUIRE(Changes.size() == 1u);
        CHECK(Changes[0].Text == "Discord.RelayUrl: https://relay.example -> (empty)");
    }
}
