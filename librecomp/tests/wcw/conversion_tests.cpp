//
// librecomp
//

// [wcw fullconv] Unit tests for librecomp/conversion (aki.json parsing and the session enable rule). Built and run as
// part of every librecomp build (LIBRECOMP_WCW_TESTS); a failure fails the build.

#include "librecomp/conversion.hpp"
#include "librecomp/conversion_codes.hpp"

#include <cstdio>
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
