#pragma once

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <cstddef>
#include <cstdint>

// Writes a byte stream into ONE flash partition, addressed by label, at a caller
// chosen start offset.
//
// Raw writes throughout — app and data partitions alike — deliberately, so that
// nothing has to survive between requests. The OTA API's handle is the reason the
// old version could only do a whole image in a single command: it is opened and
// closed inside one call, and validation happens when it closes. Writing raw
// instead lets an upload be any number of independent commands, each resumed at an
// address, with gaps in between for other traffic.
//
// What that gives up: esp_ota_write() withholds the image's magic bytes until the
// end, so a half-written app image can never look bootable. Here that protection
// moves from impossible to *checked* — the boot slot is not switched until
// Activate(), and esp_ota_set_boot_partition validates the image before switching.
// Until then otadata still points at the old slot, so a partial image is inert.
//
// It never erases. That is Clear()'s job, and keeping them apart is what lets a write
// resume at any offset: erasing the sector a resumed write starts in would take the
// previous piece's tail with it. Writing over unerased flash produces a wrong image
// rather than an error — caught by Activate(), which validates before switching the
// boot slot.
class PartitionWriter
{
    static constexpr const char* TAG = "PartitionWriter";

public:
    /// Setup: find the partition and check it may be written. On failure *err points
    /// at a static reason string and ok() is false. `startOffset` is where the first
    /// write lands and need not be sector aligned.
    PartitionWriter(const char* label, size_t startOffset, const char** err);

    PartitionWriter(const PartitionWriter&)            = delete;
    PartitionWriter& operator=(const PartitionWriter&) = delete;

    bool   ok() const { return p_ != nullptr; }
    size_t written() const { return written_; }

    /// Append `size` bytes at the current position. False on flash error/overflow.
    bool write(const void* data, size_t size);

    /// Erase a whole partition, so an upload can start from a known state.
    /// Returns nullptr on success or a static error string.
    static const char* Clear(const char* label);

    /// Validate the image and make it the next boot slot. App partitions only — a
    /// no-op success for data, which has nothing to activate.
    /// Returns nullptr on success or a static error string.
    static const char* Activate(const char* label);

private:
    const esp_partition_t* p_        = nullptr;   // nullptr == setup failed
    size_t                 offset_   = 0;         // absolute position of the next write
    size_t                 written_  = 0;         // bytes written by THIS instance

    /// Shared lookup plus the "may we write this?" check (never the running slot).
    static const esp_partition_t* Resolve(const char* label, const char** err);
};
