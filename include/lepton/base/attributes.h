#pragma once

namespace lepton {

    
#define LEPTON_API __attribute__((visibility("default")))

constexpr inline int LEPTON_VERSION_MAJOR{0};
constexpr inline int LEPTON_VERSION_MINOR{1};
constexpr inline int LEPTON_VERSION_PATCH{0};

/**
 * __has_attribute
 */
#ifndef LEPTON_HAS_ATTRIBUTE
    #ifdef __has_attribute
        #define LEPTON_HAS_ATTRIBUTE(x) __has_attribute(x)
    #else
        #define LEPTON_HAS_ATTRIBUTE(x) 0
    #endif
#endif

/**
 * Prevents the compiler from inlining a function
 */
#ifndef LEPTON_NOINLINE
    #if LEPTON_HAS_ATTRIBUTE(noinline)
        #define LEPTON_NOINLINE __attribute__((noinline))
    #else
        #define LEPTON_NOINLINE
    #endif
#endif

/**
 * Always inline a function
 */
#ifndef LEPTON_ALWAYS_INLINE
    #if LEPTON_HAS_ATTRIBUTE(always_inline)
        #define LEPTON_ALWAYS_INLINE inline __attribute__((always_inline))
    #else
        #define LEPTON_ALWAYS_INLINE inline
    #endif
#endif

/**
 * Gcc hot/cold attributes
 * Tells GCC that a function is hot or cold. GCC can use this information to
 * improve static analysis, i.e. a conditional branch to a cold function
 * is likely to be not-taken.
 */
#ifndef LEPTON_ATTRIBUTE_HOT
    #if LEPTON_HAS_ATTRIBUTE(hot)
        #define LEPTON_ATTRIBUTE_HOT __attribute__((hot))
    #else
        #define LEPTON_ATTRIBUTE_HOT
    #endif
#endif

#ifndef LEPTON_ATTRIBUTE_COLD
    #if LEPTON_HAS_ATTRIBUTE(cold)
        #define LEPTON_ATTRIBUTE_COLD __attribute__((cold))
    #else
        #define LEPTON_ATTRIBUTE_COLD
    #endif
#endif

#ifndef LEPTON_ATTRIBUTE_PACKED
    #if LEPTON_HAS_ATTRIBUTE(packed)
        #define LEPTON_ATTRIBUTE_PACKED __attribute__((packed))
    #else
        #define LEPTON_ATTRIBUTE_PACKED
    #endif
#endif

} // namespace lepton
