#ifndef __RECOMP_SAVE_BACKUP_RULES_HPP__
#define __RECOMP_SAVE_BACKUP_RULES_HPP__

// [wcw saves] Pure rules for automatic save backups (aki-roadmap#50): backup file names, which backups to keep, when a
// backup is due. No file I/O here (that is save_backups.cpp), so they are unit tested
// (librecomp/tests/save_backup_rules_tests.cpp, run on every build).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace recomp {
    namespace save_backups {
        // How many backups are kept per save file (the oldest beyond this are deleted).
        constexpr size_t MaxBackupsPerFile = 10;
        // A change-triggered backup is taken at most this often per save file (a launch backup always is).
        constexpr int64_t MinChangeBackupIntervalSeconds = 10 * 60;

        // A local date and time to the second, as written in backup names.
        struct Timestamp {
            int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
            bool operator==(const Timestamp &other) const = default;
            // Seconds since 1970-01-01 00:00:00 of the same (local) clock, proleptic Gregorian: orders timestamps and
            // gives exact differences.
            int64_t sort_key() const;
        };

        // Seconds between two timestamps (b - a), exact for any two dates.
        int64_t seconds_between(const Timestamp &a, const Timestamp &b);

        // "wwf.nomercy.us.bin" + 2026-10-14 03:15:02 -> "wwf.nomercy.us.20261014-031502.bin"; the stem is the save
        // file name without its final ".bin" (".pak.bin" files keep ".pak" in the stem, so they never collide with the
        // main save's backups).
        std::string backup_file_name(const std::string &save_file_name, const Timestamp &when);

        // The timestamp of a backup of save_file_name, if candidate is one; nullopt for anything else (another save
        // file's backups, the pak file's backups, stray files, malformed dates).
        std::optional<Timestamp> parse_backup_file_name(const std::string &save_file_name, const std::string &candidate);

        // Backups to delete so at most keep remain: the oldest first. names are backup file names of one save file.
        std::vector<std::string> backups_to_delete(const std::string &save_file_name, const std::vector<std::string> &names, size_t keep);

        // Backups of one save file, newest first (what the restore menu lists).
        std::vector<std::string> backups_newest_first(const std::string &save_file_name, const std::vector<std::string> &names);

        enum class BackupReason { Launch, Change, BeforeRestore };

        // Whether a backup is due now. Launch and BeforeRestore: always (when the save file exists). Change: when there
        // is no backup yet, or the newest one is at least MinChangeBackupIntervalSeconds old (a game that saves
        // constantly would otherwise rotate every backup out within minutes).
        bool backup_due(BackupReason reason, const std::optional<Timestamp> &newest_backup, const Timestamp &now);

        // "2026-10-14 03:15:02" for menus.
        std::string display_time(const Timestamp &when);
    }
}

#endif
