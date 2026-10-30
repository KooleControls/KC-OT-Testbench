#pragma once
#include <type_traits>

// --------------------------------------------------
// Generic flag operator enabler
// --------------------------------------------------
template<typename Enum>
struct EnableBitMaskOperators {
    static constexpr bool enable = false;
};

// --------------------------------------------------
// Bitwise operator overloads (enabled via specialization)
// --------------------------------------------------
template<typename Enum>
constexpr typename std::enable_if<EnableBitMaskOperators<Enum>::enable, Enum>::type
operator|(Enum lhs, Enum rhs) {
    using U = typename std::underlying_type<Enum>::type;
    return static_cast<Enum>(static_cast<U>(lhs) | static_cast<U>(rhs));
}

template<typename Enum>
constexpr typename std::enable_if<EnableBitMaskOperators<Enum>::enable, Enum>::type
operator&(Enum lhs, Enum rhs) {
    using U = typename std::underlying_type<Enum>::type;
    return static_cast<Enum>(static_cast<U>(lhs) & static_cast<U>(rhs));
}

template<typename Enum>
constexpr typename std::enable_if<EnableBitMaskOperators<Enum>::enable, Enum>::type
operator^(Enum lhs, Enum rhs) {
    using U = typename std::underlying_type<Enum>::type;
    return static_cast<Enum>(static_cast<U>(lhs) ^ static_cast<U>(rhs));
}

template<typename Enum>
constexpr typename std::enable_if<EnableBitMaskOperators<Enum>::enable, Enum>::type
operator~(Enum lhs) {
    using U = typename std::underlying_type<Enum>::type;
    return static_cast<Enum>(~static_cast<U>(lhs));
}

template<typename Enum>
constexpr typename std::enable_if<EnableBitMaskOperators<Enum>::enable, Enum&>::type
operator|=(Enum& lhs, Enum rhs) {
    lhs = lhs | rhs;
    return lhs;
}

template<typename Enum>
constexpr typename std::enable_if<EnableBitMaskOperators<Enum>::enable, Enum&>::type
operator&=(Enum& lhs, Enum rhs) {
    lhs = lhs & rhs;
    return lhs;
}

// --------------------------------------------------
// Logical operator overloads (for readability)
// --------------------------------------------------

// true if both sides share any flag bits
template<typename Enum>
constexpr typename std::enable_if<EnableBitMaskOperators<Enum>::enable, bool>::type
operator&&(Enum lhs, Enum rhs) {
    using U = typename std::underlying_type<Enum>::type;
    return (static_cast<U>(lhs) & static_cast<U>(rhs)) != 0;
}

// true if either side has any flag bit set
template<typename Enum>
constexpr typename std::enable_if<EnableBitMaskOperators<Enum>::enable, bool>::type
operator||(Enum lhs, Enum rhs) {
    using U = typename std::underlying_type<Enum>::type;
    return (static_cast<U>(lhs) | static_cast<U>(rhs)) != 0;
}

// --------------------------------------------------
// Helper: HasFlag(enumValue, flag)
// --------------------------------------------------
template<typename Enum>
constexpr bool HasFlag(Enum value, Enum flag) {
    using U = typename std::underlying_type<Enum>::type;
    return (static_cast<U>(value) & static_cast<U>(flag)) == static_cast<U>(flag);
}

// --------------------------------------------------
// Convenience macro to enable bitmask operators
// --------------------------------------------------
#define ENABLE_BITMASK_OPERATORS(EnumType) \
    template<> struct EnableBitMaskOperators<EnumType> { \
        static constexpr bool enable = true; \
    }
