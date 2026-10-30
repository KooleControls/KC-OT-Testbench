#pragma once

#include "Stream.h"
#include "esp_log.h"
#include <cstring>
#include <algorithm>

class BufferStream : public Stream
{
    char* buf_;
    size_t cap_;
    size_t len_ = 0;
    bool overflowed_ = false;

public:
    BufferStream(char* buf, size_t capacity) : buf_(buf), cap_(capacity)
    {
        buf_[0] = '\0';
    }

    size_t write(const void* data, size_t size, TickType_t timeout = portMAX_DELAY) override
    {
        size_t avail = cap_ - 1 - len_;
        size_t n = std::min(size, avail);
        if (n > 0)
        {
            memcpy(buf_ + len_, data, n);
            len_ += n;
            buf_[len_] = '\0';
        }
        if (n < size && !overflowed_)
        {
            overflowed_ = true;
            ESP_LOGE("BufferStream", "Overflow! capacity=%u, tried to write %u more (already %u). "
                     "Output is truncated - increase the underlying buffer.",
                     static_cast<unsigned>(cap_), static_cast<unsigned>(size),
                     static_cast<unsigned>(len_));
        }
        return n;
    }

    bool overflowed() const { return overflowed_; }

    size_t read(void* buffer, size_t size, TickType_t timeout = portMAX_DELAY) override
    {
        return 0;
    }

    const char* data() const { return buf_; }
    size_t length() const { return len_; }

    void reset()
    {
        len_ = 0;
        buf_[0] = '\0';
        overflowed_ = false;
    }
};
