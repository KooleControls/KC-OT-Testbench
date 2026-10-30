#pragma once

#include "interfaces/SerialPort.h"
#include "freertos/FreeRTOS.h"
#include <cstddef>
#include <cstdint>

// ──────────────────────────────────────────────────────────────
// The NXP ISP protocol: the LPC11U6x boot ROM's serial bootloader, spoken over a
// serial line. Getting the chip INTO the bootloader is the caller's (it is a reset
// with the ISP pin held, which is board wiring); this is everything after that.
//
// The command set and its shape are the ones the KC1245 gateway used to flash the
// PCB1246 (nxp_isp/ISPConnection.cpp there): '?' and "Synchronized" to lock the
// boot ROM's autobaud, the IRC frequency in kHz, echo off, unlock, then sector-wise
// prepare/erase and 512-byte UU-encoded writes to RAM copied to flash 4 KB at a time.
// Two things are added: a compare (M) to check flash without reading it back, and
// a RESEND retry on a write block.
//
// Blocking, on the caller's task. Every reply has a timeout; nothing here retries
// beyond the RESEND the ROM asks for.
// ──────────────────────────────────────────────────────────────
class LpcIsp
{
public:
    /// The chip's flash in sectors: 24 x 4 KB, then 5 x 32 KB (LPC11U68, 256 KB).
    static constexpr uint32_t PAGE = 4096;          // what one copy (C) writes
    static constexpr uint32_t FLASH_SIZE = 256 * 1024;

    /// Where pages are staged in the chip's RAM before a copy. Clear of the boot ROM's
    /// own RAM, which sits below 0x1000025C and at the top of SRAM0.
    static constexpr uint32_t RAM_BUFFER = 0x10000300;

    /// The ROM maps itself over the first 512 bytes of flash while in ISP, so a compare
    /// there sees the ROM. Nothing below this address can be checked.
    static constexpr uint32_t REMAPPED = 0x200;

    enum class Compare : uint8_t { Same, Different, Error };

    explicit LpcIsp(SerialPort& port) : port_(port) {}

    /// Lock the boot ROM's autobaud and switch its echo off. Call with the chip just
    /// reset into ISP. False when it never answers: no module, or not in ISP.
    bool Synchronize();

    /// Allow erase and write. Every flash command after this needs it.
    bool Unlock();

    /// Compare `len` bytes of flash at `flashAddr` with `data`, which is staged in the
    /// chip's RAM first. `len` a multiple of 4 and at most PAGE; `flashAddr` at or past
    /// REMAPPED.
    Compare CompareFlash(uint32_t flashAddr, const uint8_t* data, size_t len);

    /// Erase the sectors from the one holding `fromAddr` to the one holding `toAddr`.
    bool Erase(uint32_t fromAddr, uint32_t toAddr);

    /// Write one PAGE-aligned page into erased flash and check it landed. `len` is how
    /// much of `data` is image; the rest of the page is written as 0xFF.
    bool WritePage(uint32_t flashAddr, const uint8_t* data, size_t len);

    /// The last ISP return code (0 = success), for a status to show. -1 is a timeout
    /// or a reply that was not a number.
    int LastCode() const { return lastCode_; }
    const char* LastStep() const { return lastStep_; }

private:
    static constexpr size_t WRITE_BLOCK = 512;      // per W command: 12 UU lines

    SerialPort& port_;
    int lastCode_ = 0;
    const char* lastStep_ = "";

    static uint32_t SectorOf(uint32_t addr);

    void Drain();
    bool Send(const char* text);
    bool ReadLine(char* line, size_t cap, uint32_t timeoutMs);

    /// Send a command line and return its numeric reply, skipping its echo.
    int Command(const char* step, const char* line, uint32_t timeoutMs = 1000);

    /// Send `line` and wait for "OK", skipping its echo.
    bool ExpectOk(const char* step, const char* line);

    bool Prepare(uint32_t fromSector, uint32_t toSector);
    bool WriteRam(uint32_t ramAddr, const uint8_t* data, size_t len);
};
