//
// librecomp
//

// [wcw] Unit tests for librecomp/bundled_packs (the bundled starter model packs and their updates, aki-roadmap#149).
// Built and run as part of every librecomp build (LIBRECOMP_WCW_TESTS); a failure fails the build.

#include "librecomp/bundled_packs.hpp"

#include <string>
#include <vector>

#include "wcw_test.h"

using namespace recomp::mods::bundled_packs;

namespace {
    const std::string BinBytes = std::string("\x00\x01\r\n", 4);
}

TEST_CASE("bundled packs: the pack hash matches AkiMods pack_history.py (shared vector)") {
    // pack_history.pack_hash_files([('pack.json', b'{\r\n  "id": "x"\r\n}\r\n'), ('model.bin', b'\x00\x01\r\n')])
    CHECK(hash_hex(pack_hash({ { "pack.json", "{\r\n  \"id\": \"x\"\r\n}\r\n" }, { "model.bin", BinBytes } })) == "974f489db7b79f83");
}

TEST_CASE("bundled packs: the hash ignores file order and a checkout's CRLF in text files, not in binaries") {
    const uint64_t crlf = pack_hash({ { "pack.json", "{\r\n  \"id\": \"x\"\r\n}\r\n" }, { "model.bin", BinBytes } });
    const uint64_t lf = pack_hash({ { "model.bin", BinBytes }, { "pack.json", "{\n  \"id\": \"x\"\n}\n" } });
    CHECK(crlf == lf);
    // model.bin's own CR LF bytes are data.
    CHECK(pack_hash({ { "pack.json", "{\n  \"id\": \"x\"\n}\n" }, { "model.bin", std::string("\x00\x01\n", 3) } }) != lf);
    // An edit anywhere, or a renamed file, changes it.
    CHECK(pack_hash({ { "pack.json", "{\n  \"id\": \"y\"\n}\n" }, { "model.bin", BinBytes } }) != lf);
    CHECK(pack_hash({ { "pack2.json", "{\n  \"id\": \"x\"\n}\n" }, { "model.bin", BinBytes } }) != lf);
    CHECK(hash_hex(0x0123456789abcdefULL) == "0123456789abcdef");
}

TEST_CASE("bundled packs: versions compare part by part as numbers") {
    CHECK(compare_versions("1.0.0", "1.1.0") < 0);
    CHECK(compare_versions("1.10.0", "1.9.0") > 0);
    CHECK(compare_versions("1.0", "1.0.0") == 0);
    CHECK(compare_versions("", "1.0.0") < 0);
    CHECK(compare_versions("2", "1.9.9") > 0);
    CHECK(compare_versions("1.x.0", "1.0.0") == 0);
    CHECK(pack_version("{ \"id\": \"q-warrior\", \"version\": \"1.1.0\" }") == "1.1.0");
    CHECK(pack_version("{ \"id\": \"q-warrior\" }").empty());
    CHECK(pack_version("not json").empty());
}

TEST_CASE("bundled packs: install once; a pack the player deleted stays deleted") {
    Installed none;
    CHECK(decide(none, 1, "1.1.0", {}) == Action::Install);
    Installed deleted;
    deleted.recorded = true;
    CHECK(decide(deleted, 1, "1.1.0", { 7 }) == Action::Skip);
}

TEST_CASE("bundled packs: an older unedited bundled copy is replaced (#149); edited or the player's own is kept") {
    Installed older;
    older.recorded = true;
    older.present = true;
    older.hash = 7;
    older.version = "1.0.0";
    CHECK(decide(older, 9, "1.1.0", { 5, 7 }) == Action::Replace);
    // Edited: its hash is no bundled version's.
    Installed edited = older;
    edited.hash = 8;
    CHECK(decide(edited, 9, "1.1.0", { 5, 7 }) == Action::Keep);
    // An earlier bundle's hash but not an older version (e.g. the player bumped the version): kept.
    Installed same_version = older;
    same_version.version = "1.1.0";
    CHECK(decide(same_version, 9, "1.1.0", { 7 }) == Action::Keep);
    // Already this bundle's copy.
    Installed current = older;
    current.hash = 9;
    current.version = "1.1.0";
    CHECK(decide(current, 9, "1.1.0", { 7 }) == Action::Keep);
    // The player's own pack of that name, never installed by us: kept even if it happens to be older.
    Installed own = edited;
    own.recorded = false;
    CHECK(decide(own, 9, "1.1.0", { 7 }) == Action::Keep);
    // No history: nothing is replaced.
    CHECK(decide(older, 9, "1.1.0", {}) == Action::Keep);
}

TEST_CASE("bundled packs: .bundled and bundled-history.json parse leniently") {
    const auto r = parse_record("q-warrior.akimodel\r\n\nq-monk.akimodel  \nq-robot.akimodel");
    CHECK(r.size() == 3);
    CHECK(r.count("q-monk.akimodel") == 1);
    CHECK(format_record(r) == "q-monk.akimodel\nq-robot.akimodel\nq-warrior.akimodel\n");
    CHECK(parse_record("").empty());
    const auto h = parse_history(R"({"format": "aki-bundled-history", "version": 1,
        "previous": {"q-warrior": ["974f489db7b79f83", "bad", 12], "q-monk": "notalist", "q-robot": ["0000000000000007"]}})");
    CHECK(h.size() == 2);
    CHECK(h.at("q-warrior").size() == 1 && h.at("q-warrior")[0] == 0x974f489db7b79f83ULL);
    CHECK(h.at("q-robot")[0] == 7);
    CHECK(parse_history("not json").empty());
    CHECK(parse_history("{}").empty());
}
