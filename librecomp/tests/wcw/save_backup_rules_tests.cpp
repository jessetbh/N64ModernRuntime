//
// librecomp
//

// [wcw] Unit tests for librecomp/save_backup_rules (automatic save backups, aki-roadmap#50). Built and run as part of
// every librecomp build (LIBRECOMP_WCW_TESTS); a failure fails the build.

#include "librecomp/save_backup_rules.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "wcw_test.h"

using namespace recomp::save_backups;

static Timestamp at(int y, int mo, int d, int h, int mi, int s) {
    return Timestamp{ y, mo, d, h, mi, s };
}

// Names

TEST_CASE("names: a backup is the save's name with a sortable timestamp before .bin") {
    CHECK(backup_file_name("wwf.nomercy.us.bin", at(2026, 10, 14, 3, 15, 2)) == "wwf.nomercy.us.20261014-031502.bin");
    CHECK(backup_file_name("wwf.nomercy.us.pak.bin", at(2026, 1, 2, 23, 59, 59)) == "wwf.nomercy.us.pak.20260102-235959.bin");
}

TEST_CASE("names: a backup name parses back to its timestamp") {
    const Timestamp t = at(2026, 10, 14, 3, 15, 2);
    const auto parsed = parse_backup_file_name("wwf.nomercy.us.bin", backup_file_name("wwf.nomercy.us.bin", t));
    CHECK(parsed.has_value());
    CHECK(parsed.has_value() && (*parsed == t));
}

TEST_CASE("names: the main save and its pak file never claim each other's backups") {
    const std::string mainBackup = backup_file_name("wcw.bin", at(2026, 10, 14, 3, 15, 2));
    const std::string pakBackup = backup_file_name("wcw.pak.bin", at(2026, 10, 14, 3, 15, 2));
    CHECK(!parse_backup_file_name("wcw.bin", pakBackup).has_value());
    CHECK(!parse_backup_file_name("wcw.pak.bin", mainBackup).has_value());
    CHECK(parse_backup_file_name("wcw.pak.bin", pakBackup).has_value());
}

TEST_CASE("names: other files are not backups (no restore of stray or malformed files)") {
    const std::string save = "vpw.vpw2.jp.bin";
    CHECK(!parse_backup_file_name(save, "vpw.vpw2.jp.bin").has_value());
    CHECK(!parse_backup_file_name(save, "vpw.vpw2.jp.bin.bak").has_value());
    CHECK(!parse_backup_file_name(save, "restore.txt").has_value());
    CHECK(!parse_backup_file_name(save, "vpw.vpw2.jp.2026101-031502.bin").has_value());
    CHECK(!parse_backup_file_name(save, "vpw.vpw2.jp.20261014_031502.bin").has_value());
    CHECK(!parse_backup_file_name(save, "vpw.vpw2.jp.20261314-031502.bin").has_value());
    CHECK(!parse_backup_file_name(save, "vpw.vpw2.jp.20261014-251502.bin").has_value());
    CHECK(!parse_backup_file_name(save, "other.20261014-031502.bin").has_value());
}

// Rotation

TEST_CASE("rotation: listing is newest first, across days and years") {
    const std::string save = "s.bin";
    const std::vector<std::string> names = {
        backup_file_name(save, at(2025, 12, 31, 23, 0, 0)),
        backup_file_name(save, at(2026, 1, 1, 0, 0, 1)),
        "s.bin", "junk.txt",
        backup_file_name(save, at(2026, 1, 1, 0, 0, 0)),
    };
    const std::vector<std::string> sorted = backups_newest_first(save, names);
    CHECK_EQ(sorted.size(), size_t(3));
    CHECK(sorted[0] == backup_file_name(save, at(2026, 1, 1, 0, 0, 1)));
    CHECK(sorted[2] == backup_file_name(save, at(2025, 12, 31, 23, 0, 0)));
}

TEST_CASE("rotation: the oldest beyond the limit are deleted, oldest first") {
    const std::string save = "s.bin";
    std::vector<std::string> names;
    for (int m = 0; m < 13; m++) {
        names.push_back(backup_file_name(save, at(2026, 10, 14, 12, m, 0)));
    }
    std::reverse(names.begin(), names.end());

    const std::vector<std::string> doomed = backups_to_delete(save, names, MaxBackupsPerFile);
    CHECK_EQ(doomed.size(), size_t(3));
    CHECK(doomed[0] == backup_file_name(save, at(2026, 10, 14, 12, 0, 0)));
    CHECK(doomed[2] == backup_file_name(save, at(2026, 10, 14, 12, 2, 0)));
}

TEST_CASE("rotation: never deletes another file's backups or anything that isn't a backup") {
    const std::string save = "s.bin";
    std::vector<std::string> names = { "s.pak.20200101-000000.bin", "s.bin", "notes.txt" };
    for (int m = 0; m < 12; m++) {
        names.push_back(backup_file_name(save, at(2026, 10, 14, 12, m, 0)));
    }
    for (const std::string &name : backups_to_delete(save, names, MaxBackupsPerFile)) {
        CHECK(parse_backup_file_name(save, name).has_value());
    }
    CHECK(backups_to_delete(save, { "s.bin", "notes.txt" }, MaxBackupsPerFile).empty());
}

// When a backup is due

TEST_CASE("due: launch and before-restore backups are always taken") {
    const Timestamp now = at(2026, 10, 14, 3, 0, 0);
    CHECK(backup_due(BackupReason::Launch, now, now));
    CHECK(backup_due(BackupReason::BeforeRestore, now, now));
    CHECK(backup_due(BackupReason::Launch, std::nullopt, now));
}

TEST_CASE("due: change backups are rate-limited so a game that saves constantly can't rotate out every backup") {
    const Timestamp now = at(2026, 10, 14, 3, 30, 0);
    CHECK(backup_due(BackupReason::Change, std::nullopt, now));
    CHECK(!backup_due(BackupReason::Change, at(2026, 10, 14, 3, 25, 0), now));
    CHECK(!backup_due(BackupReason::Change, at(2026, 10, 14, 3, 20, 1), now));
    CHECK(backup_due(BackupReason::Change, at(2026, 10, 14, 3, 20, 0), now));
    // Across midnight and month ends.
    CHECK(backup_due(BackupReason::Change, at(2026, 9, 30, 23, 55, 0), at(2026, 10, 1, 0, 5, 0)));
    CHECK(!backup_due(BackupReason::Change, at(2026, 9, 30, 23, 55, 0), at(2026, 10, 1, 0, 4, 59)));
}

TEST_CASE("due: a clock set back never stops change backups") {
    CHECK(backup_due(BackupReason::Change, at(2027, 1, 1, 0, 0, 0), at(2026, 10, 14, 3, 0, 0)));
}

TEST_CASE("time: exact differences, including leap days") {
    CHECK_EQ(seconds_between(at(2024, 2, 28, 0, 0, 0), at(2024, 3, 1, 0, 0, 0)), int64_t(2 * 86400));
    CHECK_EQ(seconds_between(at(2026, 2, 28, 0, 0, 0), at(2026, 3, 1, 0, 0, 0)), int64_t(86400));
    CHECK_EQ(at(1970, 1, 1, 0, 0, 1).sort_key(), int64_t(1));
    CHECK(display_time(at(2026, 10, 14, 3, 15, 2)) == "2026-10-14 03:15:02");
}
