//
// librecomp
//

// [wcw] Unit tests for librecomp/bundled_packs (the bundled starter model packs and their updates, aki-roadmap#149).
// Built and run as part of every librecomp build (LIBRECOMP_WCW_TESTS); a failure fails the build.

#include "librecomp/bundled_packs.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "wcw_test.h"

using namespace recomp::mods::bundled_packs;
namespace fs = std::filesystem;

namespace {
    const std::string BinBytes = std::string("\x00\x01\r\n", 4);

    void write(const fs::path &p, const std::string &data) {
        fs::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f << data;
    }

    std::string read(const fs::path &p) {
        std::ifstream f(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    std::string pack_json(const char *name, const char *version) {
        return std::string("{\n  \"id\": \"") + name + "\",\n  \"version\": \"" + version + "\"\n}\n";
    }

    // A fresh folder under the temp directory, removed when the test ends.
    struct TempDir {
        fs::path path;
        TempDir() {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path = fs::temp_directory_path() / ("wcw_bundled_packs_" + std::to_string(stamp));
            fs::create_directories(path);
        }
        ~TempDir() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };
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

TEST_CASE("bundled packs: dotfiles have no extension, as in pack_history.py (os.path.splitext)") {
    // ".md" and "..json" aren't text files there, so their CR LF is data on both sides.
    CHECK(pack_hash({ { "sub/.md", "a\r\n" } }) != pack_hash({ { "sub/.md", "a\n" } }));
    CHECK(pack_hash({ { "..json", "a\r\n" } }) != pack_hash({ { "..json", "a\n" } }));
    CHECK(pack_hash({ { "sub/notes.md", "a\r\n" } }) == pack_hash({ { "sub/notes.md", "a\n" } }));
}

TEST_CASE("bundled packs: install() on disk: update unedited, keep edited and own, skip deleted, recover an interrupted update") {
    TempDir tmp;
    const fs::path src = tmp.path / "assets_models";
    const fs::path dst = tmp.path / "mods_models";
    auto old_files = [](const char *id) {
        return std::vector<std::pair<std::string, std::string>>{ { "pack.json", pack_json(id, "1.0.0") }, { "model.bin", std::string("old-") + id } };
    };
    // The bundle: six packs at 1.1.0; a, b, c, d and f have an earlier 1.0.0 version.
    std::string history = "{\"previous\": {";
    for (const char *id : { "a", "b", "c", "d", "e", "f" }) {
        write(src / (std::string(id) + ".akimodel") / "pack.json", pack_json(id, "1.1.0"));
        write(src / (std::string(id) + ".akimodel") / "model.bin", std::string("new-") + id);
        if (std::string(id) != "e") {
            history += std::string(history.back() == '{' ? "" : ",") + "\"" + id + "\": [\"" + hash_hex(pack_hash(old_files(id))) + "\"]";
        }
    }
    write(src / HistoryFile, history + "}}");
    write(src / "CREDITS.md", "credits\n");
    // The player: a unedited 1.0.0 (CRLF, as a Windows checkout gave it), b edited, c deleted, d their own pack of that
    // name (not in .bundled), e never installed, f cut off mid-update (only f.akimodel.replaced is left).
    write(dst / "a.akimodel" / "pack.json", "{\r\n  \"id\": \"a\",\r\n  \"version\": \"1.0.0\"\r\n}\r\n");
    write(dst / "a.akimodel" / "model.bin", "old-a");
    write(dst / "b.akimodel" / "pack.json", pack_json("b", "1.0.0"));
    write(dst / "b.akimodel" / "model.bin", "old-b, edited");
    write(dst / "d.akimodel" / "pack.json", pack_json("d", "0.1.0"));
    write(dst / "d.akimodel" / "model.bin", "mine");
    write(dst / "f.akimodel.replaced" / "pack.json", pack_json("f", "1.0.0"));
    write(dst / "f.akimodel.replaced" / "model.bin", "old-f");
    write(dst / RecordFile, "a.akimodel\nb.akimodel\nc.akimodel\nf.akimodel\n");

    install(src, dst);
    CHECK(read(dst / "a.akimodel" / "model.bin") == "new-a");                  // updated
    CHECK(pack_version(read(dst / "a.akimodel" / "pack.json")) == "1.1.0");
    CHECK(read(dst / "b.akimodel" / "model.bin") == "old-b, edited");          // edited: kept
    CHECK(!fs::exists(dst / "c.akimodel"));                                    // deleted: stays deleted
    CHECK(read(dst / "d.akimodel" / "model.bin") == "mine");                   // the player's own: kept
    CHECK(read(dst / "e.akimodel" / "model.bin") == "new-e");                  // new: installed
    CHECK(read(dst / "f.akimodel" / "model.bin") == "new-f");                  // recovered, then updated
    CHECK(!fs::exists(dst / "f.akimodel.replaced"));
    CHECK(!fs::exists(dst / "a.akimodel.installing") && !fs::exists(dst / "a.akimodel.replaced"));
    CHECK(read(dst / "CREDITS.md") == "credits\n");
    const auto record = parse_record(read(dst / RecordFile));
    CHECK(record.size() == 6 && record.count("d.akimodel") && record.count("e.akimodel"));

    // A second start changes nothing.
    write(dst / "b.akimodel" / "note.txt", "player file");
    install(src, dst);
    CHECK(read(dst / "a.akimodel" / "model.bin") == "new-a");
    CHECK(read(dst / "b.akimodel" / "note.txt") == "player file");
    CHECK(!fs::exists(dst / "c.akimodel"));
    // Without a history file nothing is replaced: a fresh bundle only installs.
    fs::remove(src / HistoryFile);
    write(dst / "a.akimodel" / "model.bin", "old-a");
    write(dst / "a.akimodel" / "pack.json", pack_json("a", "1.0.0"));
    install(src, dst);
    CHECK(read(dst / "a.akimodel" / "model.bin") == "old-a");
}
