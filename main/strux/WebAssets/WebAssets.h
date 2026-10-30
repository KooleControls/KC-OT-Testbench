#pragma once

#include <cstdint>

// ──────────────────────────────────────────────────────────────
// The built frontend, embedded in the application binary.
//
// The whole www/ tree is packed into one blob at build time (see
// pack_web_assets.py) and embedded as a single EMBED_FILES entry. That is what
// lets vite keep content-hashed filenames: nothing in CMake or C++ names an
// individual asset, so the bundle can change shape freely between builds.
//
// The data lives in flash-mapped rodata — reading it costs no RAM, and the
// pointers handed out here point straight into flash. Nothing is copied,
// nothing is decompressed: a gzipped file is handed over gzipped, and whoever
// sends it onward is responsible for saying so.
//
// This is deliberately not a manager: there is no state, no Init(), no
// StruxProvider. It is a const lookup table over a blob the linker placed, which
// is why it sits in its own folder rather than inside WebServerManager — the
// local HTTP route and the `web read` command are merely its first two
// consumers, and neither owns it.
// ──────────────────────────────────────────────────────────────

struct WebFile
{
    const char*    name;     // path relative to www/, e.g. "assets/index-DWtLor9m.js"
    const uint8_t* data;     // stored bytes, in flash
    uint32_t       size;     // stored size — compressed size when `gzipped`
    bool           gzipped;  // the stored bytes are a gzip stream
    const uint8_t* hash;     // 8 bytes over the stored bytes, for cache validation
};

class WebAssetTable;

// Forward iterator over the blob's directory. Holds an index, materialises a
// WebFile on dereference — there is no array of WebFile anywhere, the entries
// are read from flash on demand.
class WebAssetIterator
{
public:
    WebAssetIterator(const WebAssetTable& table, uint32_t index) : table_(table), index_(index) {}

    WebFile operator*() const;
    WebAssetIterator& operator++() { ++index_; return *this; }
    bool operator!=(const WebAssetIterator& other) const { return index_ != other.index_; }

private:
    const WebAssetTable& table_;
    uint32_t index_;
};

class WebAssetTable
{
public:
    // False when the blob is missing, truncated or not the expected format. The
    // firmware still boots: a device whose UI failed to pack is degraded, not
    // broken — the WebSocket and every command still answer — so this reports
    // rather than aborts.
    bool Valid() const { return valid_; }

    uint32_t Count() const { return valid_ ? entryCount_ : 0; }

    // Total stored bytes across all files — what a transfer would have to move.
    uint32_t StoredBytes() const { return storedBytes_; }

    // 8 bytes identifying the whole bundle, or nullptr when invalid. Lets a
    // cache decide in one comparison that it already has everything.
    const uint8_t* BundleHash() const { return valid_ ? bundleHash_ : nullptr; }

    bool At(uint32_t index, WebFile& out) const;

    // Exact match on the stored name. No leading-slash tolerance, no index.html
    // fallback, no query-string stripping: those are serving conventions and
    // belong to whoever exposes these files over a wire (StaticFileHandler), not
    // to the table.
    bool Find(const char* name, WebFile& out) const;

    WebAssetIterator begin() const { return WebAssetIterator(*this, 0); }
    WebAssetIterator end() const { return WebAssetIterator(*this, Count()); }

private:
    friend const WebAssetTable& WebAssets();

    WebAssetTable();

    const uint8_t* blob_ = nullptr;
    uint32_t blobSize_ = 0;
    uint32_t entryCount_ = 0;
    uint32_t dirOffset_ = 0;
    uint32_t storedBytes_ = 0;
    const uint8_t* bundleHash_ = nullptr;
    bool valid_ = false;
};

// The one instance. Parsed and validated on first use.
const WebAssetTable& WebAssets();
