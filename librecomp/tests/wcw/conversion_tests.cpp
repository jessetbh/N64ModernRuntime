//
// librecomp
//

// [wcw fullconv] Unit tests for librecomp/conversion (aki.json parsing and the session enable rule). Built and run as
// part of every librecomp build (LIBRECOMP_WCW_TESTS); a failure fails the build.

#include "librecomp/conversion.hpp"

#include <string>

#include "wcw_test.h"

using namespace recomp::mods::conversion;

namespace {
    const std::string Full = R"json({
        "format": "aki-conversion", "format_version": 1,
        "id": "nomercy_unlocked", "name": "WWF No Mercy (Unlocked)", "game": "nomercy",
        "components": {
            "patch": { "file": "patch.nrm" },
            "codes": { "file": "codes.txt", "rom": "nomercy-rev1" },
            "saves": [ { "file": "saves/wwf.nomercy.us.bin", "kind": "flash" } ],
            "textures": { "package": "Team-NoMercyHD", "version": "1.0.0", "sha256": "00" },
            "rt": { "lights": "rt_lights.json" }
        }
    })json";
}

TEST_CASE("conversion: a full manifest parses with every component") {
    Conversion c;
    std::string error;
    CHECK(parse(Full, "nomercy_unlocked", c, error));
    CHECK(c.id == "nomercy_unlocked");
    CHECK(c.game == "nomercy");
    CHECK(c.has_patch && c.has_codes && c.has_saves && c.has_textures && c.has_rt);
    CHECK(c.codes_file == "codes.txt");
    CHECK(c.codes_rom == "nomercy-rev1");
    CHECK(c.textures_package == "Team-NoMercyHD");
    CHECK(c.rt_lights_file == "rt_lights.json");
}

TEST_CASE("conversion: a codes-only conversion is as valid as a patch one") {
    Conversion c;
    std::string error;
    const std::string codes = R"json({ "format": "aki-conversion", "format_version": 1, "id": "roster_codes", "game": "nomercy",
        "components": { "codes": { "file": "codes.txt", "rom": "nomercy-rev1" } } })json";
    CHECK(parse(codes, "", c, error));
    CHECK(!c.has_patch && c.has_codes);
}

TEST_CASE("conversion: bad manifests are refused with a reason") {
    Conversion c;
    std::string error;
    CHECK(!parse("not json", "", c, error));
    CHECK(!parse(R"json({ "format": "aki-conversion", "format_version": 2, "id": "a", "components": { "patch": { "file": "p.nrm" } } })json", "", c, error));
    CHECK(!parse(R"json({ "format": "aki-conversion", "format_version": 1, "id": "a", "components": {} })json", "", c, error));
    // The folder name and the id must agree (the launcher installs by id).
    CHECK(!parse(Full, "other_id", c, error));
    // No escaping the conversion folder.
    CHECK(!parse(R"json({ "format": "aki-conversion", "format_version": 1, "id": "a", "components": { "codes": { "file": "..\\..\\x.txt" } } })json", "", c, error));
    CHECK(!parse(R"json({ "format": "aki-conversion", "format_version": 1, "id": "a", "components": { "rt": { "lights": "C:\\x.json" } } })json", "", c, error));
}

TEST_CASE("conversion: ids are safe folder names") {
    CHECK(valid_id("nomercy_unlocked"));
    CHECK(valid_id("nmp-v2.1"));
    CHECK(!valid_id(""));
    CHECK(!valid_id("_packages"));
    CHECK(!valid_id(".hidden"));
    CHECK(!valid_id("Upper"));
    CHECK(!valid_id("a/b"));
    CHECK(!valid_id(std::string(49, 'a')));
}

TEST_CASE("conversion: the session enables only the conversion's mods, plus the cheats engine for runnable codes") {
    const auto with_codes = session_enabled_mods({ "nomercy_unlocked_patch", "hd_pack" }, true, "aki_cheats");
    CHECK(with_codes.size() == 3);
    CHECK(with_codes.count("aki_cheats") == 1);
    const auto no_codes = session_enabled_mods({ "nomercy_unlocked_patch" }, false, "aki_cheats");
    CHECK(no_codes.size() == 1);
    CHECK(no_codes.count("aki_cheats") == 0);
}

TEST_CASE("conversion: a conversion's saves folder wins over the patch mod's; an invalid id refuses to start") {
    std::string out;
    CHECK(resolve_save_subfolder(nullptr, "", out) && out.empty());
    CHECK(resolve_save_subfolder(nullptr, "nomercy_unlocked_patch", out) && (out == "nomercy_unlocked_patch"));
    CHECK(resolve_save_subfolder("", "nomercy_unlocked_patch", out) && (out == "nomercy_unlocked_patch"));
    CHECK(resolve_save_subfolder("tna_vs_roh", "nomercy_unlocked_patch", out) && (out == "tna_vs_roh"));
    CHECK(resolve_save_subfolder("tna_vs_roh", "", out) && (out == "tna_vs_roh"));
    // Never fall back to the normal saves for a bad id.
    CHECK(!resolve_save_subfolder("../saves", "nomercy_unlocked_patch", out) && out.empty());
    CHECK(!resolve_save_subfolder("TNA", "", out));
}

TEST_CASE("conversion: codes run for the same game (the engine translates revisions); untagged or other games stay off") {
    CHECK(codes_runnable("nomercy-rev1", "nomercy-rev1"));
    CHECK(codes_runnable("nomercy-rev0", "nomercy-rev1"));
    CHECK(!codes_runnable("", "nomercy-rev1"));
    CHECK(!codes_runnable("vpw2", "nomercy-rev1"));
    CHECK(codes_runnable("vpw2", "vpw2"));
}
