// Unit tests for the texnames provider (aki-roadmap#93): the keys computed over synthetic RDRAM, the per-pack lookup and
// the separate alpha rule. No GPU, game or pack needed.

#include "wcw_texnames.h"
#include "wcw_provider.h"
#include "wcw_jabo.h"
#include "wcw_rice.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "wcw_test.h"

using RT64::TextureNameInput;
using RT64::TextureNameKey;
using RT64::TextureNameLoad;
using RT64::TextureNamePaletteLoad;
using namespace wcw_texnames;

namespace {
    constexpr uint8_t TestFmtRGBA = 0, TestFmtCI = 2;
    constexpr uint8_t TestSiz4b = 0, TestSiz8b = 1, TestSiz16b = 2;

    // RDRAM as the recomp keeps it: byte a of the N64's memory is at [a ^ 3].
    struct Rdram {
        std::vector<uint8_t> bytes = std::vector<uint8_t>(0x10000, 0);

        void put(uint32_t address, uint8_t value) { bytes[address ^ 3] = value; }
    };

    // A tile load of a width x height texture at 'address', drawn with the same tile.
    TextureNameInput tileInput(const Rdram &rdram, uint32_t address, uint8_t fmt, uint8_t siz, uint16_t width, uint16_t height) {
        TextureNameInput input;
        input.rdram = rdram.bytes.data();
        input.rdramBytes = rdram.bytes.size();
        input.load.type = TextureNameLoad::Type::Tile;
        input.load.siz = siz;
        input.load.lrs = uint16_t((width - 1) << 2);
        input.load.lrt = uint16_t((height - 1) << 2);
        input.load.address = address;
        input.load.textureSiz = siz;
        input.load.textureWidth = width;
        input.drawTile.fmt = fmt;
        input.drawTile.siz = siz;
        input.drawTile.lrs = input.load.lrs;
        input.drawTile.lrt = input.load.lrt;
        input.width = width;
        input.height = height;
        return input;
    }

    Rdram gradientRdram() {
        Rdram rdram;
        for (uint32_t i = 0; i < 256; i++) {
            rdram.put(0x1000 + i, uint8_t((i * 7) + 1));
        }

        return rdram;
    }
}

TEST_CASE("provider: an RGBA16 tile load gets the Rice CRC of its RDRAM and the MD5 of its decoded texels") {
    const Rdram rdram = gradientRdram();
    const TextureNameInput input = tileInput(rdram, 0x1000, TestFmtRGBA, TestSiz16b, 4, 4);

    TextureNameKey key;
    provider().computeKey(input, key);
    const WcwRice::TextureKey names = decodeKey(key);
    CHECK(key.valid);
    CHECK(names.valid);
    CHECK(!names.hasPalette);
    CHECK(names.fmt == TestFmtRGBA);
    CHECK(names.siz == TestSiz16b);
    CHECK(names.crc == WcwRice::riceCRC32(&rdram.bytes[0x1000], 4, 4, TestSiz16b, 8));

    uint8_t texels[32];
    for (uint32_t i = 0; i < 32; i++) {
        texels[i] = rdram.bytes[(0x1000 + i) ^ 3];
    }

    WcwJabo::Digest digest;
    CHECK(WcwJabo::textureDigest(TestFmtRGBA, TestSiz16b, texels, 4, 4, 8, nullptr, digest));
    CHECK(names.jaboValid);
    CHECK(memcmp(names.jabo, digest.bytes, 16) == 0);
}

TEST_CASE("provider: a CI8 texture's palette CRC reads the entries it uses, from the newest TLUT load of each") {
    Rdram rdram;
    // Texels using indices 0 and 1.
    for (uint32_t i = 0; i < 16; i++) {
        rdram.put(0x1000 + i, uint8_t(i & 1));
    }

    // The old load's two entries are all 0xAA, the newer one's are distinct.
    for (uint32_t i = 0; i < 4; i++) {
        rdram.bytes[0x2000 + i] = 0xAA;
    }

    rdram.bytes[0x3000] = 0x12;
    rdram.bytes[0x3001] = 0x34;
    rdram.bytes[0x3002] = 0x56;
    rdram.bytes[0x3003] = 0x78;

    TextureNameInput input = tileInput(rdram, 0x1000, TestFmtCI, TestSiz8b, 4, 4);
    const TextureNamePaletteLoad loads[2] = { { 0, 256, 0x2000, 1 }, { 0, 256, 0x3000, 2 } };
    input.paletteLoads = loads;
    input.paletteLoadCount = 2;

    TextureNameKey key;
    provider().computeKey(input, key);
    const WcwRice::TextureKey names = decodeKey(key);
    CHECK(names.valid);
    CHECK(names.hasPalette);

    uint8_t palette[4 + 512] = {};
    memcpy(&palette[4], &rdram.bytes[0x3000], 4);
    CHECK(names.paletteCrc == WcwRice::paletteCRC(palette, 0, 2, false));
    CHECK(names.jaboValid);

    // Flipping which load is newer changes both names.
    const TextureNamePaletteLoad swapped[2] = { { 0, 256, 0x2000, 2 }, { 0, 256, 0x3000, 1 } };
    input.paletteLoads = swapped;
    TextureNameKey other;
    provider().computeKey(input, other);
    const WcwRice::TextureKey otherNames = decodeKey(other);
    CHECK(otherNames.paletteCrc != names.paletteCrc);
    CHECK(memcmp(otherNames.jabo, names.jabo, 16) != 0);
}

TEST_CASE("provider: a CI4 texture's Jabo name uses the tile's palette bank") {
    Rdram rdram;
    for (uint32_t i = 0; i < 8; i++) {
        rdram.put(0x1000 + i, 0x01);
    }

    for (uint32_t i = 0; i < 512; i++) {
        rdram.put(0x2000 + i, uint8_t(i));
    }

    TextureNameInput input = tileInput(rdram, 0x1000, TestFmtCI, TestSiz4b, 4, 4);
    const TextureNamePaletteLoad loads[1] = { { 0, 256, 0x2000, 1 } };
    input.paletteLoads = loads;
    input.paletteLoadCount = 1;

    TextureNameKey bank0, bank1;
    provider().computeKey(input, bank0);
    input.drawTile.palette = 1;
    provider().computeKey(input, bank1);
    CHECK(decodeKey(bank0).jaboValid);
    CHECK(decodeKey(bank1).jaboValid);
    CHECK(memcmp(decodeKey(bank0).jabo, decodeKey(bank1).jabo, 16) != 0);
}

TEST_CASE("provider: no load, no address, or a load past the end of RDRAM gives no key and reads nothing out of bounds") {
    const Rdram rdram = gradientRdram();
    TextureNameKey key;

    TextureNameInput none = tileInput(rdram, 0x1000, TestFmtRGBA, TestSiz16b, 4, 4);
    none.load.type = TextureNameLoad::Type::None;
    provider().computeKey(none, key);
    CHECK(!key.valid);

    TextureNameInput noAddress = tileInput(rdram, 0, TestFmtRGBA, TestSiz16b, 4, 4);
    provider().computeKey(noAddress, key);
    CHECK(!key.valid);

    TextureNameInput pastEnd = tileInput(rdram, 0xFFF0, TestFmtRGBA, TestSiz16b, 32, 32);
    provider().computeKey(pastEnd, key);
    CHECK(!key.valid);

    TextureNameInput noRdram = tileInput(rdram, 0x1000, TestFmtRGBA, TestSiz16b, 4, 4);
    noRdram.rdram = nullptr;
    provider().computeKey(noRdram, key);
    CHECK(!key.valid);
}

TEST_CASE("provider: a 32-bit texture has a Rice name only (Jabo's rule for them isn't known)") {
    const Rdram rdram = gradientRdram();
    TextureNameKey key;
    provider().computeKey(tileInput(rdram, 0x1000, TestFmtRGBA, 3, 4, 4), key);
    CHECK(key.valid);
    CHECK(decodeKey(key).valid);
    CHECK(!decodeKey(key).jaboValid);
}

TEST_CASE("provider: a pack with neither kind of name has no index") {
    CHECK(provider().createPackIndex(0, { "rt64.json", "a/0123456789abcdef.dds", "readme.txt" }) == nullptr);
    CHECK(provider().createPackIndex(0, {}) == nullptr);
}

TEST_CASE("provider: a pack's Rice name beats its Jabo name for the same texture; the Jabo name is the fallback") {
    const Rdram rdram = gradientRdram();
    TextureNameKey key;
    provider().computeKey(tileInput(rdram, 0x1000, TestFmtRGBA, TestSiz16b, 4, 4), key);
    const WcwRice::TextureKey names = decodeKey(key);

    char riceName[64];
    snprintf(riceName, sizeof(riceName), "G#%08X#0#2_all.png", names.crc);
    WcwJabo::Digest digest;
    memcpy(digest.bytes, names.jabo, 16);
    const std::string jaboName = WcwJabo::toHex(digest) + ".png";

    const auto both = provider().createPackIndex(0, { jaboName, riceName });
    CHECK(both != nullptr);
    const std::string *found = both->find(key, 0x1234, 0);
    CHECK((found != nullptr) && (*found == riceName));

    const auto jaboOnly = provider().createPackIndex(0, { jaboName });
    found = jaboOnly->find(key, 0x1234, 0);
    CHECK((found != nullptr) && (*found == jaboName));

    const auto other = provider().createPackIndex(0, { "G#00000001#0#2_all.png" });
    CHECK(other->find(key, 0x1234, 0) == nullptr);
    CHECK(other->find(TextureNameKey(), 0x1234, 0) == nullptr);
}

TEST_CASE("provider: an _rgb file takes its alpha from the _a twin in the same folder; without one it is opaque") {
    const auto index = provider().createPackIndex(0, { "P/G#11111111#0#2_rgb.png", "P/G#11111111#0#2_a.png", "P/G#22222222#0#2_rgb.png", "P/G#33333333#0#2_all.png" });
    CHECK(index != nullptr);

    const std::string *alpha = nullptr;
    CHECK(index->separateAlpha("P/G#11111111#0#2_rgb.png", alpha));
    CHECK((alpha != nullptr) && (*alpha == "P/G#11111111#0#2_a.png"));
    CHECK(index->separateAlpha("P/G#22222222#0#2_rgb.png", alpha));
    CHECK(alpha == nullptr);
    CHECK(!index->separateAlpha("P/G#33333333#0#2_all.png", alpha));
    CHECK(alpha == nullptr);

    uint8_t rgba[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    const uint8_t alphaRgba[8] = { 200, 0, 0, 0, 100, 0, 0, 0 };
    provider().applySeparateAlpha(rgba, alphaRgba, 2);
    CHECK(rgba[3] == 200);
    CHECK(rgba[7] == 100);
    provider().applySeparateAlpha(rgba, nullptr, 2);
    CHECK(rgba[3] == 0xFF);
    CHECK(rgba[7] == 0xFF);
}

int main() {
    return WcwTest::runAll("texnames tests");
}
