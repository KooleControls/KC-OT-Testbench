#include "PartitionManager.h"
#include "ReplyBody.h"
#include "PartitionWriter.h"
#include "CommandManager.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>
#include <cstdio>

PartitionManager::PartitionManager(StruxProvider& strux)
    : strux_(strux)
{
}

// --------------------------------------------------------------
// Arguments and the command table.
//
// The descriptors live here rather than in the header because they describe these
// handlers and nothing else, and because a table in the header would drag every
// argument's help text into everything that includes it. `partition` is declared
// twice on purpose: `write` has a warning to give about it that the read-only
// commands do not, and a description belongs to a declaration rather than to a name.
// --------------------------------------------------------------

namespace {

// esp_partition_t::label is char[17], so sixteen characters is the longest a label
// can be -- the number a caller needs, and now declared instead of deduced.
constexpr uint16_t LABEL_MAX = 16;

constexpr CommandArg<const char*> partitionArg{
    "partition", "Label of the partition, as 'partition list' reports it.", LABEL_MAX };

constexpr CommandArg<const char*> writePartitionArg{
    "partition", "Label of the partition to write, as 'partition list' reports it. "
                 "The running app slot is always refused.", LABEL_MAX };

constexpr CommandArg<uint32_t> offsetArg{
    "offset", "Byte offset to write at. Default 0. Use it to send an image in "
              "several pieces, each its own command. The write never erases or "
              "activates: run 'partition clear' before the first piece and "
              "'partition activate' after the last.", Presence::Optional };

constexpr CommandArg<bool> restartArg{
    "restart", "true to reboot into it now. Default false, which leaves the "
               "switch to take effect whenever the device next restarts. This "
               "command is the ONLY thing that changes which image boots - "
               "uploading one does not, and 'system reboot' always comes back "
               "into the same image it was running.", Presence::Optional };

} // namespace

CommandEntry PartitionManager::statusCommand_{
    "partition status", &InvokeCommand<&PartitionManager::Cmd_UpdateStatus>,
    "Report the running firmware version, which app slot it booted from, and "
    "which slot the next update would be written to."
};

CommandEntry PartitionManager::listCommand_{
    "partition list", &InvokeCommand<&PartitionManager::Cmd_Partitions>,
    "List the flash partitions with type, offset, size, and whether each is "
    "running, is the next OTA slot, or may be written to."
};

CommandEntry PartitionManager::writeCommand_{
    "partition write", &InvokeCommand<&PartitionManager::Cmd_WritePartition>,
    "Write an image to a partition. The bytes follow the request envelope in "
    "the same channel, so this is a streaming upload rather than an argument. "
    "Never erases or activates. Destructive: it overwrites what is in the "
    "partition, so 'partition clear' it first and 'partition activate' it after.",
    { &writePartitionArg, &offsetArg }
};

CommandEntry PartitionManager::clearCommand_{
    "partition clear", &InvokeCommand<&PartitionManager::Cmd_ClearPartition>,
    "Erase a partition. Destructive and immediate - the running app slot is "
    "refused, anything else is erased.",
    { &partitionArg }
};

CommandEntry PartitionManager::activateCommand_{
    "partition activate", &InvokeCommand<&PartitionManager::Cmd_ActivatePartition>,
    "Choose which app partition boots, and optionally reboot into it now. "
    "This is the only command that changes the boot slot: an upload leaves "
    "it alone, so a written image sits inert until this says otherwise, and "
    "'system reboot' returns to the same image every time. An image that "
    "does not validate is refused.",
    { &partitionArg, &restartArg }
};

CommandEntry PartitionManager::readCommand_{
    "partition read", &InvokeCommand<&PartitionManager::Cmd_DownloadPartition>,
    "Read a partition back. The reply is one header record - ok, size, and "
    "contentType application/octet-stream - then a newline, then that many "
    "raw bytes, streamed until the channel closes. Can be megabytes.",
    { &partitionArg }
};

void PartitionManager::Init()
{
    auto initAttempt = initState_.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    strux_.getCommandManager().Register(this, {
        &statusCommand_,
        &listCommand_,
        &writeCommand_,
        &clearCommand_,
        &activateCommand_,
        &readCommand_,
    });

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

// The 4 KB I/O buffer these two commands used to carry is gone rather than moved.
// It could not live on the stack — handlers run on whichever transport task
// dispatched them, and 4 KB of a few-KB stack scribbles on whatever follows (in the
// KC1245 fork that surfaced as a NULL semaphore inside lwIP's select teardown, with
// nothing in the backtrace pointing at the culprit). The heap fixed that overrun and
// bought a fragmentation problem plus an allocation that can fail mid-write.
//
// Neither is needed: the bytes are already in a buffer at both ends. An upload sits
// in the transport's inbound buffer, a download is assembled in its framing buffer,
// and Channel lends both out (Stream::canLend), so these handlers move bytes between
// flash and a buffer they do not own.

const char* PartitionManager::GetRunningPartition() const
{
    const esp_partition_t* p = esp_ota_get_running_partition();
    return p ? p->label : "unknown";
}

const char* PartitionManager::GetNextPartition() const
{
    const esp_partition_t* p = esp_ota_get_next_update_partition(nullptr);
    return p ? p->label : "none";
}

// ──────────────────────────────────────────────────────────────
// Partition enumeration
// ──────────────────────────────────────────────────────────────

int PartitionManager::GetPartitions(PartitionInfo* out, int maxCount) const
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* next    = esp_ota_get_next_update_partition(nullptr);

    int count = 0;
    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);

    while (it != nullptr && count < maxCount)
    {
        const esp_partition_t* p = esp_partition_get(it);
        PartitionInfo& info = out[count++];

        // snprintf, not strncpy: a partition label can fill our buffer exactly,
        // and strncpy would then leave it unterminated (-Wstringop-truncation).
        snprintf(info.label, sizeof(info.label), "%s", p->label);

        // Subtype values collide across types (e.g. APP_FACTORY and DATA_OTA are both 0x00),
        // so we branch by type first.
        if (p->type == ESP_PARTITION_TYPE_APP)
        {
            strcpy(info.type, "app");
            switch (p->subtype)
            {
                case ESP_PARTITION_SUBTYPE_APP_FACTORY: strcpy(info.subtype, "factory"); break;
                case ESP_PARTITION_SUBTYPE_APP_OTA_0:   strcpy(info.subtype, "ota_0"); break;
                case ESP_PARTITION_SUBTYPE_APP_OTA_1:   strcpy(info.subtype, "ota_1"); break;
                default: snprintf(info.subtype, sizeof(info.subtype), "0x%02x", (int)p->subtype);
            }
        }
        else
        {
            strcpy(info.type, "data");
            switch (p->subtype)
            {
                case ESP_PARTITION_SUBTYPE_DATA_OTA: strcpy(info.subtype, "ota"); break;
                case ESP_PARTITION_SUBTYPE_DATA_PHY: strcpy(info.subtype, "phy"); break;
                case ESP_PARTITION_SUBTYPE_DATA_NVS: strcpy(info.subtype, "nvs"); break;
                case ESP_PARTITION_SUBTYPE_DATA_FAT: strcpy(info.subtype, "fat"); break;
                default: snprintf(info.subtype, sizeof(info.subtype), "0x%02x", (int)p->subtype);
            }
        }

        info.offset = p->address;
        info.size   = p->size;
        info.running = (running && p == running);
        info.nextOta = (next && p == next);

        // Uploadable: any non-running OTA app slot. Nothing else — the frontend
        // travels inside the app image now, so there is no data partition on this
        // device a user is meant to write.
        info.uploadable = (p->type == ESP_PARTITION_TYPE_APP && !info.running);

        info.version[0] = '\0';
        if (p->type == ESP_PARTITION_TYPE_APP)
        {
            esp_app_desc_t desc;
            if (esp_ota_get_partition_description(p, &desc) == ESP_OK)
            {
                snprintf(info.version, sizeof(info.version), "%s", desc.version);
            }
        }

        it = esp_partition_next(it);
    }

    if (it != nullptr)
        esp_partition_iterator_release(it);

    return count;
}

// ──────────────────────────────────────────────────────────────
// Status / enumeration commands
// ──────────────────────────────────────────────────────────────

CommandResult PartitionManager::Cmd_UpdateStatus(CommandContext& ctx)
{
    auto resp = ctx.reply.object();

    const esp_app_desc_t* app = esp_app_get_description();

    resp.field("firmware", app->version);
    resp.field("running", GetRunningPartition());
    resp.field("nextSlot", GetNextPartition());
    return CommandResult::Ok;
}

CommandResult PartitionManager::Cmd_Partitions(CommandContext& ctx)
{
    static constexpr int MAX_PARTITIONS = 16;
    PartitionInfo parts[MAX_PARTITIONS];
    int count = GetPartitions(parts, MAX_PARTITIONS);

    auto root = ctx.reply.object();
    auto arr  = root.array("partitions");

    for (int i = 0; i < count; i++)
    {
        const auto& p = parts[i];
        auto o = arr.object();
        o.field("label",      p.label);
        o.field("type",       p.type);
        o.field("subtype",    p.subtype);
        o.field("offset",     p.offset);
        o.field("size",       p.size);
        o.field("running",    p.running);
        o.field("nextOta",    p.nextOta);
        o.field("uploadable", p.uploadable);
        o.field("version",    p.version);
    }
    return CommandResult::Ok;
}

// ──────────────────────────────────────────────────────────────
// Streamed upload — the body is written at `offset` (default 0). Nothing is
// erased and nothing is activated; the caller owns 'partition clear' before and
// 'partition activate' after.
// ──────────────────────────────────────────────────────────────

CommandResult PartitionManager::Cmd_WritePartition(CommandContext& ctx)
{
    // Reply is a stream of newline-free JSON messages, one per chunk: zero or more
    // progress reports {"p":<bytesWritten>} flushed as they happen, then a final
    // result. Progress is device-authoritative (bytes actually written to flash),
    // so the client's bar tracks the real write, not bytes queued into the socket.
    static constexpr size_t REPORT_EVERY = 32 * 1024;

    const char*    label  = ctx.arg(writePartitionArg);
    const uint32_t offset = ctx.arg(offsetArg);

    // `in` is positioned at the body, past the envelope the framework decoded.
    const char* err = nullptr;
    PartitionWriter w(label, offset, &err);
    if (!w.ok())
    {
        auto resp = ctx.reply.object();
        resp.field("ok", false);
        resp.field("error", err);
        return CommandResult::Ok;
    }

    // Asked before the loop, not inside it: past this point 0 means end of image,
    // and a stream that lends nothing would look exactly like an empty one — an
    // upload that "succeeded" having written nothing.
    if (!ctx.in.canLend())
    {
        auto resp = ctx.reply.object();
        resp.field("ok", false);
        resp.field("error", "transport cannot stream");
        return CommandResult::Ok;
    }

    const uint8_t* chunk = nullptr;
    size_t n;
    size_t reported = 0;
    while ((n = ctx.in.lendInput(chunk)) > 0)   // 0 == end of stream == full image
    {
        if (!w.write(chunk, n))
        {
            auto resp = ctx.reply.object();
            resp.field("ok", false);
            resp.field("error", "write failed");
            return CommandResult::Ok;
        }
        if (w.written() - reported >= REPORT_EVERY)
        {
            {
                auto progress = ctx.reply.object();
                progress.field("p", static_cast<uint32_t>(w.written()));
            }
            ctx.reply.flush();   // end this record and push it now, not at FINAL
            reported = w.written();
        }
    }

    // A stream that broke is not a complete write, even though it ended the same way
    // a complete one does — read() returns 0 for both. Returning without activating
    // leaves the boot pointer where it was, so a truncated image is inert and the
    // sender is told the real reason instead of "image validation failed" later.
    if (ctx.in.failed())
    {
        ESP_LOGE(TAG, "request stream failed after %u bytes, not activating",
                 (unsigned)w.written());
        // The byte count belongs in the message, so it is composed as a value. A field
        // value is still just a value; nothing here names the wire format.
        char why[48];
        snprintf(why, sizeof(why), "stream failed at %lu bytes", (unsigned long)w.written());

        auto resp = ctx.reply.object();
        resp.field("ok", false);
        resp.field("error", why);
        return CommandResult::Ok;
    }

    // This piece landed, and that is all this command claims. Erasing is `clear`'s
    // job and switching the boot slot is `activate`'s; a write writes.
    // The dispatcher's finish() emits this as the FINAL chunk.
    auto resp = ctx.reply.object();
    resp.field("ok", true);
    resp.field("offset", offset);
    resp.field("size", static_cast<uint32_t>(w.written()));
    return CommandResult::Ok;
}

// ──────────────────────────────────────────────────────────────
// Chunked-upload steps — the erase before and the activation after a write, which
// the caller runs itself so an upload can be many short channels instead of one long one.
// ──────────────────────────────────────────────────────────────

// These two are the first handlers written against the console request format
// rather than the JSON envelope:
//
//     clearPartition -p ota_1
//     activatePartition -p ota_1
//
// Note what is absent: no JsonReader, so no buffer holding the request. `label` is
// seventeen bytes because a partition label is seventeen bytes — the request's length
// does not enter into it. The reply stays JSON, which costs nothing because writing
// is single-pass already.
//
// Converted first because they are new and nothing in the web UI calls them yet, so
// the format can be proven on hardware without touching the frontend.

CommandResult PartitionManager::Cmd_ClearPartition(CommandContext& ctx)
{
    const char* label = ctx.arg(partitionArg);

    auto resp = ctx.reply.object();
    if (const char* err = PartitionWriter::Clear(label))
    {
        resp.field("ok", false);
        resp.field("error", err);
        return CommandResult::Ok;
    }
    resp.field("ok", true);
    return CommandResult::Ok;
}

CommandResult PartitionManager::Cmd_ActivatePartition(CommandContext& ctx)
{
    const char* label   = ctx.arg(partitionArg);
    const bool  restart = ctx.arg(restartArg);

    bool activated = false;
    {
        auto resp = ctx.reply.object();
        if (const char* err = PartitionWriter::Activate(label))
        {
            resp.field("ok", false);
            resp.field("error", err);
            return CommandResult::Ok;
        }
        resp.field("ok", true);
        resp.field("restarting", restart);
        activated = true;
    }   // close the scope BEFORE restarting so the reply is complete

    if (activated && restart)
    {
        ESP_LOGI(TAG, "rebooting into '%s'", label);
        // The same half second `system reboot` takes, and for the same reason:
        // the reply is written but the socket has not drained yet. It is the one
        // sleep a handler is allowed, because the task is about to stop existing.
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    return CommandResult::Ok;
}

// ──────────────────────────────────────────────────────────────
// Partition download — tiny JSON request in, raw bytes out
// ──────────────────────────────────────────────────────────────

CommandResult PartitionManager::Cmd_DownloadPartition(CommandContext& ctx)
{
    const char* label = ctx.arg(partitionArg);

    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!p)
    {
        auto resp = ctx.reply.object();
        resp.field("ok", false);
        resp.field("error", "unknown partition");
        return CommandResult::Ok;
    }

    ESP_LOGI(TAG, "Download partition '%s' (%lu bytes)", label, (unsigned long)p->size);

    // The header record, and the reason this command stopped answering with bare
    // bytes. Every refusal above is an ordinary record, so a reader that got only
    // bytes had to GUESS which it was holding -- the frontend did it by testing
    // whether a short reply began with {"ok":false, which a partition is entitled
    // to contain. Declaring the body removes the guess: one record, then the size
    // so a truncated read is detectable, then the bytes.
    auto head = ctx.reply.object();
    head.field("ok", true);
    head.field("size", static_cast<uint32_t>(p->size));
    Stream& body = head.body(protocol::CONTENT_TYPE_OCTETS);

    if (!body.canLend())
    {
        ESP_LOGE(TAG, "transport cannot stream a partition body");
        return CommandResult::Ok;
    }

    // Read flash straight into the reply frame the transport is about to send. The
    // run is one chunk's worth of payload, not a size of ours — which is why there
    // is no buffer here to pick a size for.
    size_t offset = 0;
    while (offset < p->size)
    {
        size_t avail = 0;
        uint8_t* dst = body.lendOutput(avail);
        if (dst == nullptr)
        {
            ESP_LOGW(TAG, "Client disconnected during download");
            return CommandResult::Ok;
        }

        size_t n = (p->size - offset < avail) ? (p->size - offset) : avail;
        if (esp_partition_read(p, offset, dst, n) != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_partition_read failed at offset %lu", (unsigned long)offset);
            return CommandResult::Ok;
        }
        body.commitOutput(n);
        offset += n;
    }
    return CommandResult::Ok;
}
