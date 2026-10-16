#include "librecomp/save_backups.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <system_error>

namespace recomp {
    namespace save_backups {
        // Backups (and the pending-restore note) of all files in one save folder.
        static std::mutex backups_mutex;
        static const char *RestoreNoteName = "restore.txt";

        static std::filesystem::path backups_folder(const std::filesystem::path &save_file) {
            return save_file.parent_path() / "backups";
        }

        static std::filesystem::path pak_file_of(const std::filesystem::path &main_save) {
            std::filesystem::path p = main_save;
            p.replace_extension(".pak.bin");
            return p;
        }

        static Timestamp from_time_t(std::time_t t) {
            std::tm local{};
#ifdef _WIN32
            localtime_s(&local, &t);
#else
            localtime_r(&t, &local);
#endif
            return Timestamp{ local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec };
        }

        Timestamp now() {
            return from_time_t(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
        }

        static std::vector<std::string> backup_names_in(const std::filesystem::path &folder) {
            std::vector<std::string> names;
            std::error_code ec;
            for (std::filesystem::directory_iterator it(folder, ec), end; !ec && (it != end); it.increment(ec)) {
                if (it->is_regular_file(ec)) {
                    names.push_back(it->path().filename().string());
                }
            }

            return names;
        }

        bool backup_save_file(const std::filesystem::path &save_file, BackupReason reason) {
            std::lock_guard lock{ backups_mutex };
            std::error_code ec;
            if (save_file.empty() || !std::filesystem::is_regular_file(save_file, ec)) {
                return false;
            }

            const std::filesystem::path folder = backups_folder(save_file);
            const std::string save_name = save_file.filename().string();
            const std::vector<std::string> existing = backup_names_in(folder);
            const std::vector<std::string> newest = backups_newest_first(save_name, existing);
            std::optional<Timestamp> newest_time;
            if (!newest.empty()) {
                newest_time = parse_backup_file_name(save_name, newest.front());
            }

            const Timestamp when = now();
            if (!backup_due(reason, newest_time, when)) {
                return false;
            }

            std::filesystem::create_directories(folder, ec);
            const std::filesystem::path target = folder / backup_file_name(save_name, when);
            std::filesystem::copy_file(save_file, target, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                fprintf(stderr, "[saves] backup of %s failed: %s\n", save_name.c_str(), ec.message().c_str());
                return false;
            }

            std::vector<std::string> names = existing;
            names.push_back(target.filename().string());
            for (const std::string &old : backups_to_delete(save_name, names, MaxBackupsPerFile)) {
                std::filesystem::remove(folder / old, ec);
            }

            fprintf(stderr, "[saves] backed up %s -> backups/%s\n", save_name.c_str(), target.filename().string().c_str());
            return true;
        }

        std::vector<BackupEntry> list_backups(const std::filesystem::path &main_save) {
            std::lock_guard lock{ backups_mutex };
            std::vector<BackupEntry> out;
            if (main_save.empty()) {
                return out;
            }

            const std::filesystem::path folder = backups_folder(main_save);
            const std::vector<std::string> names = backup_names_in(folder);
            for (const std::filesystem::path &save_file : { main_save, pak_file_of(main_save) }) {
                const std::string save_name = save_file.filename().string();
                for (const std::string &name : backups_newest_first(save_name, names)) {
                    out.push_back(BackupEntry{ save_file, folder / name, *parse_backup_file_name(save_name, name), save_file != main_save });
                }
            }

            std::stable_sort(out.begin(), out.end(), [](const BackupEntry &a, const BackupEntry &b) {
                return a.when.sort_key() > b.when.sort_key();
            });

            return out;
        }

        std::optional<Timestamp> last_write_time(const std::filesystem::path &save_file) {
            std::error_code ec;
            const auto ftime = std::filesystem::last_write_time(save_file, ec);
            if (ec) {
                return std::nullopt;
            }

            const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
            return from_time_t(std::chrono::system_clock::to_time_t(sys));
        }

        // The note holds two lines: the save file's name and the backup's name (both in the save's folder tree).
        bool request_restore(const BackupEntry &entry) {
            std::lock_guard lock{ backups_mutex };
            std::error_code ec;
            const std::filesystem::path folder = backups_folder(entry.save_file);
            std::filesystem::create_directories(folder, ec);
            std::ofstream note(folder / RestoreNoteName, std::ios::trunc);
            if (!note.good()) {
                return false;
            }

            note << entry.save_file.filename().string() << "\n" << entry.backup_file.filename().string() << "\n";
            fprintf(stderr, "[saves] restore of %s scheduled for the next start\n", entry.backup_file.filename().string().c_str());
            return note.good();
        }

        std::optional<BackupEntry> pending_restore(const std::filesystem::path &main_save) {
            if (main_save.empty()) {
                return std::nullopt;
            }

            const std::filesystem::path folder = backups_folder(main_save);
            std::ifstream note(folder / RestoreNoteName);
            std::string save_name, backup_name;
            if (!note.good() || !std::getline(note, save_name) || !std::getline(note, backup_name)) {
                return std::nullopt;
            }

            // Only files of this game's save folder, named like its own backups.
            const std::filesystem::path save_file = main_save.parent_path() / save_name;
            if ((save_file != main_save) && (save_file != pak_file_of(main_save))) {
                return std::nullopt;
            }

            const std::optional<Timestamp> when = parse_backup_file_name(save_name, backup_name);
            if (!when.has_value()) {
                return std::nullopt;
            }

            return BackupEntry{ save_file, folder / backup_name, *when, save_file != main_save };
        }

        void cancel_restore(const std::filesystem::path &main_save) {
            std::lock_guard lock{ backups_mutex };
            std::error_code ec;
            std::filesystem::remove(backups_folder(main_save) / RestoreNoteName, ec);
        }

        // Returns the file it restored (empty if none).
        static std::filesystem::path apply_pending_restore(const std::filesystem::path &main_save) {
            const std::optional<BackupEntry> entry = pending_restore(main_save);
            cancel_restore(main_save);
            if (!entry.has_value()) {
                return {};
            }

            std::error_code ec;
            if (!std::filesystem::is_regular_file(entry->backup_file, ec)) {
                fprintf(stderr, "[saves] restore skipped: %s is gone\n", entry->backup_file.filename().string().c_str());
                return {};
            }

            // Keep what's there now, so the restore can be undone from the same menu.
            backup_save_file(entry->save_file, BackupReason::BeforeRestore);
            std::filesystem::copy_file(entry->backup_file, entry->save_file, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                fprintf(stderr, "[saves] restore of %s failed: %s\n", entry->backup_file.filename().string().c_str(), ec.message().c_str());
                return {};
            }

            fprintf(stderr, "[saves] restored %s from backups/%s\n", entry->save_file.filename().string().c_str(), entry->backup_file.filename().string().c_str());
            return entry->save_file;
        }

        void on_game_start(const std::filesystem::path &main_save) {
            if (main_save.empty()) {
                return;
            }

            // A just-restored file is already a backup, and its before-restore backup was taken this second: a launch
            // backup would get the same name and overwrite it.
            const std::filesystem::path restored = apply_pending_restore(main_save);
            for (const std::filesystem::path &save_file : { main_save, pak_file_of(main_save) }) {
                if (save_file != restored) {
                    backup_save_file(save_file, BackupReason::Launch);
                }
            }
        }
    }
}
