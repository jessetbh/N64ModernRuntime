#ifndef __RECOMP_SAVE_BACKUPS_HPP__
#define __RECOMP_SAVE_BACKUPS_HPP__

// [wcw saves] Automatic save backups (aki-roadmap#50). Timestamped copies of the game's save files (the cartridge
// save <game>.bin and the Controller Pak image <game>.pak.bin) go to a backups/ folder next to them:
// - at launch, before the game touches them;
// - when the game writes them, at most every MinChangeBackupIntervalSeconds per file;
// - before a restore replaces one (so a restore can be undone).
// The newest MaxBackupsPerFile per file are kept. A restore picked in the menu is applied the next time the game
// starts, before it reads its saves, so the running game can't overwrite it. Rules: save_backup_rules.hpp.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "librecomp/save_backup_rules.hpp"

namespace recomp {
    namespace save_backups {
        struct BackupEntry {
            std::filesystem::path save_file;   // the file it restores (e.g. saves/wwf.nomercy.us.bin)
            std::filesystem::path backup_file; // saves/backups/wwf.nomercy.us.20261014-031502.bin
            Timestamp when;
            bool controller_pak = false;       // a .pak.bin (Controller Pak) backup
        };

        // The game's main save file: the running game's (ultramodern::get_save_file_path) once it has started, else
        // the registered game's default location (the stock saves folder). Empty if neither is known.
        std::filesystem::path main_save_file();

        // Back up save_file now if it exists and the rules say a backup is due; then delete the oldest beyond
        // MaxBackupsPerFile. Returns whether a backup was written. Errors are logged, never thrown.
        bool backup_save_file(const std::filesystem::path &save_file, BackupReason reason);

        // At game start: apply a pending restore, then take the launch backups of the main save and its pak file.
        void on_game_start(const std::filesystem::path &main_save);

        // The backups of the main save and its .pak.bin, newest first.
        std::vector<BackupEntry> list_backups(const std::filesystem::path &main_save);

        // When save_file was last written, or nullopt if it doesn't exist.
        std::optional<Timestamp> last_write_time(const std::filesystem::path &save_file);

        // Schedule entry to replace its save file the next time the game starts (one pending restore at a time; a new
        // request replaces it). Returns false if it couldn't be recorded.
        bool request_restore(const BackupEntry &entry);

        // The pending restore for main_save's folder, if any.
        std::optional<BackupEntry> pending_restore(const std::filesystem::path &main_save);

        // Cancel a pending restore.
        void cancel_restore(const std::filesystem::path &main_save);

        // Now, on the local clock.
        Timestamp now();
    }
}

#endif
