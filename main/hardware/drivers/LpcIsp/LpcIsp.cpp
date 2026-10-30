#include "LpcIsp.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// ISP return codes this file tells apart (UM10732, ISP status codes).
constexpr int CMD_SUCCESS   = 0;
constexpr int COMPARE_ERROR = 10;

// The boot ROM runs from its internal RC oscillator while in ISP, so this is the
// IRC's frequency in kHz rather than the board's crystal.
constexpr const char* IRC_KHZ = "12000";

constexpr const char* UNLOCK_CODE = "23130";

// Bytes per UU-encoded line, the protocol's maximum.
constexpr size_t UU_LINE = 45;

char UuChar(uint8_t v) { return v == 0 ? '`' : static_cast<char>(v + 0x20); }

/// One UU-encoded line of `n` bytes (n <= UU_LINE), CR LF included. Returns its length.
size_t UuEncodeLine(const uint8_t* p, size_t n, char* out)
{
    size_t w = 0;
    out[w++] = static_cast<char>(n + 0x20);
    for (size_t i = 0; i < n; i += 3)
    {
        const uint8_t a = p[i];
        const uint8_t b = i + 1 < n ? p[i + 1] : 0;
        const uint8_t c = i + 2 < n ? p[i + 2] : 0;
        out[w++] = UuChar(a >> 2);
        out[w++] = UuChar(static_cast<uint8_t>(((a & 0x03) << 4) | (b >> 4)));
        out[w++] = UuChar(static_cast<uint8_t>(((b & 0x0F) << 2) | (c >> 6)));
        out[w++] = UuChar(c & 0x3F);
    }
    out[w++] = '\r';
    out[w++] = '\n';
    return w;
}

} // namespace

// ── Line I/O ─────────────────────────────────────────────────

void LpcIsp::Drain()
{
    uint8_t junk[32];
    while (port_.read(junk, sizeof(junk), 0) > 0) {}
}

bool LpcIsp::Send(const char* text)
{
    const size_t len = strlen(text);
    return port_.write(text, len, pdMS_TO_TICKS(1000)) == len;
}

bool LpcIsp::ReadLine(char* line, size_t cap, uint32_t timeoutMs)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeoutMs);
    size_t n = 0;
    for (;;)
    {
        const TickType_t now = xTaskGetTickCount();
        if (static_cast<int32_t>(deadline - now) <= 0)
            return false;

        char c;
        if (port_.read(&c, 1, deadline - now) != 1)
            continue;
        // CR ends a line too: the ROM's echo ends in a bare CR, so with echo on
        // "Synchronized\r\n" comes back as "Synchronized\rOK\r\n".
        if (c == '\r' || c == '\n')
        {
            if (n == 0)
                continue;          // the LF of a CR LF, or an empty line
            line[n] = '\0';
            return true;
        }
        if (n + 1 < cap)
            line[n++] = c;
    }
}

int LpcIsp::Command(const char* step, const char* line, uint32_t timeoutMs)
{
    char out[48];
    snprintf(out, sizeof(out), "%s\r\n", line);
    lastStep_ = step;
    lastCode_ = -1;
    if (!Send(out))
        return -1;

    // At most the echo and then the code. The echo only exists until `A 0` has run,
    // and it is the command exactly as sent.
    char reply[48];
    for (int i = 0; i < 2; ++i)
    {
        if (!ReadLine(reply, sizeof(reply), timeoutMs))
            return -1;
        if (strcmp(reply, line) == 0)
            continue;
        char* end = nullptr;
        const long code = strtol(reply, &end, 10);
        if (end == reply || *end != '\0')
            return -1;
        lastCode_ = static_cast<int>(code);
        return lastCode_;
    }
    return -1;
}

bool LpcIsp::ExpectOk(const char* step, const char* line)
{
    char out[32];
    snprintf(out, sizeof(out), "%s\r\n", line);
    lastStep_ = step;
    lastCode_ = -1;
    if (!Send(out))
        return false;

    char reply[32];
    for (int i = 0; i < 2; ++i)
    {
        if (!ReadLine(reply, sizeof(reply), 1000))
            return false;
        if (strcmp(reply, line) == 0)
            continue;              // the echo
        if (strcmp(reply, "OK") != 0)
            return false;
        lastCode_ = 0;
        return true;
    }
    return false;
}

// ── Session ──────────────────────────────────────────────────

bool LpcIsp::Synchronize()
{
    Drain();

    // The ROM measures the baud rate on '?' and answers once it has it. A few tries,
    // because the first byte after a reset can land before the ROM is listening.
    lastStep_ = "synchronize";
    lastCode_ = -1;
    bool synced = false;
    for (int i = 0; i < 10 && !synced; ++i)
    {
        if (!Send("?"))
            return false;
        char reply[32];
        synced = ReadLine(reply, sizeof(reply), 200) && strcmp(reply, "Synchronized") == 0;
    }
    if (!synced)
        return false;

    return ExpectOk("synchronize", "Synchronized")
        && ExpectOk("frequency", IRC_KHZ)
        && Command("echo off", "A 0") == CMD_SUCCESS;
}

bool LpcIsp::Unlock()
{
    char line[16];
    snprintf(line, sizeof(line), "U %s", UNLOCK_CODE);
    return Command("unlock", line) == CMD_SUCCESS;
}

// ── Flash ────────────────────────────────────────────────────

uint32_t LpcIsp::SectorOf(uint32_t addr)
{
    constexpr uint32_t SMALL = 4096, SMALL_COUNT = 24, LARGE = 32768;
    if (addr < SMALL * SMALL_COUNT)
        return addr / SMALL;
    return SMALL_COUNT + (addr - SMALL * SMALL_COUNT) / LARGE;
}

bool LpcIsp::Prepare(uint32_t fromSector, uint32_t toSector)
{
    char line[24];
    snprintf(line, sizeof(line), "P %lu %lu",
             static_cast<unsigned long>(fromSector), static_cast<unsigned long>(toSector));
    return Command("prepare", line) == CMD_SUCCESS;
}

bool LpcIsp::Erase(uint32_t fromAddr, uint32_t toAddr)
{
    const uint32_t from = SectorOf(fromAddr), to = SectorOf(toAddr);
    if (!Prepare(from, to))
        return false;

    char line[24];
    snprintf(line, sizeof(line), "E %lu %lu",
             static_cast<unsigned long>(from), static_cast<unsigned long>(to));
    // Each sector is ~100 ms; the large ones and a long range take longer.
    return Command("erase", line, 5000) == CMD_SUCCESS;
}

bool LpcIsp::WriteRam(uint32_t ramAddr, const uint8_t* data, size_t len)
{
    char line[32];
    snprintf(line, sizeof(line), "W %lu %u",
             static_cast<unsigned long>(ramAddr), static_cast<unsigned>(len));

    // The ROM sums what it decoded and asks for the block again when the sum it was
    // sent disagrees: a corrupted line is a RESEND, not a bad flash.
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        if (Command("write", line) != CMD_SUCCESS)
            return false;

        uint32_t sum = 0;
        char uu[2 + UU_LINE / 3 * 4 + 2];
        for (size_t off = 0; off < len; off += UU_LINE)
        {
            const size_t n = std::min(UU_LINE, len - off);
            for (size_t i = 0; i < n; ++i)
                sum += data[off + i];
            const size_t w = UuEncodeLine(data + off, n, uu);
            if (port_.write(uu, w, pdMS_TO_TICKS(1000)) != w)
                return false;
        }

        char check[16];
        snprintf(check, sizeof(check), "%lu\r\n", static_cast<unsigned long>(sum));
        if (!Send(check))
            return false;

        char reply[16];
        lastStep_ = "write";
        if (!ReadLine(reply, sizeof(reply), 1000))
            return false;
        if (strcmp(reply, "OK") == 0)
            return true;
        if (strcmp(reply, "RESEND") != 0)
            return false;
    }
    return false;
}

LpcIsp::Compare LpcIsp::CompareFlash(uint32_t flashAddr, const uint8_t* data, size_t len)
{
    if (flashAddr < REMAPPED || len == 0 || len > PAGE || (len % 4) != 0)
        return Compare::Error;

    for (size_t off = 0; off < len; off += WRITE_BLOCK)
        if (!WriteRam(RAM_BUFFER + off, data + off, std::min(WRITE_BLOCK, len - off)))
            return Compare::Error;

    char line[40];
    snprintf(line, sizeof(line), "M %lu %lu %u",
             static_cast<unsigned long>(flashAddr), static_cast<unsigned long>(RAM_BUFFER),
             static_cast<unsigned>(len));
    const int code = Command("compare", line);
    if (code == CMD_SUCCESS)
        return Compare::Same;
    if (code == COMPARE_ERROR)
    {
        // Followed by the offset of the first difference, which nothing here needs.
        char offset[16];
        ReadLine(offset, sizeof(offset), 200);
        return Compare::Different;
    }
    return Compare::Error;
}

bool LpcIsp::WritePage(uint32_t flashAddr, const uint8_t* data, size_t len)
{
    if ((flashAddr % PAGE) != 0)
        return false;

    // Stage the page in RAM. The image's last page is short: the rest goes out as
    // 0xFF, which is what erased flash reads as anyway.
    uint8_t block[WRITE_BLOCK];
    for (size_t off = 0; off < PAGE; off += WRITE_BLOCK)
    {
        const uint8_t* src = data + off;
        if (off + WRITE_BLOCK > len)
        {
            memset(block, 0xFF, sizeof(block));
            if (off < len)
                memcpy(block, data + off, len - off);
            src = block;
        }
        if (!WriteRam(RAM_BUFFER + off, src, WRITE_BLOCK))
            return false;
    }

    const uint32_t sector = SectorOf(flashAddr);
    if (!Prepare(sector, sector))
        return false;

    char line[40];
    snprintf(line, sizeof(line), "C %lu %lu %lu",
             static_cast<unsigned long>(flashAddr), static_cast<unsigned long>(RAM_BUFFER),
             static_cast<unsigned long>(PAGE));
    if (Command("copy", line, 2000) != CMD_SUCCESS)
        return false;

    // The page is still in RAM, so checking it costs a command rather than a read-back.
    // Except under the ROM's own mapping, which a compare cannot see past.
    const uint32_t skip = flashAddr < REMAPPED ? REMAPPED - flashAddr : 0;
    snprintf(line, sizeof(line), "M %lu %lu %lu",
             static_cast<unsigned long>(flashAddr + skip),
             static_cast<unsigned long>(RAM_BUFFER + skip),
             static_cast<unsigned long>(PAGE - skip));
    return Command("verify", line) == CMD_SUCCESS;
}
