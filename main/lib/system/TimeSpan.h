#pragma once

class TimeSpan {
    constexpr static const char* TAG = "TimeSpan";

public:
    TimeSpan();
    explicit TimeSpan(long seconds);

    // Arithmetic operators
    TimeSpan operator+(const TimeSpan& other) const;
    TimeSpan operator-(const TimeSpan& other) const;
    TimeSpan& operator+=(const TimeSpan& other);
    TimeSpan& operator-=(const TimeSpan& other);

    // Comparison operators
    bool operator==(const TimeSpan& other) const;
    bool operator!=(const TimeSpan& other) const;
    bool operator<(const TimeSpan& other) const;
    bool operator<=(const TimeSpan& other) const;
    bool operator>(const TimeSpan& other) const;
    bool operator>=(const TimeSpan& other) const;

    /// @brief Seconds component modulo 60 (0-59)
    int Seconds() const;
    long TotalSeconds() const;

    /// @brief Minutes component modulo 60 (0-59)
    int Minutes() const;
    long TotalMinutes() const;

    /// @brief Hours component modulo 24 (0-23)
    int Hours() const;
    long TotalHours() const;

    // Factory methods
    static TimeSpan FromHours(long hours);
    static TimeSpan FromMinutes(long minutes);
    static TimeSpan FromSeconds(long secs);
    static bool FromStringLocal(TimeSpan& result, const char* str, const char* format);
    static TimeSpan Zero();

private:
    long seconds;
};
