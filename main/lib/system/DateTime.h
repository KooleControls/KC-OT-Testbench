#pragma once
#include <ctime>
#include "TimeSpan.h"

class DateTime {
    constexpr const static char* TAG = "DateTime";
    std::time_t utc;  // Seconds since epoch (UTC)

    explicit DateTime(std::time_t utc);

public:
    DateTime();

    // Factory methods
    static DateTime Now();
    static DateTime FromUtc(std::time_t secondsSinceEpoch);
    static DateTime FromLocal(const std::tm& localTm);
    static bool FromStringLocal(DateTime& result, const char* str, const char* format);

    static DateTime MinValue();
    static DateTime MaxValue();

    // String conversion
    size_t ToStringUtc(char* buffer, size_t size, const char* format) const;
    size_t ToStringLocal(char* buffer, size_t size, const char* format) const;

    // Time components
    TimeSpan GetTimeOfDayUtc() const;
    TimeSpan GetTimeOfDayLocal() const;

    // Local calendar components
    int DayLocal() const;
    int MonthLocal() const;
    int YearLocal() const;
    int HourLocal() const;
    int MinuteLocal() const;
    int SecondLocal() const;

    // Raw access
    std::time_t UtcSeconds() const;

    // Conversion to tm
    std::tm ToUtcTm() const;
    std::tm ToLocalTm() const;

    // Operators
    DateTime operator+(const TimeSpan& ts) const;
    DateTime operator-(const TimeSpan& ts) const;
    DateTime& operator+=(const TimeSpan& ts);
    DateTime& operator-=(const TimeSpan& ts);
    TimeSpan operator-(const DateTime& other) const;

    bool operator==(const DateTime& other) const;
    bool operator!=(const DateTime& other) const;
    bool operator<(const DateTime& other) const;
    bool operator<=(const DateTime& other) const;
    bool operator>(const DateTime& other) const;
    bool operator>=(const DateTime& other) const;

    static constexpr const char* FormatIso8601 = "%FT%TZ";
    static constexpr const char* FormatDateOnly = "%F";
    static constexpr const char* FormatTimeOnly = "%T";
};
