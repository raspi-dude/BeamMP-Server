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

        /// Writes this config out, overwriting the file.
        void FlushToFile(const std::filesystem::path& Path) const;
    };

} // namespace Moderation
