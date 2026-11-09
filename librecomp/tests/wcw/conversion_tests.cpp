//
// librecomp
//

// [wcw fullconv] Unit tests for librecomp/conversion (aki.json parsing and the session enable rule). Built and run as
// part of every librecomp build (LIBRECOMP_WCW_TESTS); a failure fails the build.

#include "librecomp/conversion.hpp"
#include "librecomp/conversion_codes.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

TEST_CASE("conversion: an inline texture pack folder counts as textures, with its naming") {
    Conversion c;
    std::string error;
    const std::string pack = R"json({ "format": "aki-conversion", "format_version": 1, "id": "hd_rice", "game": "wt",
        "components": { "textures": { "dir": "textures/", "naming": "rice" } } })json";
    CHECK(parse(pack, "hd_rice", c, error));
    CHECK(c.has_textures);
    CHECK(c.textures_dir == "textures");
    CHECK(c.textures_naming == "rice");
    CHECK(c.textures_package.empty());
    // The pack folder stays inside the conversion too.
    CHECK(!parse(R"json({ "format": "aki-conversion", "format_version": 1, "id": "a", "components": { "textures": { "dir": "../other" } } })json", "", c, error));
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

TEST_CASE("conversion: a wrongly typed manifest is refused, never thrown (aki-roadmap#243)") {
    const auto refused = [](const std::string &text, const char *want) {
        Conversion c;
        std::string error;
        bool ok = true;
        bool threw = false;
        try {
            ok = parse(text, "", c, error);
        }
        catch (...) {
            threw = true;
        }
        CHECK(!threw);
        CHECK(!ok);
        CHECK(error.find(want) != std::string::npos);
    };
    const std::string head = R"json({ "format": "aki-conversion", "format_version": 1, "id": "a", )json";
    // Components written as lists (the authoring mistake that crashed the game).
    refused(head + R"json("components": { "codes": [{ "file": "c.txt" }] } })json", "components.codes must be an object");
    refused(head + R"json("components": { "textures": [{ "dir": "t" }] } })json", "components.textures must be an object");
    refused(head + R"json("components": { "rt": [{ "lights": "l.json" }] } })json", "components.rt must be an object");
    refused(head + R"json("components": { "patch": [{ "file": "p.nrm" }] } })json", "components.patch must be an object");
    refused(head + R"json("components": { "models": [], "codes": { "file": "c.txt" } } })json", "components.models must be an object");
    refused(head + R"json("components": { "codes": "c.txt" } })json", "components.codes must be an object");
    // Values of the wrong type.
    refused(R"json({ "format": "aki-conversion", "format_version": 1, "id": 5, "components": { "codes": { "file": "c.txt" } } })json", "wrong type");
    refused(R"json({ "format": "aki-conversion", "format_version": "1", "id": "a", "components": { "codes": { "file": "c.txt" } } })json", "wrong type");
    refused(head + R"json("components": { "codes": { "file": 5 } } })json", "wrong type");
    refused(head + R"json("components": { "codes": { "file": "c.txt", "rom": "x", "rom_revision": 1 } } })json", "wrong type");
    refused(head + R"json("components": { "textures": { "package": true } } })json", "wrong type");
    refused(head + R"json("components": { "rt": { "lights": 1 } } })json", "wrong type");
    refused(head + R"json("components": { "models": { "dir": "m", "slots": [{ "wrestler": "0x0404", "pack": "p", "name": 7 }] } } })json", "wrong type");
    // saves stays a list, and the valid manifests still parse.
    Conversion c;
    std::string error;
    CHECK(parse(head + R"json("components": { "saves": ["s.sra"] } })json", "", c, error));
    CHECK(c.has_saves);
    CHECK(parse(Full, "", c, error));
    CHECK(c.has_patch && c.has_codes);
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

namespace {
    // A small RDRAM in the recomp's layout (byte a at a ^ 3), with big-endian helpers.
    struct Ram {
        std::vector<uint8_t> bytes = std::vector<uint8_t>(0x10000);
        uint8_t get8(uint32_t a) const { return bytes[(a - 0x80000000u) ^ 3]; }
        uint16_t get16(uint32_t a) const { return uint16_t((get8(a) << 8) | get8(a + 1)); }
        void set8(uint32_t a, uint8_t v) { bytes[(a - 0x80000000u) ^ 3] = v; }
        void set16(uint32_t a, uint16_t v) { set8(a, uint8_t(v >> 8)); set8(a + 1, uint8_t(v)); }
        void run(const std::vector<codes::Code> &table) { codes::apply(table, bytes.data(), uint32_t(bytes.size())); }
    };

    std::vector<codes::Code> parsed(const std::string &text, const std::string &game = "nomercy", int rev = 1) {
        std::vector<codes::Code> out;
        codes::Report r;
        CHECK(codes::parse_file(text, game, rev, out, r));
        return out;
    }
}

TEST_CASE("conversion: the launcher's AKI_CONVERSION wins over the Mods-menu choice and is strict (#57)") {
    StartupChoice c = choose_startup("aew64", "nomercy_unlocked\n");
    CHECK(c.id == "aew64");
    CHECK(c.from_launcher);
    CHECK(!c.lenient());
    // An id the launcher passes is never second-guessed here: the loader refuses a bad one, as before.
    c = choose_startup("Not Valid!", "aew64\n");
    CHECK(c.id == "Not Valid!");
    CHECK(c.from_launcher);
}

TEST_CASE("conversion: without the launcher, conversions/active.txt's first line (trimmed) is a lenient choice (#57)") {
    StartupChoice c = choose_startup(nullptr, "aew64\r\nleftover\n");
    CHECK(c.id == "aew64");
    CHECK(!c.from_launcher);
    CHECK(c.lenient());
    CHECK(choose_startup("", "  revse \t\n").id == "revse");
    CHECK(choose_startup(nullptr, "nomercy_unlocked").id == "nomercy_unlocked");
}

TEST_CASE("conversion: no launcher id and an empty or blank choice file mean no conversion (#57)") {
    CHECK(choose_startup(nullptr, "").id.empty());
    CHECK(choose_startup(nullptr, "   \n").id.empty());
    CHECK(choose_startup("", "\r\n").id.empty());
    CHECK(!choose_startup(nullptr, "").lenient());
    CHECK(!choose_startup(nullptr, "").from_launcher);
}

TEST_CASE("conversion: a Mods-menu choice that can't run says why, so the game starts normally instead (#57)") {
    Conversion c;
    CHECK(check_choice("nomercy_unlocked", Full, c).empty());
    CHECK(c.id == "nomercy_unlocked" && c.has_patch);
    CHECK(check_choice("../aew64", Full, c) == "\"../aew64\" is not a valid conversion id");
    CHECK(check_choice("C:", Full, c) == "\"C:\" is not a valid conversion id");
    CHECK(check_choice("aew64", "", c) == "it isn't installed any more");
    CHECK(!check_choice("aew64", "not json", c).empty());
    // The folder's aki.json names another id.
    CHECK(!check_choice("aew64", Full, c).empty());
}

TEST_CASE("conversion codes: grammar, tags and what is ignored") {
    std::vector<codes::Code> out;
    codes::Report r;
    const std::string file = "\xEF\xBB\xBF# rom: nomercy-rev1\r\n[Cheat]\r\n8109940A FFFF # note\r\n800001231234\r\n"
        "F1000000 0000\r\n89000000 0001\r\n12345678 0000\r\nDEADBEEF Mode\r\n80000000 12\r\nD0000010 0001\r\n";
    CHECK(codes::parse_file(file, "nomercy", 1, out, r));
    CHECK_EQ(r.writes, 2);
    CHECK_EQ(r.enablers, 1);
    CHECK_EQ(r.buttons, 1);
    CHECK_EQ(r.unknown, 1);
    CHECK_EQ(r.malformed, 1);           // "80000000 12" (a name like "DEADBEEF Mode" isn't counted)
    CHECK_EQ(r.added, 2);               // the trailing D0 governs nothing and is dropped
    CHECK(out[0].addr == 0x8009940Au && out[0].value == 0xFFFF && out[0].type == 0x81);
    CHECK(out[1].addr == 0x80000123u && out[1].value == 0x1234);

    // Another revision or game: not this table's (the cheats mod translates No Mercy Rev 0).
    CHECK(!codes::parse_file("# rom: nomercy-rev0\n8109940A FFFF\n", "nomercy", 1, out, r) && out.empty());
    CHECK(!codes::parse_file("# rom: vpw2\n8109940A FFFF\n", "nomercy", 1, out, r));
    CHECK(!codes::parse_file("# rom: nomercy2\n", "nomercy", 1, out, r));
    // Untagged files run as written.
    CHECK(codes::parse_file("8109940A FFFF\n", "wt", 0, out, r) && (out.size() == 1));

    std::string key;
    int rev = -1;
    codes::split_rom_tag("nomercy-rev1", key, rev);
    CHECK((key == "nomercy") && (rev == 1));
    codes::split_rom_tag("vpw2", key, rev);
    CHECK((key == "vpw2") && (rev == 0));
}

TEST_CASE("conversion codes: writes, conditionals and repeaters as GameShark units") {
    Ram ram;
    ram.run(parsed("80000100 00AB\n81000102 1234\nA0000104 00CD\n"));
    CHECK_EQ(ram.get8(0x80000100), uint8_t(0xAB));
    CHECK_EQ(ram.get16(0x80000102), uint16_t(0x1234));
    CHECK_EQ(ram.get8(0x80000104), uint8_t(0xCD));

    // D0 true -> the next write runs; D1 false -> skipped; D2/D3 differ.
    ram.set16(0x80000200, 0x0005);
    ram.run(parsed("D0000201 0005\n80000300 0001\nD1000200 0006\n80000301 0001\nD3000200 0006\n80000302 0001\nD2000201 0005\n80000303 0001\n"));
    CHECK_EQ(ram.get8(0x80000300), uint8_t(1));
    CHECK_EQ(ram.get8(0x80000301), uint8_t(0));
    CHECK_EQ(ram.get8(0x80000302), uint8_t(1));
    CHECK_EQ(ram.get8(0x80000303), uint8_t(0));

    // Stacked conditionals all have to hold; a false one skips the whole unit it governs (here a repeater + write).
    ram.run(parsed("D0000201 0005\nD0000201 0009\n50000302 0001\n81000400 0010\n80000500 0077\n"));
    CHECK_EQ(ram.get16(0x80000400), uint16_t(0));
    CHECK_EQ(ram.get8(0x80000500), uint8_t(0x77)); // the code after the skipped unit still runs

    // 50 00NNSS IIII: 3 writes, address += 2, value += 1.
    ram.run(parsed("50000302 0001\n81000600 0010\n"));
    CHECK_EQ(ram.get16(0x80000600), uint16_t(0x10));
    CHECK_EQ(ram.get16(0x80000602), uint16_t(0x11));
    CHECK_EQ(ram.get16(0x80000604), uint16_t(0x12));
    CHECK_EQ(ram.get16(0x80000606), uint16_t(0));

    // Outside RDRAM: skipped, never written.
    ram.run(parsed("817FFFFE 1234\n"));
    CHECK_EQ(ram.get16(0x8000FFFE), uint16_t(0));
}

TEST_CASE("conversion codes: no size cap (a 300,000-code table)") {
    std::string text = "# rom: nomercy-rev1\n";
    for (int i = 0; i < 300000; i++) {
        char line[32];
        snprintf(line, sizeof(line), "80%06X 00%02X\n", 0x1000 + (i % 0xE000), i & 0xFF);
        text += line;
    }

    const auto table = parsed(text);
    CHECK_EQ(table.size(), size_t(300000));
    Ram ram;
    ram.run(table);
    // The last write to an address wins, as when the mod ran the list in order.
    CHECK_EQ(ram.get8(0x80001001), uint8_t(286721 & 0xFF)); // last written by code 286721 (0xE000 * 5 + 1)
}

TEST_CASE("conversion: codes run for the same game (the engine translates revisions); untagged or other games stay off") {
    CHECK(codes_runnable("nomercy-rev1", "nomercy-rev1"));
    CHECK(codes_runnable("nomercy-rev0", "nomercy-rev1"));
    CHECK(!codes_runnable("", "nomercy-rev1"));
    CHECK(!codes_runnable("vpw2", "nomercy-rev1"));
    CHECK(codes_runnable("vpw2", "vpw2"));
}

// The session rule with the player's add-ons (aki-roadmap#154 step 4).

TEST_CASE("conversion: aki.json addons policy parses (default data)") {
    Conversion c;
    std::string error;
    CHECK(parse(Full, "", c, error));
    CHECK(c.addons_allow == AddonsAllow::Data);
    CHECK(c.addons_mods.empty());
    std::string with = Full;
    with.insert(with.rfind('}'), R"(, "addons": { "allow": "none", "mods": ["aki_camera", 5] })");
    CHECK(parse(with, "", c, error));
    CHECK(c.addons_allow == AddonsAllow::None);
    CHECK(c.addons_mods == std::vector<std::string>{ "aki_camera" });
    std::string all = Full;
    all.insert(all.rfind('}'), R"(, "addons": { "allow": "all" })");
    CHECK(parse(all, "", c, error) && c.addons_allow == AddonsAllow::All);
    std::string odd = Full;
    odd.insert(odd.rfind('}'), R"(, "addons": { "allow": "everything" })");
    CHECK(parse(odd, "", c, error) && c.addons_allow == AddonsAllow::Data);   // unknown: the default
}

TEST_CASE("conversion: player addons.json parses; bad input is empty") {
    const PlayerAddons p = parse_player_addons(R"({"enabled": ["aki_camera"], "disabled": ["hd", 3]})");
    CHECK(p.enabled == std::vector<std::string>{ "aki_camera" });
    CHECK(p.disabled == std::vector<std::string>{ "hd" });
    CHECK(parse_player_addons("nope").enabled.empty());
}

TEST_CASE("conversion: session add-ons follow the conversion's policy and the player's word") {
    const std::vector<PlayerMod> mods = {
        { "hd_textures", false, false, true },      // data pack, enabled
        { "off_textures", false, false, false },    // data pack, not enabled
        { "aki_camera", true, false, true },        // code, enabled
        { "aki_modelimport", true, false, true },   // code, enabled
        { "aki_cheats", true, false, true },        // the cheats engine
    };
    Conversion c;   // default: data
    std::vector<std::string> why;
    auto on = session_enabled_mods({ "hack_patch" }, false, "aki_cheats", mods, c, {}, &why);
    CHECK(on.contains("hack_patch"));
    CHECK(on.contains("hd_textures"));
    CHECK(!on.contains("off_textures"));
    CHECK(!on.contains("aki_camera"));
    CHECK(!on.contains("aki_cheats"));          // no runnable codes, not listed
    CHECK(why.size() == mods.size());           // one line per player mod

    c.addons_mods = { "aki_modelimport" };      // the conversion's allow-list
    on = session_enabled_mods({ "hack_patch" }, true, "aki_cheats", mods, c, {});
    CHECK(on.contains("aki_modelimport"));
    CHECK(!on.contains("aki_camera"));
    CHECK(on.contains("aki_cheats"));           // runnable codes

    c.addons_allow = AddonsAllow::All;
    on = session_enabled_mods({ "hack_patch" }, false, "aki_cheats", mods, c, {});
    CHECK(on.contains("aki_camera") && on.contains("hd_textures") && !on.contains("off_textures"));

    c.addons_allow = AddonsAllow::None;
    PlayerAddons player;
    player.enabled = { "aki_camera", "off_textures" };
    player.disabled = { "hd_textures" };
    on = session_enabled_mods({ "hack_patch" }, false, "aki_cheats", mods, c, player);
    CHECK(on.contains("aki_camera"));           // the player's word wins over the conversion's
    CHECK(on.contains("off_textures"));
    CHECK(!on.contains("hd_textures"));
    CHECK(!on.contains("aki_modelimport"));     // allow none
    CHECK(on.contains("hack_patch"));           // the conversion's own always run
}

TEST_CASE("conversion: another hack's patch never joins; the cheats engine needs the conversion's codes") {
    const std::vector<PlayerMod> mods = {
        { "other_hack", false, true, true },        // another ROM hack's patch, enabled
        { "aki_cheats", true, false, true },        // the cheats engine, enabled
        { "both", true, false, true },              // in both of the player's lists
    };
    Conversion c;
    c.addons_allow = AddonsAllow::All;
    auto on = session_enabled_mods({ "hack_patch" }, false, "aki_cheats", mods, c, {});
    CHECK(!on.contains("other_hack"));           // even under "all"
    CHECK(!on.contains("aki_cheats"));           // no runnable codes: the stock list would poke the hack
    CHECK(on.contains("both"));
    PlayerAddons player;
    player.enabled = { "other_hack", "aki_cheats", "both", "hack_patch" };
    player.disabled = { "both", "hack_patch" };
    on = session_enabled_mods({ "hack_patch" }, false, "aki_cheats", mods, c, player);
    CHECK(!on.contains("other_hack"));           // not even when the player asks
    CHECK(on.contains("aki_cheats"));            // the player's own choice
    CHECK(!on.contains("both"));                 // disabled wins
    CHECK(on.contains("hack_patch"));            // the conversion's own can't be turned off
    on = session_enabled_mods({ "hack_patch" }, true, "aki_cheats", mods, c, {});
    CHECK(on.contains("aki_cheats"));            // runnable codes
}

// aki-roadmap#226: the models component.
TEST_CASE("conversion: a models folder is a component of its own, inside the conversion folder (#226)") {
    Conversion c;
    std::string error;
    const std::string models = R"json({ "format": "aki-conversion", "format_version": 1, "id": "roster_models", "game": "vpw2",
        "components": { "models": { "dir": "models/" } } })json";
    CHECK(parse(models, "roster_models", c, error));
    CHECK(c.has_models && (c.models_dir == "models"));
    CHECK(!c.has_patch && !c.has_codes);
    CHECK(c.models_refusal.empty());

    Conversion d;
    CHECK(parse(Full, "nomercy_unlocked", d, error));
    CHECK(!d.has_models && d.models_dir.empty());   // no component: the player's own packs, as before

    const std::string empty = R"json({ "format": "aki-conversion", "format_version": 1, "id": "m", "game": "vpw2",
        "components": { "models": { } } })json";
    CHECK(!parse(empty, "", c, error));             // no dir and nothing else: no usable components

    const std::string escape = R"json({ "format": "aki-conversion", "format_version": 1, "id": "m", "game": "vpw2",
        "components": { "patch": { "file": "patch.nrm" }, "models": { "dir": "../mods/models" } } })json";
    CHECK(!parse(escape, "", c, error));
    CHECK(error.find("leaves the conversion folder") != std::string::npos);
}

TEST_CASE("conversion: models are refused beside a patch that uses the models window (#226)") {
    // Plain patches carry no relink.json, and a de-relinked hack that moved nothing (VPW2 freem Edition) has empty lists.
    CHECK(models_refusal("").empty());
    CHECK(models_refusal(R"json({"format": 1, "base": "vpw2-rev0", "functions": [], "code_ranges": [], "data_ranges": []})json").empty());
    // Born to be Wired / No Mercy Plus: code and data at 0x80400FE0..0x804A0000.
    const std::string btbw = R"json({"code_ranges": [[2151682016, 2152333312], [2399141888, 2399193212]],
        "data_ranges": [[2151682016, 2152333312, 1823168]], "preload": [{"rom": 1823168, "ram": 2151682016, "size": 651296}],
        "zero": [{"ram": 2151678080, "size": 130900}]})json";
    const std::string why = models_refusal(btbw);
    CHECK(!why.empty());
    CHECK(why.find("aren't supported yet") != std::string::npos);
    CHECK(why.find('#') == std::string::npos);  // players see it: no private issue numbers (aki-roadmap#281)
    CHECK(why.find("0x80400FE0") != std::string::npos);  // the first range that overlaps, as the hack wrote it
    // Legends / Showdown 64: code above 8 MB (fine) but a zeroed block at 0x80600000 (inside the window).
    CHECK(!models_refusal(R"json({"code_ranges": [[2399141888, 2399146444]], "zero": [{"ram": 2153775104, "size": 6864}]})json").empty());
    CHECK(models_refusal(R"json({"code_ranges": [[2399141888, 2399146444]]})json").empty());
    // KSEG1 addresses compare physically; touching the window's edges from outside is fine.
    CHECK(!models_refusal(R"json({"zero": [{"ram": 2689597440, "size": 16}]})json").empty());   // 0xA0500000
    CHECK(models_refusal(R"json({"zero": [{"ram": 2147483648, "size": 4194304}]})json").empty());  // 0x80000000 + 4 MB ends at the window
    CHECK(models_refusal(R"json({"code_ranges": [[2155872256, 2155872260]]})json").empty());      // 0x80800000: past it
    CHECK(!models_refusal(R"json({"code_ranges": [[2155872252, 2155872256]]})json").empty());     // its last word
    CHECK(models_refusal(R"json({"zero": [{"ram": 2151677952, "size": 0}]})json").empty());        // empty range
    // Unknown memory use is refused too.
    CHECK(!models_refusal("not json").empty());
}

TEST_CASE("conversion: a session with usable models turns on model import, refused models leave it to the add-on rules (#226)") {
    const std::vector<PlayerMod> mods = {
        { "aki_modelimport", true, false, false },  // installed (bundled), off in the player's setup
        { "aki_camera", true, false, true },
    };
    Conversion c;
    c.has_models = true;
    c.models_dir = "models";
    std::vector<std::string> why;
    auto on = session_enabled_mods({ "hack_patch" }, false, "aki_cheats", mods, c, {}, &why);
    CHECK(on.contains(ModelsModId));
    CHECK(on.contains("hack_patch") && !on.contains("aki_camera"));
    CHECK(why.size() == mods.size());           // still one line per player mod

    c.models_refusal = "its patch uses Expansion RAM";
    on = session_enabled_mods({ "hack_patch" }, false, "aki_cheats", mods, c, {});
    CHECK(!on.contains(ModelsModId));           // not enabled by the player: stays off

    Conversion none;                            // no models component: unchanged
    on = session_enabled_mods({ "hack_patch" }, false, "aki_cheats", mods, none, {});
    CHECK(!on.contains(ModelsModId));
}

TEST_CASE("conversion: models slots name the hack's wrestlers and become the host's models.txt (#226)") {
    Conversion c;
    std::string error;
    const std::string slots = R"json({ "format": "aki-conversion", "format_version": 1, "id": "aew", "game": "nomercy",
        "components": { "models": { "dir": "models", "format": 2, "roster": "models/roster.json", "slots": [
            { "wrestler": "0x0404", "name": "Kenny Omega", "costume": 0, "pack": "kenny" },
            { "wrestler": "302", "name": "Jon Moxley", "pack": "mox" },
            { "wrestler": 7, "pack": "q-robot" } ] } } })json";
    CHECK(parse(slots, "aew", c, error));
    CHECK(c.has_models && c.models_has_slots && c.models_refusal.empty());
    CHECK(c.models_format == 2);
    CHECK(c.models_roster == "models/roster.json");
    CHECK(c.models_slots.size() == 3);
    CHECK((c.models_slots[0].wrestler == 0x0404) && (c.models_slots[0].costume == 0) && (c.models_slots[0].name == "Kenny Omega"));
    CHECK((c.models_slots[1].wrestler == 0x0302) && (c.models_slots[1].costume == -1));
    CHECK(c.models_slots[2].wrestler == 7);
    CHECK(models_txt(c.models_slots) == "# from aki.json components.models.slots\n"
        "kenny wrestler 0x0404 costume 0\nmox wrestler 0x0302\nq-robot wrestler 0x0007\n");

    // No format: the first version; no slots: dir/models.txt is read as it is.
    const std::string plain = R"json({ "format": "aki-conversion", "format_version": 1, "id": "m", "game": "vpw2",
        "components": { "models": { "dir": "models" } } })json";
    CHECK(parse(plain, "", c, error));
    CHECK((c.models_format == 1) && !c.models_has_slots && c.models_refusal.empty());
}

TEST_CASE("conversion: a newer model format or a bad slot turns the models off with a reason, the conversion still runs (#226)") {
    auto with = [](const std::string &models) {
        return R"json({ "format": "aki-conversion", "format_version": 1, "id": "m", "game": "nomercy",
            "components": { "patch": { "file": "patch.nrm" }, "models": )json" + models + " } }";
    };
    Conversion c;
    std::string error;
    CHECK(parse(with(R"json({ "dir": "models", "format": 3 })json"), "", c, error));
    CHECK(c.has_patch && c.has_models);
    CHECK(c.models_refusal.find("format 3") != std::string::npos);
    CHECK(c.models_refusal.find("update the game") != std::string::npos);

    CHECK(parse(with(R"json({ "dir": "models", "format": "two" })json"), "", c, error));
    CHECK(c.models_refusal.find("\"format\"") != std::string::npos);

    for (const char *bad : { R"json([{ "pack": "kenny" }])json", R"json([{ "wrestler": "0x10000", "pack": "kenny" }])json",
             R"json([{ "wrestler": "0x0404", "pack": "../kenny" }])json", R"json([{ "wrestler": "0x0404", "pack": "kenny", "costume": -1 }])json",
             R"json([{ "wrestler": "zz", "pack": "kenny" }])json", R"json("kenny")json" }) {
        CHECK(parse(with(std::string(R"json({ "dir": "models", "slots": )json") + bad + " }"), "", c, error));
        CHECK(!c.models_refusal.empty());
        CHECK(c.models_slots.empty());
    }

    // The session leaves model import to the add-on rules then, as for a relinked patch.
    const std::vector<PlayerMod> mods = { { "aki_modelimport", true, false, false } };
    CHECK(!session_enabled_mods({ "m" }, false, "aki_cheats", mods, c, {}).contains(ModelsModId));

    // The roster path stays inside the conversion folder.
    CHECK(!parse(with(R"json({ "dir": "models", "roster": "../roster.json" })json"), "", c, error));
}

TEST_CASE("conversion: seed saves are saves[] entries with mode seed or no mode; their paths stay inside (#242)") {
    auto with = [](const std::string &saves) {
        return R"json({ "format": "aki-conversion", "format_version": 1, "id": "s", "game": "nomercy",
            "components": { "patch": { "file": "patch.nrm" }, "saves": )json" + saves + " } }";
    };
    Conversion c;
    std::string error;
    CHECK(parse(with(R"json([ { "file": "saves/wwf.nomercy.us.bin", "kind": "flash" },
        { "file": "saves/pak0.bin", "kind": "pak0", "mode": "seed" }, { "file": "saves/other.bin", "mode": "later" }, "junk", {} ])json"), "", c, error));
    CHECK(c.has_saves);
    CHECK(c.seed_saves.size() == 2);
    CHECK(c.seed_saves[0] == "saves/wwf.nomercy.us.bin");
    CHECK(c.seed_saves[1] == "saves/pak0.bin");

    CHECK(parse(Full, "nomercy_unlocked", c, error));
    CHECK(c.seed_saves.size() == 1);

    for (const char *bad : { "../wwf.nomercy.us.bin", "/wwf.nomercy.us.bin", "\\\\wwf.nomercy.us.bin", "C:/wwf.nomercy.us.bin" }) {
        CHECK(!parse(with(std::string(R"json([ { "file": ")json") + bad + R"json(" } ])json"), "", c, error));
        CHECK(error.find("leaves the conversion folder") != std::string::npos);
    }
}

TEST_CASE("conversion: the RT rig path is set only when the conversion has one and the variable is unset (#241)") {
    Conversion c;
    std::string error;
    CHECK(parse(Full, "nomercy_unlocked", c, error));
    const std::filesystem::path folder = std::filesystem::u8path("conv/nomercy_unlocked");
    CHECK(rt_lights_path(c, folder, nullptr) == folder / "rt_lights.json");
    CHECK(rt_lights_path(c, folder, "") == folder / "rt_lights.json");
    CHECK(rt_lights_path(c, folder, "C:/launcher/rt_lights.json").empty());   // the launcher's (or a test run's) wins

    Conversion none;
    CHECK(rt_lights_path(none, folder, nullptr).empty());
}

TEST_CASE("conversion: seed saves are copied once, never over an existing save; a missing one is logged (#242)") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("wcw_conversion_seed_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path folder = root / "conversions" / "s";
    const fs::path saves = root / "saves" / "s";
    fs::create_directories(folder / "saves");
    auto write = [](const fs::path &p, const std::string &text) { std::ofstream(p, std::ios::binary) << text; };
    auto read = [](const fs::path &p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); };
    write(folder / "saves" / "wwf.nomercy.us.bin", "seed-flash");
    write(folder / "saves" / "pak0.bin", "seed-pak");

    Conversion c;
    c.id = "s";
    c.seed_saves = { "saves/wwf.nomercy.us.bin", "saves/pak0.bin", "saves/missing.bin" };

    SeedReport r = seed_saves(c, folder, saves);   // creates saves/s/
    CHECK(r.seeded == 2 && r.kept == 0 && r.failed == 1);
    CHECK(r.lines.size() == 3);
    CHECK(read(saves / "wwf.nomercy.us.bin") == "seed-flash");
    CHECK(read(saves / "pak0.bin") == "seed-pak");
    CHECK(!fs::exists(saves / "missing.bin"));
    CHECK(!fs::exists(saves / "wwf.nomercy.us.bin.seed-tmp"));

    // The player's progress stays.
    write(saves / "wwf.nomercy.us.bin", "player");
    fs::remove(saves / "pak0.bin");
    r = seed_saves(c, folder, saves);
    CHECK(r.seeded == 1 && r.kept == 1 && r.failed == 1);
    CHECK(read(saves / "wwf.nomercy.us.bin") == "player");
    CHECK(read(saves / "pak0.bin") == "seed-pak");

    std::error_code ec;
    fs::remove_all(root, ec);
}
