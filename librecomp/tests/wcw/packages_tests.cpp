//
// librecomp
//

// [wcw] Unit tests for librecomp/packages (aki-package.json manifests and the packages.json registry, aki-roadmap#154).
// Built and run as part of every librecomp build (LIBRECOMP_WCW_TESTS); a failure fails the build.

#include "librecomp/packages.hpp"
#include "librecomp/bundled_packs.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "wcw_test.h"

using namespace recomp::packages;
namespace fs = std::filesystem;

namespace {
    void write(const fs::path &p, const std::string &data) {
        fs::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f << data;
    }

    struct TempDir {
        fs::path path;
        TempDir() {
            path = fs::temp_directory_path() /
                ("wcw_packages_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            fs::create_directories(path);
        }
        ~TempDir() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    const char *Sha = "0123456789abcdef0123456789ABCDEF01234567";

    std::string manifest(const std::string &kind, const std::string &payload, const std::string &extra = "") {
        return std::string("{\"format\": 1, \"kind\": \"") + kind + "\", \"id\": \"nmp_v3\", \"version\": \"3.0.0\", "
            "\"game_id\": \"wwfnomercy\", \"display_name\": \"No Mercy Plus V3\", \"authors\": [\"A\"], "
            "\"payload\": " + payload + extra + "}";
    }

    bool has_error(const std::vector<Problem> &problems, const std::string &field) {
        return std::any_of(problems.begin(), problems.end(), [&](const Problem &p) { return p.error && p.field == field; });
    }

    Entry entry(const std::string &id, Kind kind, std::vector<InstalledFile> files) {
        Entry e;
        e.id = id;
        e.kind = kind;
        e.version = "1.0.0";
        e.source = "launcher";
        e.files = std::move(files);
        return e;
    }
}

TEST_CASE("packages: kind names round-trip") {
    for (Kind k : { Kind::Addon, Kind::TexturePack, Kind::ModelPack, Kind::Conversion }) {
        CHECK(parse_kind(kind_name(k)) == k);
    }
    CHECK(!parse_kind("mod"));
    CHECK(!parse_kind(""));
}

TEST_CASE("packages: ids") {
    CHECK(valid_id("nmp_v3"));
    CHECK(valid_id("a"));
    CHECK(valid_id("0day.pack-2"));
    CHECK(valid_id(std::string(48, 'a')));
    CHECK(!valid_id(std::string(49, 'a')));    // a conversion id must fit (conversion::valid_id: 48)
    CHECK(!valid_id(""));
    CHECK(!valid_id("_hidden"));
    CHECK(!valid_id(".x"));
    CHECK(!valid_id("-x"));
    CHECK(!valid_id("NMP"));
    CHECK(!valid_id("a b"));
    CHECK(!valid_id("a/b"));
}

TEST_CASE("packages: versions") {
    CHECK(valid_version("1.2.3"));
    CHECK(valid_version("10.0.0-beta.1"));
    CHECK(!valid_version("1.2"));
    CHECK(!valid_version("1.2.3."));
    CHECK(!valid_version("v1.2.3"));
    CHECK(!valid_version("1.2.3-"));
    CHECK(!valid_version("1.2.3+build"));
    CHECK(compare_versions("1.10.0", "1.9.0") > 0);
    CHECK(compare_versions("1.2.0", "1.2.0") == 0);
    CHECK(compare_versions("1.2.0-beta", "1.2.0") < 0);     // a release beats its prerelease
    CHECK(compare_versions("1.2.0", "1.2.0-rc.1") > 0);
    CHECK(compare_versions("1.2.0-alpha", "1.2.0-beta") < 0);
    CHECK(compare_versions("1.3.0-alpha", "1.2.9") > 0);
}

TEST_CASE("packages: game ids map to the short keys") {
    CHECK(game_id_for_key("nomercy") == "wwfnomercy");
    CHECK(key_for_game_id("wcwnwoworldtour") == "wt");
    CHECK(key_for_game_id("vpw2") == "vpw2");
    CHECK(game_id_for_key("wwfnomercy").empty());
    CHECK(key_for_game_id("nomercy").empty());
}

TEST_CASE("packages: safe relative paths") {
    CHECK(safe_relative_path("a.nrm"));
    CHECK(safe_relative_path("conversion/"));
    CHECK(safe_relative_path("packs/q-robot.akimodel"));
    CHECK(!safe_relative_path(""));
    CHECK(!safe_relative_path("/abs"));
    CHECK(!safe_relative_path("C:/x"));
    CHECK(!safe_relative_path("a\\b"));
    CHECK(!safe_relative_path("../x"));
    CHECK(!safe_relative_path("a/../b"));
    CHECK(!safe_relative_path("a/./b"));
    CHECK(!safe_relative_path("a//b"));
    CHECK(!safe_relative_path("a//"));
}

TEST_CASE("packages: an addon manifest parses") {
    Manifest m;
    std::vector<Problem> p;
    const std::string text = manifest("addon", "{\"nrm\": \"aki_camera.nrm\"}",
        ", \"license\": \"GPL-3.0-only\", \"requires\": {\"minimum_recomp_version\": \"1.2.0\", \"packages\": [\"aki_cheats\"]},"
        " \"permission\": {\"redistribute\": \"yes\", \"statement\": \"ok\"}, \"replaces\": [\"camera_old\"]");
    CHECK(parse_manifest(text, m, p));
    CHECK(p.empty());
    CHECK(m.kind == Kind::Addon);
    CHECK(m.id == "nmp_v3");
    CHECK(m.game_id == "wwfnomercy");
    CHECK(m.nrm == "aki_camera.nrm");
    CHECK(m.requires_recomp == "1.2.0");
    CHECK(m.requires_packages.size() == 1);
    CHECK(m.permission.redistribute == "yes");
    CHECK(m.replaces.size() == 1);
    CHECK(payload_paths(m) == std::vector<std::string>{ "aki_camera.nrm" });
}

TEST_CASE("packages: texture, model and conversion payloads") {
    Manifest m;
    std::vector<Problem> p;
    CHECK(parse_manifest(manifest("texture-pack", "{\"rtz\": \"arena.rtz\"}"), m, p));
    CHECK(parse_manifest(manifest("texture-pack", "{\"folder\": \"arena/\"}"), m, p));
    CHECK(m.folder == "arena/");
    CHECK(parse_manifest(manifest("model-pack", "{\"packs\": [\"q-robot.akimodel/\", \"q-monk.akimodel\"], \"hd\": [\"q-robot-hd.rtz\"]}"), m, p));
    CHECK(payload_paths(m).size() == 3);
    const std::string rom = std::string(", \"base_rom\": {\"sha1\": [\"") + Sha + "\"], \"name\": \"WWF No Mercy (USA) (Rev 1)\"}";
    CHECK(parse_manifest(manifest("conversion", "{\"folder\": \"conversion\", \"addons\": [\"nmp.rtz\"]}", rom), m, p));
    CHECK(p.empty());
    CHECK(m.base_rom_sha1.size() == 1);
    CHECK(m.base_rom_sha1[0] == "0123456789abcdef0123456789abcdef01234567");   // lower-cased
    CHECK(payload_paths(m) == (std::vector<std::string>{ "conversion", "nmp.rtz" }));
}

TEST_CASE("packages: every missing required field is reported") {
    Manifest m;
    std::vector<Problem> p;
    CHECK(!parse_manifest("{}", m, p));
    for (const char *f : { "format", "kind", "id", "version", "game_id", "display_name", "authors", "payload" }) {
        CHECK(has_error(p, f));
    }
    p.clear();
    CHECK(!parse_manifest("[1]", m, p));
    CHECK(p.size() == 1);
    p.clear();
    CHECK(!parse_manifest("not json", m, p));
}

TEST_CASE("packages: bad values are errors") {
    Manifest m;
    std::vector<Problem> p;
    CHECK(!parse_manifest(manifest("addon", "{\"nrm\": \"a.nrm\"}").replace(11, 1, "2"), m, p));   // format 2
    CHECK(has_error(p, "format"));
    p.clear();
    std::string bad_game = manifest("addon", "{\"nrm\": \"a.nrm\"}");
    bad_game.replace(bad_game.find("wwfnomercy"), 10, "nomercy");
    CHECK(!parse_manifest(bad_game, m, p));
    CHECK(has_error(p, "game_id"));
    CHECK(p.back().message.find("wwfnomercy") != std::string::npos);   // names the right id
    p.clear();
    CHECK(!parse_manifest(manifest("conversion", "{\"folder\": \"c\"}"), m, p));    // no base_rom
    CHECK(has_error(p, "base_rom.sha1"));
    p.clear();
    CHECK(!parse_manifest(manifest("addon", "{\"nrm\": \"../evil.nrm\"}"), m, p));
    CHECK(has_error(p, "payload.nrm"));
    p.clear();
    CHECK(!parse_manifest(manifest("addon", "{\"rtz\": \"a.rtz\"}"), m, p));        // wrong payload for the kind
    CHECK(has_error(p, "payload.nrm"));
    p.clear();
    CHECK(!parse_manifest(manifest("texture-pack", "{\"rtz\": \"a.rtz\", \"folder\": \"b\"}"), m, p));
    CHECK(has_error(p, "payload"));
    p.clear();
    CHECK(!parse_manifest(manifest("model-pack", "{\"packs\": [\"q-robot\"]}"), m, p));
    CHECK(has_error(p, "payload.packs"));
    p.clear();
    CHECK(!parse_manifest(manifest("addon", "{\"nrm\": \"a.nrm\"}", ", \"replaces\": [\"nmp_v3\"]"), m, p));
    CHECK(has_error(p, "replaces"));
    p.clear();
    CHECK(!parse_manifest(manifest("addon", "{\"nrm\": \"a.nrm\"}", ", \"base_rom\": {\"sha1\": [\"abc\"]}"), m, p));
    CHECK(has_error(p, "base_rom.sha1"));
}

TEST_CASE("packages: an odd permission value is only a warning") {
    Manifest m;
    std::vector<Problem> p;
    CHECK(parse_manifest(manifest("addon", "{\"nrm\": \"a.nrm\"}", ", \"permission\": {\"redistribute\": \"maybe\"}"), m, p));
    CHECK(p.size() == 1);
    CHECK(!p[0].error);
}

TEST_CASE("packages: registry round-trips, sorted by id") {
    Registry r;
    Entry b = entry("b_pack", Kind::TexturePack, { { "mods/b_pack.rtz", 0x1234 } });
    Entry a = entry("a_conv", Kind::Conversion, { { "conversions/a_conv", 0xFFFFFFFFFFFFFFFFULL } });
    a.display_name = "A";
    a.installed = "2026-10-29T15:00:00Z";
    Entry tied = entry("a_tex", Kind::TexturePack, { { "mods/a_tex.rtz", 7 } });
    tied.parent = "a_conv";
    upsert(r, b);
    upsert(r, a);
    upsert(r, tied);
    CHECK(r.packages.size() == 3);
    CHECK(r.packages[0].id == "a_conv");
    CHECK(r.packages[2].id == "b_pack");
    const std::string text = format_registry(r);
    Registry back;
    CHECK(parse_registry(text, back) == RegistryStatus::Ok);
    CHECK(format_registry(back) == text);
    CHECK(find(back, "a_conv")->files[0].hash == 0xFFFFFFFFFFFFFFFFULL);
    CHECK(find(back, "a_tex")->parent == "a_conv");
    CHECK(find(back, "a_conv")->display_name == "A");
    upsert(back, entry("b_pack", Kind::TexturePack, {}));   // replace, not add
    CHECK(back.packages.size() == 3);
    CHECK(find(back, "b_pack")->files.empty());
    CHECK(remove(back, "b_pack"));
    CHECK(!remove(back, "b_pack"));
    CHECK(!find(back, "b_pack"));
}

TEST_CASE("packages: a bad registry entry is dropped, the rest kept") {
    Registry r;
    const std::string text = "{\"format\": 1, \"packages\": ["
        "{\"id\": \"ok\", \"kind\": \"addon\", \"files\": [{\"path\": \"mods/ok.nrm\", \"hash\": \"00000000000000ff\"}]},"
        "{\"id\": \"BAD\", \"kind\": \"addon\"},"
        "{\"id\": \"evil\", \"kind\": \"addon\", \"files\": [{\"path\": \"../x\", \"hash\": \"0000000000000000\"}]},"
        "{\"id\": \"nokind\"},"
        "{\"id\": \"ok\", \"kind\": \"texture-pack\"}]}";   // duplicate id
    CHECK(parse_registry(text, r) == RegistryStatus::Damaged);
    CHECK(r.packages.size() == 1);
    CHECK(r.packages[0].files[0].hash == 0xff);
    CHECK(parse_registry("{\"format\": 2, \"packages\": []}", r) == RegistryStatus::Newer);   // don't guess
    CHECK(r.packages.empty());
    CHECK(parse_registry("", r) == RegistryStatus::Corrupt);
    CHECK(parse_registry("{\"format\": 1}", r) == RegistryStatus::Corrupt);
    CHECK(parse_registry("{\"format\": 1, \"packages\": []}", r) == RegistryStatus::Ok);
    CHECK(may_save(RegistryStatus::Ok));
    CHECK(may_save(RegistryStatus::Damaged));
    CHECK(!may_save(RegistryStatus::Newer));
    CHECK(!may_save(RegistryStatus::Corrupt));
    CHECK(!may_save(RegistryStatus::IoError));
}

TEST_CASE("packages: owners and conflicts") {
    Registry r;
    upsert(r, entry("nmp_v3", Kind::Conversion, { { "conversions/nmp_v3", 1 } }));
    upsert(r, entry("arena", Kind::TexturePack, { { "mods/arena.rtz", 2 } }));
    CHECK(owner_of(r, "conversions/nmp_v3") == "nmp_v3");
    CHECK(owner_of(r, "conversions/nmp_v3/aki.json") == "nmp_v3");
    CHECK(owner_of(r, "conversions/nmp_v30").empty());          // a '/' boundary, not a string prefix
    CHECK(owner_of(r, "mods/manual.nrm").empty());
    CHECK(conflict(r, "nmp_v3", Kind::Conversion, { "conversions/nmp_v3" }).empty());   // its own upgrade
    CHECK(!conflict(r, "nmp_v3", Kind::Addon, { "mods/nmp_v3.nrm" }).empty());         // one namespace per game
    CHECK(!conflict(r, "other", Kind::TexturePack, { "mods/arena.rtz" }).empty());     // a file another owns
    CHECK(!conflict(r, "other", Kind::Conversion, { "conversions" }).empty());         // a folder holding one
    CHECK(conflict(r, "other", Kind::TexturePack, { "mods/other.rtz" }).empty());
}

TEST_CASE("packages: file states") {
    const InstalledFile f{ "mods/a.nrm", 42 };
    CHECK(file_state(f, 42) == FileState::Unchanged);
    CHECK(file_state(f, 43) == FileState::Edited);
    CHECK(file_state(f, std::nullopt) == FileState::Missing);
}

TEST_CASE("packages: hash_path on disk (folders match the bundled pack hash, text is CRLF-blind)") {
    TempDir t;
    write(t.path / "pack.akimodel" / "pack.json", "{\"id\": \"x\"}\n");
    write(t.path / "pack.akimodel" / "sub" / "model.bin", std::string("\x00\x01\r\n", 4));
    const auto folder = hash_path(t.path / "pack.akimodel");
    CHECK(folder.has_value());
    CHECK(*folder == recomp::mods::bundled_packs::pack_hash({ { "pack.json", "{\"id\": \"x\"}\n" },
        { "sub/model.bin", std::string("\x00\x01\r\n", 4) } }));
    write(t.path / "pack.akimodel" / "pack.json", "{\"id\": \"x\"}\r\n");      // a checkout's line endings
    CHECK(hash_path(t.path / "pack.akimodel") == folder);
    write(t.path / "pack.akimodel" / "sub" / "model.bin", std::string("\x00\x01\n", 3));   // binary: CRLF counts
    CHECK(hash_path(t.path / "pack.akimodel") != folder);

    write(t.path / "a.nrm", "zip bytes");
    CHECK(hash_path(t.path / "a.nrm") == recomp::mods::bundled_packs::pack_hash({ { "a.nrm", "zip bytes" } }));
    CHECK(!hash_path(t.path / "missing.nrm"));
}

TEST_CASE("packages: registry load/save on disk") {
    TempDir t;
    Registry r;
    CHECK(load_registry(t.path, r) == RegistryStatus::Ok);   // no file yet: empty
    CHECK(r.packages.empty());
    upsert(r, entry("arena", Kind::TexturePack, { { "mods/arena.rtz", 99 } }));
    CHECK(save_registry(t.path, r));
    CHECK(fs::exists(t.path / RegistryFile));
    CHECK(!fs::exists(t.path / "packages.json.tmp"));
    Registry back;
    CHECK(load_registry(t.path, back) == RegistryStatus::Ok);
    CHECK(back.packages.size() == 1);
    CHECK(find(back, "arena")->files[0].hash == 99);
    remove(back, "arena");
    CHECK(save_registry(t.path, back));     // overwrites the existing file
    Registry empty;
    CHECK(load_registry(t.path, empty) == RegistryStatus::Ok);
    CHECK(empty.packages.empty());
    write(t.path / RegistryFile, "{ broken");
    CHECK(load_registry(t.path, empty) == RegistryStatus::Corrupt);
    CHECK(empty.packages.empty());
}

// Review findings (aki-roadmap#154 step 1 self-review): each fails on the first version of the module.

TEST_CASE("packages: hash_path streams and still equals pack_hash (CRLF split across pieces)") {
    TempDir t;
    const std::string text = "line1\r\nline2\r\r\nend\r";
    const std::string bin = std::string("\r\n\x00\xff\r", 5);
    write(t.path / "p" / "notes.txt", text);
    write(t.path / "p" / "b" / "model.bin", bin);
    const uint64_t expect = recomp::mods::bundled_packs::pack_hash({ { "notes.txt", text }, { "b/model.bin", bin } });
    for (size_t chunk : { size_t(1), size_t(2), size_t(3), size_t(6), size_t(1) << 20 }) {
        CHECK(hash_path(t.path / "p", chunk) == expect);
    }
    write(t.path / "notes.txt", text);
    CHECK(hash_path(t.path / "notes.txt", 1) == hash_bytes("notes.txt", text));
}

TEST_CASE("packages: a newer kind and unknown fields survive a load and save") {
    Registry r;
    const std::string text = "{\"format\": 1, \"packages\": ["
        "{\"id\": \"future\", \"kind\": \"sound-pack\", \"version\": \"1.0.0\", \"files\": [{\"path\": \"mods/future.snd\", \"hash\": \"0000000000000001\"}],"
        " \"signed_by\": {\"key\": \"k\"}},"
        "{\"id\": \"arena\", \"kind\": \"texture-pack\", \"verified\": true, \"files\": []}]}";
    CHECK(parse_registry(text, r) == RegistryStatus::Ok);
    CHECK(find(r, "future")->kind == Kind::Unknown);
    CHECK(owner_of(r, "mods/future.snd") == "future");     // still owns its files
    Registry back;
    CHECK(parse_registry(format_registry(r), back) == RegistryStatus::Ok);
    const std::string out = format_registry(back);
    CHECK(out.find("\"sound-pack\"") != std::string::npos);
    CHECK(out.find("\"signed_by\"") != std::string::npos);
    CHECK(out.find("\"verified\": true") != std::string::npos);
    CHECK(!conflict(back, "future", Kind::Addon, {}).empty());   // its id is still taken
}

TEST_CASE("packages: a bad parent, a non-list files or a doubly claimed path is damage") {
    Registry r;
    CHECK(parse_registry("{\"format\": 1, \"packages\": [{\"id\": \"a\", \"kind\": \"addon\", \"parent\": \"BAD\"}]}", r) == RegistryStatus::Damaged);
    CHECK(find(r, "a") && find(r, "a")->parent.empty());
    CHECK(parse_registry("{\"format\": 1, \"packages\": [{\"id\": \"a\", \"kind\": \"addon\", \"files\": \"x\"}]}", r) == RegistryStatus::Damaged);
    CHECK(r.packages.empty());
    CHECK(parse_registry("{\"format\": 1, \"packages\": ["
        "{\"id\": \"a\", \"kind\": \"conversion\", \"files\": [{\"path\": \"conversions/x\", \"hash\": \"0000000000000001\"}]},"
        "{\"id\": \"b\", \"kind\": \"addon\", \"files\": [{\"path\": \"Conversions/X/aki.json\", \"hash\": \"0000000000000002\"}]}]}", r) == RegistryStatus::Damaged);
    CHECK(find(r, "a") && !find(r, "b"));
}

TEST_CASE("packages: names Windows can't make or would alias are refused") {
    CHECK(!safe_relative_path("conversions/nmp."));
    CHECK(!safe_relative_path("a /b"));
    CHECK(!safe_relative_path("aux.rtz"));
    CHECK(!safe_relative_path("nul/x"));
    CHECK(!safe_relative_path("packs/COM1.akimodel"));
    CHECK(!safe_relative_path("a<b"));
    CHECK(!safe_relative_path("what?.rtz"));
    CHECK(!safe_relative_path(std::string("a\x01" "b")));
    CHECK(safe_relative_path("console.rtz"));
    CHECK(safe_relative_path("com10/x"));
    CHECK(safe_relative_path("caf\xc3\xa9.rtz"));       // UTF-8 is fine
    CHECK(!valid_id("nmp."));
    CHECK(!valid_id("con"));
    CHECK(!valid_id("lpt9.x"));
    CHECK(valid_id("console"));
    CHECK(valid_id("com10"));
}

TEST_CASE("packages: ownership ignores case and a trailing slash") {
    Registry r;
    upsert(r, entry("x", Kind::Conversion, { { "conversions/x/", 1 } }));
    CHECK(find(r, "x")->files[0].path == "conversions/x");
    CHECK(owner_of(r, "conversions/x/aki.json") == "x");
    CHECK(owner_of(r, "Conversions/X") == "x");
    upsert(r, entry("robot", Kind::ModelPack, { { "mods/models/q-robot.akimodel", 2 } }));
    CHECK(!conflict(r, "other", Kind::ModelPack, { "mods/models/Q-Robot.akimodel" }).empty());
}

TEST_CASE("packages: prerelease and long version numbers follow SemVer") {
    CHECK(compare_versions("1.0.0-beta.2", "1.0.0-beta.10") < 0);
    CHECK(compare_versions("1.0.0-beta.10", "1.0.0-beta.10.1") < 0);
    CHECK(compare_versions("1.0.0-1", "1.0.0-alpha") < 0);       // a number before text
    CHECK(compare_versions("1.0.0-rc.1", "1.0.0-rc.01") == 0);
    CHECK(compare_versions("1.0.10000000000", "1.0.9999999999") > 0);
    CHECK(compare_versions("1.0.10000000001", "1.0.10000000000") > 0);
    CHECK(!valid_version("1.0.0-beta..1"));
}

TEST_CASE("packages: replaced packages don't conflict; payload parts can't overlap") {
    Registry r;
    upsert(r, entry("camera_old", Kind::Addon, { { "mods/aki_camera.nrm", 1 } }));
    CHECK(!conflict(r, "camera", Kind::Addon, { "mods/aki_camera.nrm" }).empty());
    CHECK(conflict(r, "camera", Kind::Addon, { "mods/aki_camera.nrm" }, { "camera_old" }).empty());
    Manifest m;
    std::vector<Problem> p;
    CHECK(!parse_manifest(manifest("model-pack", "{\"packs\": [\"q.akimodel\", \"q.akimodel/\"]}"), m, p));
    CHECK(has_error(p, "payload"));
    p.clear();
    const std::string rom = std::string(", \"base_rom\": {\"sha1\": [\"") + Sha + "\"]}";
    CHECK(!parse_manifest(manifest("conversion", "{\"folder\": \"c\", \"addons\": [\"c/t.rtz\"]}", rom), m, p));
    CHECK(has_error(p, "payload"));
}

TEST_CASE("packages: a huge format number or a blank display name is an error") {
    Manifest m;
    std::vector<Problem> p;
    std::string text = manifest("addon", "{\"nrm\": \"a.nrm\"}");
    CHECK(!parse_manifest(std::string(text).replace(11, 1, "4294967297"), m, p));
    CHECK(has_error(p, "format"));
    p.clear();
    text.replace(text.find("No Mercy Plus V3"), 16, "   ");
    CHECK(!parse_manifest(text, m, p));
    CHECK(has_error(p, "display_name"));
}
