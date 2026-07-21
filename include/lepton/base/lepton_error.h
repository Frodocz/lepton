#pragma once

#include "lepton/base/attributes.h"
#include "lepton/base/logger.h"

#include <exception>
#include <string>

#if defined(LEPTON_NO_EXCEPTIONS)
    #include <cstdio>
    #include <cstdlib>

    #define LEPTON_REQUIRE(expression, error)                            \
        do                                                               \
        {                                                                \
            if (!(expression)) [[unlikely]]                              \
            {                                                            \
                std::fprintf(stderr, "Lepton fatal error: %s (%s:%d)\n", \
                    error, __FILE__, __LINE__);                          \
                std::abort();                                            \
            }                                                            \
        } while (0)

    #define LEPTON_TRY if (true)
    #define LEPTON_THROW(ex) LEPTON_REQUIRE(false, ex.what())
    #define LEPTON_CATCH(x) if (false)
    #define LEPTON_CATCH_ALL() if (false)
#else
    #define LEPTON_REQUIRE(expression, error)                            \
        do                                                               \
        {                                                                \
            if (!(expression)) [[unlikely]]                              \
            {                                                            \
                throw lepton::LeptonError(error);                       \
            }                                                            \
        } while (0)

    #define LEPTON_TRY try
    #define LEPTON_THROW(ex) throw(ex)
    #define LEPTON_CATCH(x) catch (x)
    #define LEPTON_CATCH_ALL() catch (...)
#endif

namespace lepton
{

/**
 * custom exception
 */
class LEPTON_API LeptonError : public std::exception
{
public:
  explicit LeptonError(std::string s) : _error(static_cast<std::string&&>(s)) {}
  explicit LeptonError(const char* s) : _error(s) {}

  [[nodiscard]] const char* what() const noexcept override { return _error.data(); }

private:
    std::string _error;
};

#define LEPTON_ASSERT(cond, ...)                                        \
    {                                                                   \
        if (!(cond))                                                    \
        {                                                               \
            LOG_ERROR(__VA_ARGS__);                                     \
            LEPTON_THROW(lepton::LeptonError{"Assert failed: " #cond}); \
        }                                                               \
    }

} // namespace lepton
