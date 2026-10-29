//
// librecomp
//

// [wcw] Unit tests for the migration to the package registry (aki-roadmap#154 step 3).
// Built and run as part of every librecomp build (LIBRECOMP_WCW_TESTS); a failure fails the build.

#include "librecomp/packages.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "json/json.hpp"
#include "miniz.h"
#include "miniz_zip.h"
#include "wcw_test.h"

using namespace recomp::packages;
namespace fs = std::filesystem;

namespace {
    void write(const fs::path &p, const std::string &data) {
        fs::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f << data;
    }

    std::string read(const fs::path &p) {
        std::ifstream f(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    struct TempDir {
        fs::path path;
        TempDir() {
            static int n = 0;
            path = fs::temp_directory_path() / ("wcw_pkg_migrate_test_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(n++));
            fs::create_directories(path);
        }
        ~TempDir() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    void make_zip(const fs::path &zip, const std::map<std::string, std::string> &entries) {
        fs::create_directories(zip.parent_path());
        mz_zip_archive a{};
        mz_zip_writer_init_file(&a, zip.string().c_str(), 0);
        for (const auto &[name, data] : entries) {
            mz_zip_writer_add_mem(&a, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION);
        }
        mz_zip_writer_finalize_archive(&a);
        mz_zip_writer_end(&a);
    }

    std::string mod_json(const std::string &id, const std::string &version) {
        return "{\"id\": \"" + id + "\", \"display_name\": \"" + id + " name\", \"version\": \"" + version + "\", \"authors\": [\"A\"], \"game_id\": \"wwfnomercy\"}";
    }

    MigrateOptions opts() {
        MigrateOptions o;
        o.game_id = "wwfnomercy";
        o.rom_sha1 = "0123456789abcdef0123456789abcdef01234567";
        o.now = "2026-10-29T12:00:00Z";
        return o;
    }

    // A data folder as players have it today.
    void populate(const fs::path &data) {
        make_zip(data / "mods" / "aki_camera.nrm", { { "mod.json", mod_json("aki_camera", "0.7.0") }, { "mod_binary.bin", "code" } });
        make_zip(data / "mods" / "nm_unlocked.nrm", { { "mod.json", mod_json("nm_unlocked", "1.0.0") }, { "patch.bps", "bps" } });
        make_zip(data / "mods" / "nmp_hack.nrm", { { "mod.json", mod_json("nmp_hack", "2.1.0") }, { "patch.bps", "bps2" } });
        make_zip(data / "mods" / "Bad_Id.nrm", { { "mod.json", mod_json("Bad_Id", "1.0.0") }, { "mod_binary.bin", "code" } });
        make_zip(data / "mods" / "arena.rtz", { { "rt64.json", "{}" } });
        write(data / "mods" / "loose_pack" / "rt64.json", "{}");
        write(data / "mods" / "models" / "q-robot.akimodel" / "pack.json", "{\"id\": \"q-robot\", \"version\": \"1.3.0\", \"name\": \"Robot\"}");
        write(data / "mods" / "models" / "mine.akimodel" / "pack.json", "{\"id\": \"mine\"}");
        write(data / "mods" / "models" / ".bundled", "q-robot.akimodel\n");
        write(data / "conversions" / "aew64" / "aki.json", "{\"format\": \"aki-conversion\", \"format_version\": 1, \"id\": \"aew64\", \"name\": \"AEW 64\", \"version\": \"1.0.0\"}");
        write(data / "conversions" / "_packages" / "shared.rtz", "x");
        write(data / "saves" / "nmp_hack" / "slot.sram", "save");
        write(data / "mods.json", "{\"enabled_mods\": [\"aki_camera\", \"nmp_hack\", \"nm_unlocked\"], \"mod_order\": [\"nmp_hack\", \"aki_camera\", \"nm_unlocked\"]}");
    }
}

TEST_CASE("migrate: patch hacks become conversions, the first enabled one is chosen") {
    TempDir t;
    const fs::path data = t.path / "data";
    populate(data);
    const MigrateReport r = migrate(data, opts());
    CHECK(r.ran);
    CHECK(r.moved == (std::vector<std::string>{ "nm_unlocked", "nmp_hack" }));
    CHECK(!fs::exists(data / "mods" / "nmp_hack.nrm"));
    CHECK(fs::exists(data / "conversions" / "nmp_hack" / "patch.nrm"));
    const nlohmann::json aki = nlohmann::json::parse(read(data / "conversions" / "nmp_hack" / "aki.json"));
    CHECK(aki["format"] == "aki-conversion" && aki["format_version"] == 1 && aki["id"] == "nmp_hack");
    CHECK(aki["game"] == "nomercy" && aki["version"] == "2.1.0");
    CHECK(aki["components"]["patch"]["file"] == "patch.nrm");
    CHECK(aki["base_rom"]["sha1"] == "0123456789ABCDEF0123456789ABCDEF01234567");
    CHECK(r.chosen == "nmp_hack");                              // first in mod order
    CHECK(r.also_enabled == std::vector<std::string>{ "nm_unlocked" });
    CHECK(read(data / "conversions" / "active.txt") == "nmp_hack\n");
    CHECK(read(data / "saves" / "nmp_hack" / "slot.sram") == "save");   // saves untouched, same folder
}

TEST_CASE("migrate: what is installed is recorded; bad ids stay manual; the marker stops a second run") {
    TempDir t;
    const fs::path data = t.path / "data";
    populate(data);
    const MigrateReport r = migrate(data, opts());
    Registry reg;
    CHECK(load_registry(data, reg) == RegistryStatus::Ok);
    auto kind_of = [&](const char *id) { const Entry *e = find(reg, id); return e ? std::string(kind_name(e->kind)) : std::string("-"); };
    CHECK(kind_of("aki_camera") == "addon");
    CHECK(kind_of("nmp_hack") == "conversion");
    CHECK(kind_of("nm_unlocked") == "conversion");
    CHECK(kind_of("aew64") == "conversion");
    CHECK(kind_of("arena") == "texture-pack");
    CHECK(kind_of("loose_pack") == "texture-pack");
    CHECK(kind_of("q-robot") == "model-pack");
    CHECK(kind_of("mine") == "model-pack");
    CHECK(kind_of("Bad_Id") == "-");                            // not a package id: stays a manual install
    CHECK(!find(reg, "_packages") && !find(reg, "models"));
    CHECK(find(reg, "q-robot")->source == "bundled");
    CHECK(find(reg, "mine")->source == "migrated" && find(reg, "mine")->version == "0.0.0");
    CHECK(find(reg, "aki_camera")->version == "0.7.0");
    CHECK(find(reg, "nmp_hack")->files[0].path == "conversions/nmp_hack");
    CHECK(find(reg, "aki_camera")->files[0].hash == *hash_path(data / "mods" / "aki_camera.nrm"));
    CHECK(fs::exists(data / MigrationMarker));
    CHECK(read(data / MigrationLog).find("Bad_Id") != std::string::npos);
    CHECK(migration_done(data));
    CHECK(!migrate(data, opts()).ran);                          // once per data folder
    CHECK(fs::exists(data / "mods" / "Bad_Id.nrm"));            // nothing deleted
}

TEST_CASE("migrate: a dry run changes nothing") {
    TempDir t;
    const fs::path data = t.path / "data";
    populate(data);
    MigrateOptions o = opts();
    o.dry_run = true;
    const MigrateReport r = migrate(data, o);
    CHECK(r.ran);
    CHECK(r.moved.size() == 2);
    CHECK(std::find(r.recorded.begin(), r.recorded.end(), "nmp_hack") != r.recorded.end());
    CHECK(fs::exists(data / "mods" / "nmp_hack.nrm"));
    CHECK(!fs::exists(data / "conversions" / "nmp_hack"));
    CHECK(!fs::exists(data / RegistryFile));
    CHECK(!fs::exists(data / MigrationMarker));
    CHECK(!fs::exists(data / "conversions" / "active.txt"));
}

TEST_CASE("migrate: an existing choice, a taken id and a newer registry are respected") {
    TempDir t;
    const fs::path data = t.path / "data";
    populate(data);
    write(data / "conversions" / "active.txt", "aew64\n");
    write(data / "conversions" / "nm_unlocked" / "aki.json", "{\"format\": \"aki-conversion\", \"format_version\": 1, \"id\": \"nm_unlocked\"}");
    const MigrateReport r = migrate(data, opts());
    CHECK(read(data / "conversions" / "active.txt") == "aew64\n");   // the player's choice stays
    CHECK(r.chosen.empty());
    CHECK(fs::exists(data / "mods" / "nm_unlocked.nrm"));           // its conversion id is taken: left alone
    CHECK(std::find(r.moved.begin(), r.moved.end(), "nm_unlocked") == r.moved.end());

    TempDir t2;
    const fs::path d2 = t2.path / "data";
    populate(d2);
    write(d2 / RegistryFile, "{\"format\": 2, \"packages\": []}");
    CHECK(!migrate(d2, opts()).ran);
    CHECK(fs::exists(d2 / "mods" / "nmp_hack.nrm"));
    CHECK(!fs::exists(d2 / MigrationMarker));
}

TEST_CASE("migrate: an empty data folder just gets the marker") {
    TempDir t;
    const MigrateReport r = migrate(t.path, opts());
    CHECK(r.ran && r.recorded.empty() && r.moved.empty());
    CHECK(migration_done(t.path));
}
