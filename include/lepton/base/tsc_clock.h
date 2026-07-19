#pragma once

#include "lepton/base/attributes.h"
#include "third_party/tscns.h"

namespace lepton
{

class LEPTON_API TscClock
{
public:
    TscClock(const TscClock&) = delete;
    TscClock& operator=(const TscClock&) = delete;

    static int64_t rdtsc()
    {
        return TSCNS::rdtsc();
    }

    static int64_t tscns()
    {
        return instance().tsc_clock_.rdns();
    }

    static int64_t sysns()
    {
        return TSCNS::rdsysns();
    }

    static void calibrate()
    {
        instance().tsc_clock_.calibrate();
    }
private:
    TSCNS tsc_clock_;

    TscClock()
    {
        tsc_clock_.init();
    }

    static TscClock& instance()
    {
        static TscClock inst;
        return inst;
    }
};

} // namespace lepton
