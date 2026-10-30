#pragma once

#include "StruxProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "Mutex.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <cstdint>
#include <cstddef>

class Stream;
class ReplyObject;

class ConsoleManager {
    static constexpr const char* TAG = "ConsoleManager";

public:
    static constexpr int32_t MAX_LINES = 200;
    static constexpr int32_t MAX_LINE_LEN = 200;

    /// Enough for a line escaped as {"log":"..."} in the worst case.
    static constexpr size_t JSON_CAP = MAX_LINE_LEN * 2 + 32;

public:
    explicit ConsoleManager(StruxProvider& strux);

    ConsoleManager(const ConsoleManager&) = delete;
    ConsoleManager& operator=(const ConsoleManager&) = delete;

    void Init();

    void WriteHistory(ReplyObject& resp) const;

    /// The sequence a consumer joining NOW should start from. A fresh connection
    /// takes this rather than the oldest line, because backfill is what `log list`
    /// is for -- a browser that just opened does not want the last twenty minutes
    /// replayed at it as live output.
    uint32_t Tip() const;

    /// The next line at or after `cursor`, already framed as the broadcast record
    /// ({"log":"..."}), with `cursor` advanced past it. Returns bytes written, or
    /// 0 when there is nothing more.
    ///
    /// This replaces the single broadcast callback the manager used to hold. A
    /// consumer is a cursor and nothing else: no queue per subscriber, no
    /// registration, and no fan-out living in whoever happened to own the
    /// callback. A cursor that falls behind the ring jumps to the oldest line
    /// still there, which is the ring doing its job rather than an error.
    size_t ReadJson(uint32_t& cursor, char* out, size_t cap);

    /// Marks the calling task as shipping log lines for as long as it exists.
    ///
    /// Sending a line can fail, and a failure logs -- from the TLS stack, from
    /// httpd, from us. That line would be shipped by the next drain, fail again
    /// and log again. Lines raised inside a scope are still stored, so `log list`
    /// and the serial console keep them; they are skipped by ReadJson, so they
    /// cannot feed the transport that produced them. This is the same defence the
    /// old broadcast task had as `broadcastTaskHandle_`, and it has to move here
    /// because the task it belonged to is gone.
    class DrainScope
    {
    public:
        explicit DrainScope(ConsoleManager& console) : console_(console) { console_.EnterDrain(); }
        ~DrainScope() { console_.ExitDrain(); }
        DrainScope(const DrainScope&) = delete;
        DrainScope& operator=(const DrainScope&) = delete;
    private:
        ConsoleManager& console_;
    };

private:
    StruxProvider& strux_;
    InitState initState_;

    // Ring buffer for log lines (allocated in PSRAM during Init)
    char (*lines_)[MAX_LINE_LEN] = nullptr;
    int32_t head_ = 0;
    int32_t count_ = 0;

    // Total lines ever stored. A cursor is one of these, which is what makes a
    // consumer stateless from the ring's point of view and what a future `since=`
    // would be expressed in.
    uint32_t seq_ = 0;

    // Per slot: was this line raised inside a DrainScope? Kept beside the ring
    // rather than in it so the line buffers stay plain text for WriteHistory.
    bool selfInflicted_[MAX_LINES] = {};

    mutable Mutex mutex_;

    // Tasks currently inside a DrainScope. Four covers every Connection this
    // device can have at once; a fifth simply is not recorded, which costs the
    // loop defence for that drain and nothing else.
    static constexpr int MAX_DRAINERS = 4;
    std::atomic<TaskHandle_t> drainers_[MAX_DRAINERS] = {};

    void EnterDrain();
    void ExitDrain();
    bool InDrain() const;


    // Line accumulator (vprintf can be called multiple times per line)
    char lineBuf_[MAX_LINE_LEN] = {};
    int32_t lineLen_ = 0;

    void FlushLine();
    void StoreLine(const char* line, int32_t len);

    static int LogOutput(const char* fmt, va_list args);
    static ConsoleManager* s_instance_;

    // ── WebSocket commands (registered with CommandManager in Init) ──
    CommandResult Cmd_GetLogs(CommandContext& ctx);

    // Defined in ConsoleManager.cpp, beside the handler.
    static CommandEntry logListCommand_;
};
