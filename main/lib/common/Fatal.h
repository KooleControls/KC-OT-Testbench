#pragma once

#include <cstdlib>
#include "esp_log.h"

// ──────────────────────────────────────────────────────────────
// FATAL — unrecoverable-situation helper.
//
// For bugs that must never limp along (dangling registry entries,
// corrupted chains, wrong-type conversions): logs the message with
// file:line context, then aborts into the ESP-IDF panic handler,
// which prints the backtrace and resets the device.
//
// Unlike assert(), this is UNCONDITIONAL — it survives NDEBUG and
// any CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL setting. Use
// assert() for sloppiness checks; use FATAL for failures that would
// otherwise corrupt a data structure and hide.
// ──────────────────────────────────────────────────────────────
#define FATAL(fmt, ...)                                                        \
    do                                                                         \
    {                                                                          \
        ESP_LOGE("FATAL", "%s:%d: " fmt, __FILE__, __LINE__, ##__VA_ARGS__);   \
        abort();                                                               \
    } while (0)
