#include "WebAssets.h"

#include <cstring>
#include <esp_log.h>

static const char* TAG = "WebAssets";

// Emitted by the EMBED_FILES entry in main/CMakeLists.txt.
extern const uint8_t kWebAssetsStart[] asm("_binary_web_assets_bin_start");
extern const uint8_t kWebAssetsEnd[] asm("_binary_web_assets_bin_end");

namespace
{
    constexpr uint8_t  MAGIC[4] = { 'S', 'X', 'W', 'A' };
    constexpr uint16_t VERSION = 1;

    constexpr uint32_t HEADER_SIZE = 32;
    constexpr uint32_t ENTRY_SIZE = 120;
    constexpr uint32_t NAME_SIZE = 96;

    constexpr uint32_t FLAG_GZIP = 1u << 0;

    // Field offsets, mirroring the struct formats in pack_web_assets.py.
    constexpr uint32_t HDR_MAGIC = 0;
    constexpr uint32_t HDR_VERSION = 4;
    constexpr uint32_t HDR_COUNT = 6;
    constexpr uint32_t HDR_DIR_OFFSET = 8;
    constexpr uint32_t HDR_DATA_OFFSET = 12;
    constexpr uint32_t HDR_TOTAL_SIZE = 16;
    constexpr uint32_t HDR_BUNDLE_HASH = 20;

    constexpr uint32_t ENT_NAME = 0;
    constexpr uint32_t ENT_OFFSET = 96;
    constexpr uint32_t ENT_LENGTH = 100;
    constexpr uint32_t ENT_FLAGS = 104;
    constexpr uint32_t ENT_HASH = 108;

    // The blob is byte-packed, so a 32-bit field can land unaligned — and an
    // unaligned word load from flash-mapped rodata faults on Xtensa. memcpy is
    // the portable way to say "read these four bytes"; the compiler folds it into
    // the right instructions.
    uint32_t ReadU32(const uint8_t* at)
    {
        uint32_t value;
        memcpy(&value, at, sizeof(value));
        return value;
    }

    uint16_t ReadU16(const uint8_t* at)
    {
        uint16_t value;
        memcpy(&value, at, sizeof(value));
        return value;
    }
}

WebAssetTable::WebAssetTable()
{
    blob_ = kWebAssetsStart;
    blobSize_ = static_cast<uint32_t>(kWebAssetsEnd - kWebAssetsStart);

    if (blobSize_ < HEADER_SIZE ||
        memcmp(blob_ + HDR_MAGIC, MAGIC, sizeof(MAGIC)) != 0 ||
        ReadU16(blob_ + HDR_VERSION) != VERSION)
    {
        ESP_LOGE(TAG, "embedded blob is not a v%u SXWA archive (%lu bytes)",
                 VERSION, static_cast<unsigned long>(blobSize_));
        return;
    }

    entryCount_ = ReadU16(blob_ + HDR_COUNT);
    dirOffset_ = ReadU32(blob_ + HDR_DIR_OFFSET);

    const uint32_t dataOffset = ReadU32(blob_ + HDR_DATA_OFFSET);
    const uint32_t totalSize = ReadU32(blob_ + HDR_TOTAL_SIZE);

    // The header describes the blob; the blob is what the linker actually gave
    // us. Disagreement means a truncated or mismatched embed, and every offset
    // below would be read on trust — so stop here instead.
    if (totalSize != blobSize_ ||
        dirOffset_ != HEADER_SIZE ||
        dataOffset != HEADER_SIZE + entryCount_ * ENTRY_SIZE ||
        dataOffset > blobSize_)
    {
        ESP_LOGE(TAG, "embedded blob header is inconsistent (%lu entries, %lu of %lu bytes)",
                 static_cast<unsigned long>(entryCount_),
                 static_cast<unsigned long>(totalSize),
                 static_cast<unsigned long>(blobSize_));
        entryCount_ = 0;
        return;
    }

    // Validate every entry once, here, so that At()/Find() can hand out raw
    // pointers without re-checking on each call — and so a bad blob is one log
    // line at boot rather than a fault at the moment someone asks for a file.
    for (uint32_t i = 0; i < entryCount_; ++i)
    {
        const uint8_t* entry = blob_ + dirOffset_ + i * ENTRY_SIZE;
        const uint32_t offset = ReadU32(entry + ENT_OFFSET);
        const uint32_t length = ReadU32(entry + ENT_LENGTH);

        // The name is a fixed field the packer NUL-pads; without a terminator in
        // it, WebFile::name would not be a C string.
        if (entry[ENT_NAME + NAME_SIZE - 1] != 0 ||
            offset < dataOffset || offset > blobSize_ || length > blobSize_ - offset)
        {
            ESP_LOGE(TAG, "embedded blob entry %lu is out of bounds",
                     static_cast<unsigned long>(i));
            entryCount_ = 0;
            return;
        }

        storedBytes_ += length;
    }

    bundleHash_ = blob_ + HDR_BUNDLE_HASH;
    valid_ = true;
}

bool WebAssetTable::At(uint32_t index, WebFile& out) const
{
    if (!valid_ || index >= entryCount_)
    {
        return false;
    }

    const uint8_t* entry = blob_ + dirOffset_ + index * ENTRY_SIZE;

    out.name = reinterpret_cast<const char*>(entry + ENT_NAME);
    out.data = blob_ + ReadU32(entry + ENT_OFFSET);
    out.size = ReadU32(entry + ENT_LENGTH);
    out.gzipped = (ReadU32(entry + ENT_FLAGS) & FLAG_GZIP) != 0;
    out.hash = entry + ENT_HASH;
    return true;
}

bool WebAssetTable::Find(const char* name, WebFile& out) const
{
    if (name == nullptr)
    {
        return false;
    }

    // Linear: a bundle is a handful of files, and a scan over flash beats
    // carrying an index for it.
    for (uint32_t i = 0; i < entryCount_; ++i)
    {
        const char* candidate =
            reinterpret_cast<const char*>(blob_ + dirOffset_ + i * ENTRY_SIZE + ENT_NAME);
        if (strcmp(candidate, name) == 0)
        {
            return At(i, out);
        }
    }
    return false;
}

WebFile WebAssetIterator::operator*() const
{
    WebFile file = {};
    table_.At(index_, file);
    return file;
}

const WebAssetTable& WebAssets()
{
    static const WebAssetTable table;
    return table;
}
