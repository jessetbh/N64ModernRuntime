// The RT64::TextureNameProvider for Rice / GLideN64 and Jabo Direct3D8 pack names (aki-roadmap#93): computes both names
// of a texture from what rt64 gathered, and looks them up per pack, Rice names first.

#include "wcw_texnames.h"
#include "wcw_provider.h"
#include "wcw_jabo.h"
#include "wcw_rice.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace wcw_texnames {
    static_assert(sizeof(WcwRice::TextureKey) <= sizeof(RT64::TextureNameKey::bytes));
    static_assert(std::is_trivially_copyable_v<WcwRice::TextureKey>);

    WcwRice::TextureKey decodeKey(const RT64::TextureNameKey &key) {
        WcwRice::TextureKey decoded;
        memcpy(&decoded, key.bytes, sizeof(decoded));
        return decoded;
    }

    static std::vector<WcwRice::PaletteLoad> paletteLoadsOf(const RT64::TextureNameInput &input) {
        std::vector<WcwRice::PaletteLoad> loads(input.paletteLoadCount);
        for (size_t i = 0; i < input.paletteLoadCount; i++) {
            loads[i].firstEntry = input.paletteLoads[i].firstEntry;
            loads[i].count = input.paletteLoads[i].count;
            loads[i].address = input.paletteLoads[i].address;
            loads[i].sequence = input.paletteLoads[i].sequence;
        }

        return loads;
    }

    // The texture's Jabo Direct3D8 name (wcw_jabo.h): its texels at the drawn width and height, read from the RDRAM last
    // loaded into its TMEM address (rows as Gadge64's computeJaboKey derives them), with the CI palette from the recorded
    // TLUT loads. RDRAM is byte-swapped in 32-bit words, so byte a is RDRAM[a ^ 3].
    static void computeJaboKey(const RT64::TextureNameInput &input, WcwRice::TextureKey &key) {
        const RT64::TextureNameDrawTile &drawTile = input.drawTile;
        const RT64::TextureNameLoad &load = input.load;
        const uint32_t width = input.width;
        const uint32_t height = input.height;
        if ((width == 0) || (height == 0) || (width > 1024) || (height > 1024) || (drawTile.siz > WcwJabo::Siz16b)) {
            return;
        }

        const uint32_t loadBytesPerRow = uint32_t(load.textureWidth) << load.textureSiz >> 1;
        const uint32_t startOffset = uint32_t(load.uls >> 2) << load.textureSiz >> 1;
        uint32_t start = 0;
        uint32_t rowStride = 0;
        if (load.type == RT64::TextureNameLoad::Type::Block) {
            start = load.address + startOffset + loadBytesPerRow * load.ult;
            rowStride = uint32_t(drawTile.line) << 3;
        }
        else if (load.type == RT64::TextureNameLoad::Type::Tile) {
            start = load.address + startOffset + loadBytesPerRow * (load.ult >> 2);
            rowStride = loadBytesPerRow;
        }
        else {
            return;
        }

        const uint32_t rowBytes = std::max(width << drawTile.siz >> 1, 1U);
        if ((uint64_t(start) + uint64_t(rowStride) * (height - 1) + rowBytes) > input.rdramBytes) {
            return;
        }

        std::vector<uint8_t> texels(size_t(rowBytes) * height);
        for (uint32_t y = 0; y < height; y++) {
            const uint32_t row = start + y * rowStride;
            for (uint32_t x = 0; x < rowBytes; x++) {
                texels[size_t(y) * rowBytes + x] = input.rdram[(row + x) ^ 3];
            }
        }

        uint8_t palette[512] = {};
        const uint8_t *tilePalette = nullptr;
        if (drawTile.fmt == WcwJabo::FmtCI) {
            const std::vector<WcwRice::PaletteLoad> paletteLoads = paletteLoadsOf(input);
            for (uint32_t e = 0; e < 256; e++) {
                uint32_t address = 0;
                if (WcwRice::paletteEntryAddress(paletteLoads, e, address) && ((uint64_t(address) + 2) <= input.rdramBytes)) {
                    palette[e * 2] = input.rdram[address ^ 3];
                    palette[e * 2 + 1] = input.rdram[(address + 1) ^ 3];
                }
            }

            tilePalette = (drawTile.siz == WcwJabo::Siz4b) ? &palette[uint32_t(drawTile.palette & 0xF) * 32] : palette;
        }

        WcwJabo::Digest digest;
        if (!WcwJabo::textureDigest(drawTile.fmt, drawTile.siz, texels.data(), width, height, rowBytes, tilePalette, digest)) {
            return;
        }

        memcpy(key.jabo, digest.bytes, sizeof(key.jabo));
        key.jaboValid = true;
        if (WcwRice::logEnabled()) {
            static uint32_t logged = 0;
            if (++logged <= 5000) {
                fprintf(stderr, "[wcw jabo] key %s (%ux%u, fmt %u, siz %u)\n", WcwJabo::toHex(digest).c_str(), width, height, drawTile.fmt, drawTile.siz);
            }
        }
    }

    // The texture's Rice / GLideN64 key, from the RDRAM last loaded into its TMEM address, as GLideN64's _loadHiresTexture
    // computes it (see wcw_rice.h). Invalid when there's no such load or it reaches outside RDRAM.
    static void computeRiceKey(const RT64::TextureNameInput &input, WcwRice::TextureKey &result) {
        const RT64::TextureNameDrawTile &drawTile = input.drawTile;
        WcwRice::Load load;
        switch (input.load.type) {
        case RT64::TextureNameLoad::Type::Tile:
            load.type = WcwRice::LoadType::Tile;
            break;
        case RT64::TextureNameLoad::Type::Block:
            load.type = WcwRice::LoadType::Block;
            break;
        default:
            return;
        }

        load.siz = input.load.siz;
        load.masks = input.load.masks;
        load.maskt = input.load.maskt;
        load.uls = input.load.uls;
        load.ult = input.load.ult;
        load.lrs = input.load.lrs;
        load.lrt = input.load.lrt;
        load.address = input.load.address;
        load.textureSiz = input.load.textureSiz;
        load.textureWidth = input.load.textureWidth;

        WcwRice::DrawTile draw;
        draw.fmt = drawTile.fmt;
        draw.siz = drawTile.siz;
        draw.line = drawTile.line;
        draw.palette = drawTile.palette;
        draw.cms = drawTile.cms;
        draw.cmt = drawTile.cmt;
        draw.masks = drawTile.masks;
        draw.maskt = drawTile.maskt;
        draw.uls = drawTile.uls;
        draw.ult = drawTile.ult;
        draw.lrs = drawTile.lrs;
        draw.lrt = drawTile.lrt;

        WcwRice::Geometry geometry;
        if (!WcwRice::computeGeometry(load, draw, geometry)) {
            return;
        }

        // The CRC may read up to 4 bytes before the start (rows under 4 bytes, see riceCRC32).
        if ((geometry.rdramStart < 4) || ((uint64_t(geometry.rdramStart) + geometry.bytesRead + 4) > input.rdramBytes)) {
            return;
        }

        const uint8_t *src = &input.rdram[geometry.rdramStart];
        result.crc = WcwRice::riceCRC32(src, geometry.width, geometry.height, drawTile.siz, geometry.bytesPerRow);
        result.fmt = drawTile.fmt;
        result.siz = drawTile.siz;
        result.hasPalette = WcwRice::usesPalette(drawTile.fmt, drawTile.siz, input.tlut != 0);
        if (result.hasPalette) {
            const bool ci4 = (drawTile.siz == 0);
            const uint32_t ciMax = ci4 ? WcwRice::maxCI4(src, geometry.width, geometry.height, geometry.bytesPerRow) : WcwRice::maxCI8(src, geometry.width, geometry.height, geometry.bytesPerRow);
            const std::vector<WcwRice::PaletteLoad> paletteLoads = paletteLoadsOf(input);

            // GLideN64's 256-entry palette copy, with a zero guard in front, for the entries the CRC reads.
            const uint32_t firstEntry = ci4 ? (uint32_t(drawTile.palette & 0xF) << 4) : 0;
            const uint32_t entryCount = std::min(ciMax + 1, 256U - firstEntry);
            uint8_t palette[4 + 512] = {};
            const uint32_t fromEntry = (firstEntry >= 2) ? (firstEntry - 2) : 0;
            for (uint32_t e = fromEntry; e < (firstEntry + entryCount); e++) {
                uint32_t address = 0;
                if (WcwRice::paletteEntryAddress(paletteLoads, e, address) && ((uint64_t(address) + 2) <= input.rdramBytes)) {
                    palette[4 + e * 2] = input.rdram[address];
                    palette[4 + e * 2 + 1] = input.rdram[address + 1];
                }
            }

            result.paletteCrc = WcwRice::paletteCRC(palette, firstEntry, entryCount, ci4);
        }

        result.valid = true;
        if (WcwRice::logEnabled()) {
            static uint32_t logged = 0;
            if (++logged <= 5000) {
                fprintf(stderr, "[wcw rice] key %s (%dx%d, bpl %d, rdram %08x, load %s)\n", WcwRice::textureKeyToString(result).c_str(),
                    geometry.width, geometry.height, geometry.bytesPerRow, geometry.rdramStart, (load.type == WcwRice::LoadType::Tile) ? "tile" : "block");
            }
        }
    }

    // Textures matched since the packs were loaded, for the logs.
    static std::atomic<uint32_t> riceMatches{ 0 };
    static std::atomic<uint32_t> jaboMatches{ 0 };

    class PackIndex : public RT64::TextureNamePackIndex {
    public:
        WcwRice::Index rice;
        WcwJabo::Index jabo;

        const std::string *find(const RT64::TextureNameKey &key, uint64_t textureHash, int packIndex) const override {
            const WcwRice::TextureKey names = decodeKey(key);
            if (names.valid) {
                const std::string *path = rice.empty() ? nullptr : rice.find(names.crc, names.paletteCrc, names.fmt, names.siz, names.hasPalette);
                if (path != nullptr) {
                    const uint32_t matches = ++riceMatches;
                    if (WcwRice::logEnabled() || (matches <= 10) || ((matches % 100) == 0)) {
                        fprintf(stderr, "[wcw rice] match #%u %s %016llx -> pack %d %s\n", matches, WcwRice::textureKeyToString(names).c_str(), (unsigned long long)(textureHash), packIndex, path->c_str());
                    }

                    return path;
                }
            }

            if (names.jaboValid) {
                WcwJabo::Digest digest;
                memcpy(digest.bytes, names.jabo, sizeof(digest.bytes));
                const std::string *path = jabo.empty() ? nullptr : jabo.find(digest);
                if (path != nullptr) {
                    const uint32_t matches = ++jaboMatches;
                    if (WcwRice::logEnabled() || (matches <= 10) || ((matches % 100) == 0)) {
                        fprintf(stderr, "[wcw jabo] match #%u %s %016llx -> pack %d %s\n", matches, WcwJabo::toHex(digest).c_str(), (unsigned long long)(textureHash), packIndex, path->c_str());
                    }

                    return path;
                }
            }

            return nullptr;
        }

        bool separateAlpha(const std::string &relativePath, const std::string *&alphaPath) const override {
            alphaPath = nullptr;
            if (!WcwRice::isRgbFile(relativePath)) {
                return false;
            }

            alphaPath = rice.alphaFor(relativePath);
            return true;
        }
    };

    class Provider : public RT64::TextureNameProvider {
    public:
        void computeKey(const RT64::TextureNameInput &input, RT64::TextureNameKey &key) const override {
            WcwRice::TextureKey names;
            if ((input.rdram != nullptr) && (input.load.address != 0)) {
                computeRiceKey(input, names);
                computeJaboKey(input, names);
            }

            key.valid = names.valid || names.jaboValid;
            memcpy(key.bytes, &names, sizeof(names));
        }

        // In case-insensitive path order, so the first file of a duplicated key wins as in GLideN64. Any pack can hold
        // these names; rt64.json needs no entries.
        std::unique_ptr<RT64::TextureNamePackIndex> createPackIndex(int packIndex, const std::vector<std::string> &relativePaths) const override {
            if (packIndex == 0) {
                riceMatches = 0;
                jaboMatches = 0;
            }

            std::vector<std::string> sortedPaths = relativePaths;
            std::sort(sortedPaths.begin(), sortedPaths.end(), WcwRice::pathLess);

            std::unique_ptr<PackIndex> index = std::make_unique<PackIndex>();
            size_t riceNames = 0;
            size_t jaboNames = 0;
            for (const std::string &relativePath : sortedPaths) {
                if (relativePath.find('#') != std::string::npos) {
                    riceNames += index->rice.add(relativePath) ? 1 : 0;
                }
                else {
                    jaboNames += index->jabo.add(relativePath) ? 1 : 0;
                }
            }

            if (riceNames > 0) {
                fprintf(stderr, "[wcw rice] pack %d: %zu Rice-named files, %zu keys indexed\n", packIndex, riceNames, index->rice.size());
            }

            if (jaboNames > 0) {
                fprintf(stderr, "[wcw jabo] pack %d: %zu Jabo-named files, %zu digests indexed\n", packIndex, jaboNames, index->jabo.size());
            }

            if ((riceNames == 0) && (jaboNames == 0)) {
                return nullptr;
            }

            return index;
        }

        void applySeparateAlpha(uint8_t *rgba, const uint8_t *alphaRgba, size_t pixelCount) const override {
            if (alphaRgba != nullptr) {
                WcwRice::mergeAlpha(rgba, alphaRgba, pixelCount);
            }
            else {
                WcwRice::makeOpaque(rgba, pixelCount);
            }
        }
    };

    const RT64::TextureNameProvider &provider() {
        static const Provider instance;
        return instance;
    }
};
