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

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Built-in moderation. Settings live in their own TOML file (RaspiConfig.toml)
// so that ServerConfig.toml keeps its original shape and semantics.
//
// Everything in the `Moderation` namespace is dependency-light on purpose:
// the helpers are inline so they can be unit tested without the rest of the
// server (no fmt/boost/toml11 needed).
namespace Moderation {

    inline std::string_view Trim(std::string_view Text) {
        while (!Text.empty() && std::isspace(static_cast<unsigned char>(Text.front())) != 0) {
            Text.remove_prefix(1);
        }
        while (!Text.empty() && std::isspace(static_cast<unsigned char>(Text.back())) != 0) {
            Text.remove_suffix(1);
        }
        return Text;
    }

    inline std::string ToLower(std::string_view Text) {
        std::string Out;
        Out.reserve(Text.size());
        for (const char c : Text) {
            Out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return Out;
    }

    /// Result of parsing a ban or mute duration such as "30m", "12h", "7d", "permanent".
    struct TDuration {
        enum class EKind {
            Invalid, ///< not parsable, caller should reject the input
            Permanent, ///< "permanent", "perm", "0", "never"
            Timed ///< Seconds is meaningful
        };

        EKind Kind { EKind::Invalid };
        std::chrono::seconds Seconds { 0 };

        [[nodiscard]] bool Valid() const { return Kind != EKind::Invalid; }
        [[nodiscard]] bool IsPermanent() const { return Kind == EKind::Permanent; }
    };

    /// Accepts "30s", "10m", "2h", "7d", "1w", a bare number (minutes),
    /// and the words "permanent", "perm", "never", "forever", "0".
    inline TDuration ParseDuration(std::string_view Text) {
        TDuration Result;
        Text = Trim(Text);
        if (Text.empty()) {
            return Result;
        }
        const std::string Lower = ToLower(Text);

        if (Lower == "0" || Lower == "perm" || Lower == "permanent" || Lower == "never" || Lower == "forever") {
            Result.Kind = TDuration::EKind::Permanent;
            return Result;
        }

        size_t NumberEnd = 0;
        while (NumberEnd < Lower.size()
            && (std::isdigit(static_cast<unsigned char>(Lower[NumberEnd])) != 0 || Lower[NumberEnd] == '.')) {
            ++NumberEnd;
        }
        if (NumberEnd == 0) {
            return Result;
        }

        double Value = 0.0;
        try {
            Value = std::stod(Lower.substr(0, NumberEnd));
        } catch (const std::exception&) {
            return Result;
        }
        if (!(Value > 0.0)) {
            return Result;
        }

        const std::string_view Unit(Lower.data() + NumberEnd, Lower.size() - NumberEnd);
        double Multiplier = 0.0;
        if (Unit.empty() || Unit == "m" || Unit == "min" || Unit == "mins" || Unit == "minute" || Unit == "minutes") {
            Multiplier = 60.0; // a bare number is minutes, matching how admins usually reason
        } else if (Unit == "s" || Unit == "sec" || Unit == "secs" || Unit == "second" || Unit == "seconds") {
            Multiplier = 1.0;
        } else if (Unit == "h" || Unit == "hour" || Unit == "hours") {
            Multiplier = 3600.0;
        } else if (Unit == "d" || Unit == "day" || Unit == "days") {
            Multiplier = 86400.0;
        } else if (Unit == "w" || Unit == "week" || Unit == "weeks") {
            Multiplier = 604800.0;
        } else {
            return Result; // unknown unit
        }

        const double TotalSeconds = Value * Multiplier;
        constexpr double MaxSeconds = static_cast<double>(std::numeric_limits<int64_t>::max());
        if (TotalSeconds >= MaxSeconds) {
            return Result;
        }

        Result.Kind = TDuration::EKind::Timed;
        Result.Seconds = std::chrono::seconds(static_cast<int64_t>(TotalSeconds));
        return Result;
    }

    /// Human readable, e.g. "2d 3h 15m". Always returns at least "0s".
    inline std::string FormatDuration(std::chrono::seconds Duration) {
        int64_t Total = Duration.count();
        if (Total <= 0) {
            return "0s";
        }
        const int64_t Days = Total / 86400;
        Total %= 86400;
        const int64_t Hours = Total / 3600;
        Total %= 3600;
        const int64_t Minutes = Total / 60;
        const int64_t Seconds = Total % 60;

        std::string Out;
        const auto Append = [&Out](int64_t Value, char Unit) {
            if (Value <= 0) {
                return;
            }
            if (!Out.empty()) {
                Out.push_back(' ');
            }
            Out += std::to_string(Value);
            Out.push_back(Unit);
        };
        Append(Days, 'd');
        Append(Hours, 'h');
        Append(Minutes, 'm');
        Append(Seconds, 's');
        return Out.empty() ? "0s" : Out;
    }

    /// First configured word occurring in Text (case insensitive), or empty.
    inline std::string FindFilteredWord(const std::string& Text, const std::vector<std::string>& Words) {
        const std::string LowerText = ToLower(Text);
        for (const auto& Word : Words) {
            if (Word.empty()) {
                continue;
            }
            if (LowerText.find(ToLower(Word)) != std::string::npos) {
                return Word;
            }
        }
        return {};
    }

    /// Replaces every occurrence of every configured word with CensorChar,
    /// keeping the message length (and therefore the layout) intact.
    inline std::string CensorText(const std::string& Text, const std::vector<std::string>& Words, char CensorChar) {
        std::string Result = Text;
        const std::string LowerText = ToLower(Text);
        for (const auto& Word : Words) {
            if (Word.empty()) {
                continue;
            }
            const std::string LowerWord = ToLower(Word);
            size_t Pos = 0;
            while ((Pos = LowerText.find(LowerWord, Pos)) != std::string::npos) {
                for (size_t i = 0; i < LowerWord.size() && Pos + i < Result.size(); ++i) {
                    Result[Pos + i] = CensorChar;
                }
                Pos += LowerWord.size();
            }
        }
        return Result;
    }

    /// Operator match: the name must match exactly (case insensitive) and, if
    /// the configured operator has an IP, the IP must match too. An empty IP in
    /// the config means "any IP". An empty operator name never matches.
    inline bool MatchesOperator(std::string_view Name, std::string_view IP,
        std::string_view OperatorName, std::string_view OperatorIP) {
        if (OperatorName.empty()) {
            return false;
        }
        if (ToLower(Name) != ToLower(OperatorName)) {
            return false;
        }
        if (OperatorIP.empty()) {
            return true;
        }
        return Trim(IP) == Trim(OperatorIP);
    }

    /// True when Message is exactly Command, ignoring case and surrounding
    /// whitespace. An empty command never matches, which is how the feature is
    /// switched off.
    inline bool MatchesCommand(std::string_view Message, std::string_view Command) {
        if (Trim(Command).empty()) {
            return false;
        }
        return ToLower(Trim(Message)) == ToLower(Trim(Command));
    }

    /// Truncates each configured rules line so it fits in a chat packet, and
    /// drops empty ones so nobody gets a blank "Server:" line.
    inline std::vector<std::string> FormatRuleLines(const std::vector<std::string>& Lines) {
        std::vector<std::string> Out;
        Out.reserve(Lines.size());
        for (auto Line : Lines) {
            if (Line.size() > 400) {
                Line.resize(400);
            }
            if (!Line.empty()) {
                Out.push_back(std::move(Line));
            }
        }
        return Out;
    }

    struct TLoadResult;

    /// One RASPI_* environment variable that replaced a value from
    /// RaspiConfig.toml. Empty variables are ignored, so the file stays in
    /// charge of everything the panel does not set.
    struct TEnvOverride {
        std::string Section; ///< TOML table, e.g. "Moderation.ChatFilter"
        std::string Key; ///< key inside that table
        std::string EnvName; ///< variable the value came from
    };

    /// Settings of RaspiConfig.toml.
    struct TRaspiConfig {
        struct TOperator {
            std::string Name;
            std::string IP; ///< empty means "any IP"
        };

        struct TBanSettings {
            bool Enabled { true };
            std::string DefaultReason { "Banned by a server administrator" };
            std::string DefaultDuration { "permanent" };
            bool Persist { true };
            std::string File { "RaspiBans.json" };
        };

        struct TChatFilterSettings {
            bool Enabled { false };
            std::string Action { "censor" }; ///< censor, block, kick or mute
            std::vector<std::string> Words;
            char CensorChar { '*' };
            int MuteMinutes { 5 };
        };

        struct TAfkSettings {
            bool Enabled { false };
            int TimeoutMinutes { 20 };
            int WarnGraceSeconds { 60 };
            std::string KickReason { "Kicked for being AFK" };
            bool IgnoreOperators { true };
        };

        /// Settings for the central Raspi Host Discord relay. No bot token is
        /// stored here: the pairing code identifies one Discord channel.
        struct TDiscordSettings {
            bool Enabled { false };
            std::string RelayUrl; ///< empty until the instance is paired
            std::string PairingCode; ///< from /pair in Discord; treat like a password
            bool RelayServerChat { true };
            bool RelayDiscordChat { true };
            bool RelayEvents { true }; ///< server start/stop and player join/leave
            int PollWaitSeconds { 20 };
            int MaxQueue { 64 };
        };

        /// The in-game `!rules` command. Independent of [Moderation]: it is
        /// meant for players, not operators.
        struct TRulesSettings {
            std::string Command { "!rules" }; ///< prefix included; empty disables it
            std::vector<std::string> Lines; ///< one chat line per entry; empty disables it
        };

        bool Enabled { true };
        std::string CommandPrefix { "!" };
        std::string DefaultMuteDuration { "10m" };
        std::vector<TOperator> Operators;
        TBanSettings Bans;
        TChatFilterSettings ChatFilter;
        TAfkSettings Afk;
        TDiscordSettings Discord;
        TRulesSettings Rules;

        /// Loads Path, writing a documented default file first if it is missing.
        /// Never throws; on a parse error the built-in defaults are kept.
        static TRaspiConfig Load(const std::filesystem::path& Path);

        /// Like Load, but never creates the file and reports whether the
        /// existing one could be read. `reload` uses it so a typo cannot
        /// replace the running config with defaults.
        ///
        /// RASPI_* environment variables are applied on top of the file here,
        /// so every caller (startup, `reload`, tests) sees the same values.
        static TLoadResult TryLoad(const std::filesystem::path& Path);

        /// Writes this config out, overwriting the file.
        void FlushToFile(const std::filesystem::path& Path) const;
    };

    /// Outcome of TRaspiConfig::TryLoad: the config that was read, and whether
    /// it could be read at all. `Error` is empty when Ok is true. Defined after
    /// TRaspiConfig because it holds one by value.
    struct TLoadResult {
        TRaspiConfig Config;
        bool Ok { false };
        std::string Error;
        /// Variables that overrode the file, in the order they were applied.
        std::vector<TEnvOverride> EnvOverrides;
    };

    /// The TOML text for one key of Config, e.g. `true`, `20` or `"10m"`.
    /// Empty when the key is not one the environment may set.
    std::optional<std::string> RenderedValue(const TRaspiConfig& Config, const std::string& Section, const std::string& Key);

    /// Writes the overridden keys back into the file, leaving every other byte
    /// alone: comments, ordering, line endings and unrelated values survive.
    /// Only lines whose value actually differs are rewritten. Returns how many
    /// lines changed, so 0 means the file already agreed with the server.
    std::size_t WriteEnvOverrides(const std::filesystem::path& Path, const TRaspiConfig& Config,
        const std::vector<TEnvOverride>& Overrides);

    /// One difference between two configurations, as reported by `reload`.
    struct TConfigChange {
        std::string Section; ///< "Moderation", "Moderation.Bans", "Rules", "Discord", ...
        /// Ready to print, e.g. "Moderation.Bans.Enabled: true -> false".
        std::string Text;
    };

    inline std::string BoolText(bool Value) { return Value ? "true" : "false"; }

    /// Empty values would leave a blank spot in a log line, so spell them out.
    inline std::string ValueText(const std::string& Value) { return Value.empty() ? "(empty)" : Value; }

    /// Operators as one comparable string, e.g. "Raspi, Bob (203.0.113.7)".
    inline std::string OperatorListText(const std::vector<TRaspiConfig::TOperator>& Operators) {
        std::string Out;
        for (const auto& Operator : Operators) {
            if (!Out.empty()) {
                Out += ", ";
            }
            Out += Operator.Name;
            if (!Operator.IP.empty()) {
                Out += " (" + Operator.IP + ")";
            }
        }
        return Out.empty() ? "none" : Out;
    }

    /// Setting-by-setting comparison for `reload`, so it only reports what an
    /// edit actually changed. Word and line lists are compared by size; the
    /// names of a bad-word list are noise in the log.
    inline std::vector<TConfigChange> DiffConfigs(const TRaspiConfig& Old, const TRaspiConfig& New) {
        std::vector<TConfigChange> Changes;
        const auto Report = [&Changes](const std::string& Section, const std::string& Name,
                                const std::string& OldValue, const std::string& NewValue) {
            if (OldValue != NewValue) {
                Changes.push_back(TConfigChange { Section,
                    Section + "." + Name + ": " + ValueText(OldValue) + " -> " + ValueText(NewValue) });
            }
        };
        Report("Moderation", "Enabled", BoolText(Old.Enabled), BoolText(New.Enabled));
        Report("Moderation", "CommandPrefix", Old.CommandPrefix, New.CommandPrefix);
        Report("Moderation", "DefaultMuteDuration", Old.DefaultMuteDuration, New.DefaultMuteDuration);

        const std::string OldOperators = OperatorListText(Old.Operators);
        const std::string NewOperators = OperatorListText(New.Operators);
        if (OldOperators != NewOperators) {
            Changes.push_back(TConfigChange { "Moderation.Operators",
                "Moderation.Operators: " + OldOperators + " -> " + NewOperators });
        }

        Report("Moderation.Bans", "Enabled", BoolText(Old.Bans.Enabled), BoolText(New.Bans.Enabled));
        Report("Moderation.Bans", "DefaultReason", Old.Bans.DefaultReason, New.Bans.DefaultReason);
        Report("Moderation.Bans", "DefaultDuration", Old.Bans.DefaultDuration, New.Bans.DefaultDuration);
        Report("Moderation.Bans", "PersistToFile", BoolText(Old.Bans.Persist), BoolText(New.Bans.Persist));
        Report("Moderation.Bans", "File", Old.Bans.File, New.Bans.File);

        Report("Moderation.ChatFilter", "Enabled", BoolText(Old.ChatFilter.Enabled), BoolText(New.ChatFilter.Enabled));
        Report("Moderation.ChatFilter", "Action", Old.ChatFilter.Action, New.ChatFilter.Action);
        Report("Moderation.ChatFilter", "CensorChar", std::string(1, Old.ChatFilter.CensorChar),
            std::string(1, New.ChatFilter.CensorChar));
        Report("Moderation.ChatFilter", "MuteMinutes", std::to_string(Old.ChatFilter.MuteMinutes),
            std::to_string(New.ChatFilter.MuteMinutes));
        Report("Moderation.ChatFilter", "Words", std::to_string(Old.ChatFilter.Words.size()),
            std::to_string(New.ChatFilter.Words.size()));

        Report("Moderation.Afk", "Enabled", BoolText(Old.Afk.Enabled), BoolText(New.Afk.Enabled));
        Report("Moderation.Afk", "TimeoutMinutes", std::to_string(Old.Afk.TimeoutMinutes),
            std::to_string(New.Afk.TimeoutMinutes));
        Report("Moderation.Afk", "WarnGraceSeconds", std::to_string(Old.Afk.WarnGraceSeconds),
            std::to_string(New.Afk.WarnGraceSeconds));
        Report("Moderation.Afk", "KickReason", Old.Afk.KickReason, New.Afk.KickReason);
        Report("Moderation.Afk", "IgnoreOperators", BoolText(Old.Afk.IgnoreOperators), BoolText(New.Afk.IgnoreOperators));

        Report("Rules", "Command", Old.Rules.Command, New.Rules.Command);
        Report("Rules", "Lines", std::to_string(Old.Rules.Lines.size()),
            std::to_string(New.Rules.Lines.size()));

        Report("Discord", "Enabled", BoolText(Old.Discord.Enabled), BoolText(New.Discord.Enabled));
        Report("Discord", "RelayUrl", Old.Discord.RelayUrl, New.Discord.RelayUrl);
        if (Old.Discord.PairingCode != New.Discord.PairingCode) {
            // The pairing code is a credential: say that it changed, never print it.
            Changes.push_back(TConfigChange { "Discord", "Discord.PairingCode: changed" });
        }
        Report("Discord", "RelayServerChat", BoolText(Old.Discord.RelayServerChat),
            BoolText(New.Discord.RelayServerChat));
        Report("Discord", "RelayDiscordChat", BoolText(Old.Discord.RelayDiscordChat),
            BoolText(New.Discord.RelayDiscordChat));
        Report("Discord", "RelayEvents", BoolText(Old.Discord.RelayEvents), BoolText(New.Discord.RelayEvents));
        Report("Discord", "PollWaitSeconds", std::to_string(Old.Discord.PollWaitSeconds),
            std::to_string(New.Discord.PollWaitSeconds));
        Report("Discord", "MaxQueue", std::to_string(Old.Discord.MaxQueue),
            std::to_string(New.Discord.MaxQueue));

        return Changes;
    }

} // namespace Moderation
