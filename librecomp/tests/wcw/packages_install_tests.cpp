//
// librecomp
//

// [wcw] Unit tests for installing, upgrading and removing packages and the drop folder (aki-roadmap#154 step 2).
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
            path = fs::temp_directory_path() / ("wcw_pkg_install_test_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(n++));
            fs::create_directories(path);
        }
        ~TempDir() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    const char *Rom = "0123456789abcdef0123456789abcdef01234567";
    const char *OtherRom = "fedcba9876543210fedcba9876543210fedcba98";

    std::string manifest_json(const std::string &kind, const std::string &id, const std::string &version, const std::string &payload,
        const std::string &extra = "") {
        return "{\"format\": 1, \"kind\": \"" + kind + "\", \"id\": \"" + id + "\", \"version\": \"" + version + "\", "
            "\"game_id\": \"wwfnomercy\", \"display_name\": \"" + id + " pack\", \"authors\": [\"A\"], \"payload\": " + payload + extra + "}";
    }

    // A package folder: aki-package.json plus files.
    fs::path make_package(const fs::path &dir, const std::string &manifest, const std::map<std::string, std::string> &files) {
        write(dir / ManifestFile, manifest);
        for (const auto &[rel, data] : files) {
            write(dir / rel, data);
        }
        return dir;
    }

    bool make_zip(const fs::path &zip, const std::map<std::string, std::string> &entries, mz_uint flags = 0) {
        mz_zip_archive a{};
        if (!mz_zip_writer_init_file(&a, zip.string().c_str(), 0)) {
            return false;
        }
        for (const auto &[name, data] : entries) {
            mz_zip_writer_add_mem(&a, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION | flags);
        }
        const bool ok = mz_zip_writer_finalize_archive(&a);
        mz_zip_writer_end(&a);
        return ok;
    }

    InstallOptions opts() {
        InstallOptions o;
        o.game_id = "wwfnomercy";
        o.rom_sha1 = Rom;
        o.recomp_version = "1.2.0";
        o.date = "20261029";
        o.now = "2026-10-29T12:00:00Z";
        return o;
    }

    Manifest parsed(const std::string &text) {
        Manifest m;
        std::vector<Problem> p;
        parse_manifest(text, m, p);
        return m;
    }
}

TEST_CASE("install: placements by kind") {
    const auto addon = placements(parsed(manifest_json("addon", "cam", "1.0.0", "{\"nrm\": \"bin/aki_camera.nrm\"}")));
    CHECK(addon.size() == 1 && addon[0].from == "bin/aki_camera.nrm" && addon[0].to == "mods/aki_camera.nrm" && !addon[0].folder);
    const auto loose = placements(parsed(manifest_json("texture-pack", "arena", "1.0.0", "{\"folder\": \"tex/\"}")));
    CHECK(loose.size() == 1 && loose[0].from == "tex" && loose[0].to == "mods/arena" && loose[0].folder);
    const auto model = placements(parsed(manifest_json("model-pack", "q", "1.0.0", "{\"packs\": [\"p/q-robot.akimodel/\"], \"hd\": [\"q-robot-hd.rtz\"]}")));
    CHECK(model.size() == 2 && model[0].to == "mods/models/q-robot.akimodel" && model[1].to == "mods/q-robot-hd.rtz");
    const auto conv = placements(parsed(manifest_json("conversion", "nmp", "1.0.0", "{\"folder\": \"c\", \"addons\": [\"t.rtz\"]}",
        std::string(", \"base_rom\": {\"sha1\": [\"") + Rom + "\"]}")));
    CHECK(conv.size() == 2 && conv[0].to == "conversions/nmp" && conv[0].folder && conv[1].to == "mods/t.rtz");
}

TEST_CASE("install: plan rules") {
    Registry r;
    const Manifest addon = parsed(manifest_json("addon", "cam", "1.1.0", "{\"nrm\": \"cam.nrm\"}"));
    CHECK(plan_install(r, addon, opts()).action == Action::Install);

    InstallOptions other = opts();
    other.game_id = "vpw2";
    CHECK(plan_install(r, addon, other).action == Action::Refuse);

    const Manifest needs = parsed(manifest_json("addon", "cam", "1.0.0", "{\"nrm\": \"cam.nrm\"}", ", \"requires\": {\"minimum_recomp_version\": \"1.3.0\", \"packages\": [\"aki_cheats\"]}"));
    CHECK(plan_install(r, needs, opts()).action == Action::Refuse);
    InstallOptions unknown_build = opts();
    unknown_build.recomp_version.clear();
    const Plan p = plan_install(r, needs, unknown_build);
    CHECK(p.action == Action::Install);
    CHECK(p.notes.size() == 1 && !p.notes[0].error && p.notes[0].field == "requires.packages");

    const std::string other_rom = std::string(", \"base_rom\": {\"sha1\": [\"") + OtherRom + "\"], \"name\": \"WWF No Mercy (Rev 0)\"}";
    CHECK(plan_install(r, parsed(manifest_json("conversion", "nmp", "1.0.0", "{\"folder\": \"c\"}", other_rom)), opts()).action == Action::Refuse);
    const Plan warned = plan_install(r, parsed(manifest_json("texture-pack", "t", "1.0.0", "{\"rtz\": \"t.rtz\"}", other_rom)), opts());
    CHECK(warned.action == Action::Install && warned.notes.size() == 1 && warned.notes[0].field == "base_rom");
    InstallOptions no_rom = opts();
    no_rom.rom_sha1.clear();
    CHECK(plan_install(r, parsed(manifest_json("conversion", "nmp", "1.0.0", "{\"folder\": \"c\"}", other_rom)), no_rom).action == Action::Refuse);
    const Plan unchecked = plan_install(r, parsed(manifest_json("texture-pack", "t", "1.0.0", "{\"rtz\": \"t.rtz\"}", other_rom)), no_rom);
    CHECK(unchecked.action == Action::Install && unchecked.notes.empty());   // a pack doesn't need the ROM checked
    // Two parts landing on one path (different source folders, same file name; case doesn't matter).
    CHECK(plan_install(r, parsed(manifest_json("model-pack", "two", "1.0.0", "{\"packs\": [\"a/x.akimodel\", \"b/X.akimodel\"]}")), opts()).action == Action::Refuse);

    const Plan own_use = plan_install(r, parsed(manifest_json("addon", "cam", "1.0.0", "{\"nrm\": \"cam.nrm\"}", ", \"permission\": {\"redistribute\": \"no\"}")), opts());
    CHECK(own_use.notes.size() == 1 && own_use.notes[0].field == "permission");

    Entry e;
    e.id = "cam";
    e.kind = Kind::Addon;
    e.version = "1.0.0";
    e.files = { { "mods/cam.nrm", 1 } };
    upsert(r, e);
    CHECK(plan_install(r, addon, opts()).action == Action::Upgrade);
    CHECK(plan_install(r, addon, opts()).old_version == "1.0.0");
    const Manifest same = parsed(manifest_json("addon", "cam", "1.0.0", "{\"nrm\": \"cam.nrm\"}"));
    CHECK(plan_install(r, same, opts()).action == Action::AlreadyInstalled);
    InstallOptions again = opts();
    again.reinstall = true;
    CHECK(plan_install(r, same, again).action == Action::Reinstall);
    const Manifest older = parsed(manifest_json("addon", "cam", "0.9.0", "{\"nrm\": \"cam.nrm\"}"));
    CHECK(plan_install(r, older, opts()).action == Action::Refuse);
    InstallOptions down = opts();
    down.allow_downgrade = true;
    CHECK(plan_install(r, older, down).action == Action::Downgrade);

    // The id is taken by another kind; a path another package owns; "replaces" frees it.
    CHECK(plan_install(r, parsed(manifest_json("texture-pack", "cam", "1.0.0", "{\"rtz\": \"cam.rtz\"}")), opts()).action == Action::Refuse);
    const Manifest takes = parsed(manifest_json("addon", "camera2", "1.0.0", "{\"nrm\": \"cam.nrm\"}"));
    CHECK(plan_install(r, takes, opts()).action == Action::Refuse);
    const Manifest renames = parsed(manifest_json("addon", "camera2", "1.0.0", "{\"nrm\": \"cam.nrm\"}", ", \"replaces\": [\"cam\", \"gone\"]"));
    const Plan rp = plan_install(r, renames, opts());
    CHECK(rp.action == Action::Install);
    CHECK(rp.replaced == std::vector<std::string>{ "cam" });
}

TEST_CASE("install: edited names count up") {
    std::vector<std::string> taken = { "mods/a.nrm.edited-20261029", "mods/a.nrm.edited-20261029-2" };
    auto is_taken = [&](const std::string &n) { return std::find(taken.begin(), taken.end(), n) != taken.end(); };
    CHECK(edited_name("mods/a.nrm", "20261029", is_taken) == "mods/a.nrm.edited-20261029-3");
    CHECK(edited_name("conversions/x/", "20261029", is_taken) == "conversions/x.edited-20261029");
}

TEST_CASE("install: install, already installed, reinstall") {
    TempDir t;
    const fs::path data = t.path / "data";
    const fs::path pkg = make_package(t.path / "pkg", manifest_json("addon", "cam", "1.0.0", "{\"nrm\": \"cam.nrm\"}"), { { "cam.nrm", "v1" } });
    InstallResult r = install(pkg, data, opts());
    CHECK(r.outcome == Outcome::Installed);
    CHECK(read(data / "mods" / "cam.nrm") == "v1");
    Registry reg;
    CHECK(load_registry(data, reg) == RegistryStatus::Ok);
    const Entry *e = find(reg, "cam");
    CHECK(e && e->version == "1.0.0" && e->source == "launcher" && e->installed == "2026-10-29T12:00:00Z");
    CHECK(e && e->files.size() == 1 && e->files[0].path == "mods/cam.nrm" && e->files[0].hash == hash_bytes("cam.nrm", "v1"));
    CHECK(!fs::exists(data / "mods" / "cam.nrm.installing"));
    CHECK(install(pkg, data, opts()).outcome == Outcome::AlreadyInstalled);
    InstallOptions again = opts();
    again.reinstall = true;
    write(data / "mods" / "cam.nrm", "v1");   // unedited
    CHECK(install(pkg, data, again).outcome == Outcome::Reinstalled);
}

TEST_CASE("install: an upgrade replaces unedited files and keeps edited ones") {
    TempDir t;
    const fs::path data = t.path / "data";
    CHECK(install(make_package(t.path / "v1", manifest_json("addon", "cam", "1.0.0", "{\"nrm\": \"cam.nrm\"}"), { { "cam.nrm", "v1" } }), data, opts()).outcome == Outcome::Installed);
    InstallResult up = install(make_package(t.path / "v2", manifest_json("addon", "cam", "1.1.0", "{\"nrm\": \"cam.nrm\"}"), { { "cam.nrm", "v2" } }), data, opts());
    CHECK(up.outcome == Outcome::Upgraded);
    CHECK(up.old_version == "1.0.0");
    CHECK(up.kept.empty());
    CHECK(read(data / "mods" / "cam.nrm") == "v2");
    CHECK(!fs::exists(data / "mods" / "cam.nrm.replaced"));

    write(data / "mods" / "cam.nrm", "my edit");
    InstallResult up2 = install(make_package(t.path / "v3", manifest_json("addon", "cam", "1.2.0", "{\"nrm\": \"cam.nrm\"}"), { { "cam.nrm", "v3" } }), data, opts());
    CHECK(up2.outcome == Outcome::Upgraded);
    CHECK(read(data / "mods" / "cam.nrm") == "v3");
    CHECK(read(data / "kept" / "mods" / "cam.nrm.edited-20261029") == "my edit");   // where no mod scan sees it
    CHECK(!fs::exists(data / "mods" / "cam.nrm.edited-20261029"));
    CHECK(up2.kept == std::vector<std::string>{ "kept/mods/cam.nrm.edited-20261029" });
}

TEST_CASE("install: a hand-placed file is kept, an upgrade drops files the new version doesn't have") {
    TempDir t;
    const fs::path data = t.path / "data";
    write(data / "mods" / "models" / "q-a.akimodel" / "pack.json", "player's own");
    const std::string v1 = manifest_json("model-pack", "q", "1.0.0", "{\"packs\": [\"q-a.akimodel\", \"q-b.akimodel\"]}");
    InstallResult r = install(make_package(t.path / "v1", v1, { { "q-a.akimodel/pack.json", "a1" }, { "q-b.akimodel/pack.json", "b1" } }), data, opts());
    CHECK(r.outcome == Outcome::Installed);
    CHECK(read(data / "kept" / "mods" / "models" / "q-a.akimodel.edited-20261029" / "pack.json") == "player's own");
    CHECK(read(data / "mods" / "models" / "q-a.akimodel" / "pack.json") == "a1");
    const std::string v2 = manifest_json("model-pack", "q", "2.0.0", "{\"packs\": [\"q-a.akimodel\"]}");
    CHECK(install(make_package(t.path / "v2", v2, { { "q-a.akimodel/pack.json", "a2" } }), data, opts()).outcome == Outcome::Upgraded);
    CHECK(!fs::exists(data / "mods" / "models" / "q-b.akimodel"));      // unedited, no longer part of it
    CHECK(read(data / "mods" / "models" / "q-a.akimodel" / "pack.json") == "a2");
}

TEST_CASE("install: a conversion in a subfolder of a zip; replaces removes the old package") {
    TempDir t;
    const fs::path data = t.path / "data";
    CHECK(install(make_package(t.path / "old", manifest_json("addon", "nmp_patch", "1.0.0", "{\"nrm\": \"nmp.nrm\"}"), { { "nmp.nrm", "patch" } }), data, opts()).outcome == Outcome::Installed);
    const std::string conv = manifest_json("conversion", "nmp", "3.0.0", "{\"folder\": \"conversion\", \"addons\": [\"nmp_tex.rtz\"]}",
        std::string(", \"base_rom\": {\"sha1\": [\"") + Rom + "\"]}, \"replaces\": [\"nmp_patch\"]");
    const fs::path zip = t.path / "nmp.zip";
    CHECK(make_zip(zip, { { "NMP v3/aki-package.json", conv }, { "NMP v3/conversion/aki.json", "{}" },
        { "NMP v3/conversion/patch.nrm", "bps" }, { "NMP v3/nmp_tex.rtz", "tex" } }));
    InstallResult r = install(zip, data, opts());
    CHECK(r.outcome == Outcome::Installed);
    CHECK(read(data / "conversions" / "nmp" / "patch.nrm") == "bps");
    CHECK(read(data / "mods" / "nmp_tex.rtz") == "tex");
    CHECK(!fs::exists(data / "mods" / "nmp.nrm"));      // the replaced package's unedited file
    Registry reg;
    load_registry(data, reg);
    CHECK(!find(reg, "nmp_patch"));
    CHECK(find(reg, "nmp") && find(reg, "nmp")->files.size() == 2);
    CHECK(!fs::exists(data / ".aki-work"));             // the unpack folder is gone
}

TEST_CASE("install: an unsafe zip, a broken manifest or a missing payload changes nothing") {
    TempDir t;
    const fs::path data = t.path / "data";
    const fs::path zip = t.path / "evil.zip";
    CHECK(make_zip(zip, { { "aki-package.json", manifest_json("addon", "evil", "1.0.0", "{\"nrm\": \"e.nrm\"}") }, { "../e.nrm", "x" } }));
    InstallResult evil = install(zip, data, opts());
    CHECK(evil.outcome == Outcome::Refused);
    CHECK(evil.message.find("unsafe path") != std::string::npos);   // refused for the path, not for anything else
    CHECK(!fs::exists(data / "e.nrm") && !fs::exists(data / ".aki-work"));
    const fs::path bad = make_package(t.path / "bad", "{\"format\": 1}", {});
    InstallResult r = install(bad, data, opts());
    CHECK(r.outcome == Outcome::Refused);
    CHECK(!r.problems.empty());
    CHECK(install(make_package(t.path / "missing", manifest_json("addon", "m", "1.0.0", "{\"nrm\": \"m.nrm\"}"), {}), data, opts()).outcome == Outcome::Refused);
    CHECK(!fs::exists(data / RegistryFile));
    CHECK(!fs::exists(data / "mods"));
}

TEST_CASE("install: a newer registry is never overwritten") {
    TempDir t;
    const fs::path data = t.path / "data";
    write(data / RegistryFile, "{\"format\": 2, \"packages\": []}");
    CHECK(install(make_package(t.path / "p", manifest_json("addon", "cam", "1.0.0", "{\"nrm\": \"cam.nrm\"}"), { { "cam.nrm", "v1" } }), data, opts()).outcome == Outcome::Refused);
    CHECK(read(data / RegistryFile) == "{\"format\": 2, \"packages\": []}");
    CHECK(!fs::exists(data / "mods" / "cam.nrm"));
}

TEST_CASE("install: uninstall removes unedited files, keeps edited ones, never saves") {
    TempDir t;
    const fs::path data = t.path / "data";
    const std::string m = manifest_json("model-pack", "q", "1.0.0", "{\"packs\": [\"q-a.akimodel\"], \"hd\": [\"q-a-hd.rtz\"]}");
    CHECK(install(make_package(t.path / "p", m, { { "q-a.akimodel/pack.json", "a" }, { "q-a-hd.rtz", "hd" } }), data, opts()).outcome == Outcome::Installed);
    write(data / "mods" / "q-a-hd.rtz", "edited");
    write(data / "saves" / "q" / "slot.sav", "save");
    RemoveResult r = uninstall("q", data);
    CHECK(r.removed);
    CHECK(!fs::exists(data / "mods" / "models" / "q-a.akimodel"));
    CHECK(read(data / "mods" / "q-a-hd.rtz") == "edited");
    CHECK(r.kept == std::vector<std::string>{ "mods/q-a-hd.rtz" });
    CHECK(read(data / "saves" / "q" / "slot.sav") == "save");
    CHECK(!uninstall("q", data).removed);
}

TEST_CASE("install: the drop folder") {
    TempDir t;
    const fs::path data = t.path / "data";
    CHECK(process_import_folder(data, opts()).empty());
    CHECK(!fs::exists(data / ImportFolder));            // nothing created without one
    make_package(data / ImportFolder / "good", manifest_json("addon", "cam", "1.0.0", "{\"nrm\": \"cam.nrm\"}"), { { "cam.nrm", "v1" } });
    make_package(data / ImportFolder / "other-game", manifest_json("addon", "x", "1.0.0", "{\"nrm\": \"x.nrm\"}"), { { "x.nrm", "x" } });
    write(data / ImportFolder / "other-game" / ManifestFile, read(data / ImportFolder / "other-game" / ManifestFile).replace(
        read(data / ImportFolder / "other-game" / ManifestFile).find("wwfnomercy"), 10, "vpw2"));
    const std::vector<InstallResult> results = process_import_folder(data, opts());
    CHECK(results.size() == 2);
    CHECK(results[0].outcome == Outcome::Installed);    // sorted: good, other-game
    CHECK(results[1].outcome == Outcome::Refused);
    CHECK(!fs::exists(data / ImportFolder / "good"));
    CHECK(fs::exists(data / ImportFolder / "not-installed" / "other-game" / ManifestFile));
    CHECK(read(data / ImportFolder / "not-installed" / "other-game.txt").find("another game") != std::string::npos);
    const std::string report = read(data / ImportFolder / "last-import.json");
    CHECK(report.find("\"installed\"") != std::string::npos && report.find("\"refused\"") != std::string::npos);
    Registry reg;
    load_registry(data, reg);
    CHECK(find(reg, "cam") && find(reg, "cam")->source == "dropped");
    CHECK(process_import_folder(data, opts()).empty());  // not-installed and the report are left alone
}

// Step 2 review findings: each fails on the first version of step 2.

TEST_CASE("install: zips with non-UTF-8 names are refused, not misread; __MACOSX is ignored") {
    TempDir t;
    const fs::path data = t.path / "data";
    const fs::path oem = t.path / "oem.zip";
    CHECK(make_zip(oem, { { "aki-package.json", manifest_json("texture-pack", "oem", "1.0.0", "{\"rtz\": \"caf\x82.rtz\"}") },
        { "caf\x82.rtz", "tex" } }, MZ_ZIP_FLAG_ASCII_FILENAME));
    InstallResult r = install(oem, data, opts());          // must not throw
    CHECK(r.outcome == Outcome::Refused);
    CHECK(r.message.find("UTF-8") != std::string::npos);
    const fs::path mac = t.path / "mac.zip";
    CHECK(make_zip(mac, { { "Pack/aki-package.json", manifest_json("addon", "cam", "1.0.0", "{\"nrm\": \"cam.nrm\"}") },
        { "Pack/cam.nrm", "v1" }, { "__MACOSX/Pack/._cam.nrm", "junk" } }));
    CHECK(install(mac, data, opts()).outcome == Outcome::Installed);
    CHECK(!fs::exists(data / "__MACOSX"));
}

TEST_CASE("install: an interrupted install is undone at the next start (journal, not committed)") {
    TempDir t;
    const fs::path data = t.path / "data";
    // State after a crash mid-swap: mods/a.nrm went aside, its new copy was placed; mods/b.rtz (new) placed;
    // mods/c.rtz still staged. The registry was never written.
    write(data / "mods" / "a.nrm.replaced", "old a");
    write(data / "mods" / "a.nrm", "new a");
    write(data / "mods" / "b.rtz", "new b");
    write(data / "mods" / "c.rtz.installing", "new c");
    write(data / ".aki-work" / "install-journal.json", "{\"format\": 1, \"committed\": false, \"delete\": [], \"swaps\": ["
        "{\"dst\": \"mods/a.nrm\", \"staged\": \"mods/a.nrm.installing\", \"aside\": \"mods/a.nrm.replaced\", \"keep_aside\": false},"
        "{\"dst\": \"mods/b.rtz\", \"staged\": \"mods/b.rtz.installing\", \"aside\": \"\", \"keep_aside\": false},"
        "{\"dst\": \"mods/c.rtz\", \"staged\": \"mods/c.rtz.installing\", \"aside\": \"\", \"keep_aside\": false}]}");
    CHECK(!recover_interrupted_install(data).empty());
    CHECK(read(data / "mods" / "a.nrm") == "old a");
    CHECK(!fs::exists(data / "mods" / "a.nrm.replaced"));
    CHECK(!fs::exists(data / "mods" / "b.rtz"));
    CHECK(!fs::exists(data / "mods" / "c.rtz.installing") && !fs::exists(data / "mods" / "c.rtz"));
    CHECK(!fs::exists(data / ".aki-work"));
    CHECK(recover_interrupted_install(data).empty());       // nothing left to do
}

TEST_CASE("install: a committed install is finished at the next start; kept copies stay") {
    TempDir t;
    const fs::path data = t.path / "data";
    write(data / "mods" / "a.nrm", "new a");
    write(data / "mods" / "a.nrm.replaced", "old a");
    write(data / "mods" / "b.rtz", "new b");
    write(data / "kept" / "mods" / "b.rtz.edited-20261029", "player's b");
    write(data / "mods" / "gone.rtz", "old version's file");
    write(data / ".aki-work" / "install-journal.json", "{\"format\": 1, \"committed\": true, \"delete\": [\"mods/gone.rtz\"], \"swaps\": ["
        "{\"dst\": \"mods/a.nrm\", \"staged\": \"mods/a.nrm.installing\", \"aside\": \"mods/a.nrm.replaced\", \"keep_aside\": false},"
        "{\"dst\": \"mods/b.rtz\", \"staged\": \"mods/b.rtz.installing\", \"aside\": \"kept/mods/b.rtz.edited-20261029\", \"keep_aside\": true}]}");
    CHECK(!recover_interrupted_install(data).empty());
    CHECK(read(data / "mods" / "a.nrm") == "new a");
    CHECK(!fs::exists(data / "mods" / "a.nrm.replaced"));
    CHECK(read(data / "kept" / "mods" / "b.rtz.edited-20261029") == "player's b");
    CHECK(!fs::exists(data / "mods" / "gone.rtz"));
    CHECK(!fs::exists(data / ".aki-work"));
}

TEST_CASE("install: a package holding a link is refused") {
    TempDir t;
    const fs::path data = t.path / "data";
    const fs::path pkg = make_package(t.path / "p", manifest_json("model-pack", "q", "1.0.0", "{\"packs\": [\"q.akimodel\"]}"), { { "q.akimodel/pack.json", "a" } });
    write(t.path / "elsewhere" / "secret.txt", "s");
    std::error_code ec;
    fs::create_directory_symlink(t.path / "elsewhere", pkg / "q.akimodel" / "link", ec);
    if (ec) {
        return;   // no symlink rights (Windows without developer mode): nothing to test here
    }
    InstallResult r = install(pkg, data, opts());
    CHECK(r.outcome == Outcome::Refused);
    CHECK(!fs::exists(data / "mods" / "models" / "q.akimodel"));
}
