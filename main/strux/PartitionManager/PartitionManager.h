#pragma once

#include "StruxProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include <esp_ota_ops.h>

class Stream;

class PartitionManager {
    static constexpr const char* TAG = "PartitionManager";

public:
    explicit PartitionManager(StruxProvider& strux);

    PartitionManager(const PartitionManager&) = delete;
    PartitionManager& operator=(const PartitionManager&) = delete;

    void Init();

    // Everything else is commands: the manager's entire external
    // surface is its command table below.

private:
    StruxProvider& strux_;
    InitState initState_;

    // One mechanism for ANY partition, addressed by label — see PartitionWriter.
    // An upload is one streamed command (writePartition): the handler drains its
    // input stream straight into a PartitionWriter, finalize runs at end-of-stream.
    // There is no cross-request channel state; the transport carries the whole
    // image within one command.

    const char* GetRunningPartition() const;
    const char* GetNextPartition() const;

    // ── Partition inspection ──────────────────────────────────

    struct PartitionInfo
    {
        char     label[17];     // esp_partition_t::label is char[17] — 16 + NUL
        char     type[8];       // "app" or "data"
        char     subtype[16];   // "ota_0" / "fat" / "nvs" / "0xNN" …
        uint32_t offset;
        uint32_t size;
        bool     running;
        bool     nextOta;
        bool     uploadable;    // safe to overwrite via upload
        char     version[32];   // app partitions only; empty otherwise
    };

    /// Enumerate all partitions into `out`. Returns count written.
    int GetPartitions(PartitionInfo* out, int maxCount) const;

    // ── Commands (registered with CommandManager in Init) ──
    CommandResult Cmd_UpdateStatus(CommandContext& ctx);
    CommandResult Cmd_Partitions(CommandContext& ctx);
    /// Streamed upload: header line + body. `offset` is the start address, default 0.
    /// Writes exactly where told, erases nothing, activates nothing. The sender
    /// calls clearPartition first and activatePartition after the last piece, and
    /// may leave gaps between pieces for other traffic.
    ///
    /// Pieces exist because a single command that runs for tens of seconds
    /// monopolises the transport, which is what the relay's in-flight timeout trips
    /// over. Many short commands need no concurrency support to coexist with others.
    CommandResult Cmd_WritePartition(CommandContext& ctx);
    CommandResult Cmd_DownloadPartition(CommandContext& ctx);

    /// Erase a partition whole, so a chunked upload starts from a known state.
    CommandResult Cmd_ClearPartition(CommandContext& ctx);

    /// Validate an app image and make it the next boot slot. No-op for data.
    CommandResult Cmd_ActivatePartition(CommandContext& ctx);

    // Defined in PartitionManager.cpp, beside the handlers and the argument
    // descriptors they read. A static data member's DEFINITION is in the class's
    // scope, so it may name these private handlers from namespace scope in the .cpp;
    // that is what keeps every command's metadata next to the code it describes.
    static CommandEntry statusCommand_;
    static CommandEntry listCommand_;
    static CommandEntry writeCommand_;
    static CommandEntry clearCommand_;
    static CommandEntry activateCommand_;
    static CommandEntry readCommand_;
};
