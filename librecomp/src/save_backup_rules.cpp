#include "librecomp/save_backup_rules.hpp"

#include <algorithm>
#include <cstdio>

namespace recomp {
    namespace save_backups {
        // Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's days_from_civil).
        static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
            y -= (m <= 2) ? 1 : 0;
            const int64_t era = ((y >= 0) ? y : (y - 399)) / 400;
            const int64_t yoe = y - era * 400;
            const int64_t doy = (153 * (m + ((m > 2) ? -3 : 9)) + 2) / 5 + d - 1;
            const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            return era * 146097 + doe - 719468;
        }

        int64_t Timestamp::sort_key() const {
            return days_from_civil(year, month, day) * 86400 + int64_t(hour) * 3600 + int64_t(minute) * 60 + second;
        }

        int64_t seconds_between(const Timestamp &a, const Timestamp &b) {
            return b.sort_key() - a.sort_key();
        }

        static std::string stem_of(const std::string &save_file_name) {
            const std::string ext = ".bin";
            if ((save_file_name.size() > ext.size()) && (save_file_name.compare(save_file_name.size() - ext.size(), ext.size(), ext) == 0)) {
                return save_file_name.substr(0, save_file_name.size() - ext.size());
            }

            return save_file_name;
        }

        std::string backup_file_name(const std::string &save_file_name, const Timestamp &when) {
            char stamp[32];
            snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d", when.year, when.month, when.day, when.hour, when.minute, when.second);
            return stem_of(save_file_name) + "." + stamp + ".bin";
        }

        static bool parse_digits(const std::string &s, size_t pos, size_t count, int &out) {
            out = 0;
            for (size_t i = pos; i < pos + count; i++) {
                if ((s[i] < '0') || (s[i] > '9')) {
                    return false;
                }

                out = out * 10 + (s[i] - '0');
            }

            return true;
        }

        std::optional<Timestamp> parse_backup_file_name(const std::string &save_file_name, const std::string &candidate) {
            // <stem>.YYYYMMDD-HHMMSS.bin
            const std::string stem = stem_of(save_file_name);
            const size_t stampLength = 15;
            const std::string ext = ".bin";
            if (candidate.size() != stem.size() + 1 + stampLength + ext.size()) {
                return std::nullopt;
            }

            if ((candidate.compare(0, stem.size(), stem) != 0) || (candidate[stem.size()] != '.') ||
                (candidate.compare(candidate.size() - ext.size(), ext.size(), ext) != 0))
            {
                return std::nullopt;
            }

            const size_t p = stem.size() + 1;
            Timestamp t;
            if (!parse_digits(candidate, p, 4, t.year) || !parse_digits(candidate, p + 4, 2, t.month) || !parse_digits(candidate, p + 6, 2, t.day) ||
                (candidate[p + 8] != '-') || !parse_digits(candidate, p + 9, 2, t.hour) || !parse_digits(candidate, p + 11, 2, t.minute) ||
                !parse_digits(candidate, p + 13, 2, t.second))
            {
                return std::nullopt;
            }

            if ((t.month < 1) || (t.month > 12) || (t.day < 1) || (t.day > 31) || (t.hour > 23) || (t.minute > 59) || (t.second > 60)) {
                return std::nullopt;
            }

            return t;
        }

        std::vector<std::string> backups_newest_first(const std::string &save_file_name, const std::vector<std::string> &names) {
            std::vector<std::pair<int64_t, std::string>> dated;
            for (const std::string &name : names) {
                if (std::optional<Timestamp> t = parse_backup_file_name(save_file_name, name)) {
                    dated.emplace_back(t->sort_key(), name);
                }
            }

            // Newest first; equal times by name so the order is stable.
            std::sort(dated.begin(), dated.end(), [](const auto &a, const auto &b) {
                return (a.first != b.first) ? (a.first > b.first) : (a.second > b.second);
            });

            std::vector<std::string> out;
            for (auto &d : dated) {
                out.push_back(std::move(d.second));
            }

            return out;
        }

        std::vector<std::string> backups_to_delete(const std::string &save_file_name, const std::vector<std::string> &names, size_t keep) {
            std::vector<std::string> sorted = backups_newest_first(save_file_name, names);
            if (sorted.size() <= keep) {
                return {};
            }

            // Oldest first.
            std::vector<std::string> out(sorted.begin() + keep, sorted.end());
            std::reverse(out.begin(), out.end());
            return out;
        }

        bool backup_due(BackupReason reason, const std::optional<Timestamp> &newest_backup, const Timestamp &now) {
            if (reason != BackupReason::Change) {
                return true;
            }

            if (!newest_backup.has_value()) {
                return true;
            }

            // A clock set back (newest backup "in the future") counts as old enough: never stop backing up.
            const int64_t age = seconds_between(*newest_backup, now);
            return (age < 0) || (age >= MinChangeBackupIntervalSeconds);
        }

        std::string display_time(const Timestamp &when) {
            char text[32];
            snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d:%02d", when.year, when.month, when.day, when.hour, when.minute, when.second);
            return text;
        }
    }
}
