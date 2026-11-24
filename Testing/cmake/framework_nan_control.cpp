// Checks that the framework's SNR assertion rejects NaN outputs and references in this build.
// FrameworkSource/Error.cpp guards each value with isnan/isinf; built with -ffast-math, GCC may drop those
// guards, so a NaN output gives a NaN SNR and `snr < threshold` is false: the case would pass.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Error.h"

// As in testmain.cpp: the Corstone-300 glue overrides this to start the UART console.
extern "C" void testmain_hook(void) __attribute__((weak));
extern "C" void testmain_hook(void)
{
}

static volatile uint32_t quiet_nan_bits = 0x7fc00000u;

static float runtime_nan(void)
{
    uint32_t bits = quiet_nan_bits;
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

static bool snr_rejects(float reference, float output)
{
    try
    {
        Client::assert_snr_error(__LINE__, reference, output, 120.0f);
    }
    catch (Client::Error &)
    {
        return true;
    }
    return false;
}

int main(void)
{
    testmain_hook();

    const float nan = runtime_nan();
    struct
    {
        const char *name;
        float reference;
        float output;
        bool rejected;
    } const cases[] = {
        {"equal output", 1.0f, 1.0f, false},
        {"wrong output", 1.0f, 2.0f, true},
        {"NaN output", 1.0f, nan, true},
        {"NaN reference", nan, 1.0f, true},
    };

    int failures = 0;
    for (const auto &c : cases)
    {
        if (snr_rejects(c.reference, c.output) != c.rejected)
        {
            std::printf("NAN-CONTROL FAIL: %s was %s\n", c.name, c.rejected ? "accepted" : "rejected");
            failures++;
        }
    }
    if (failures == 0)
    {
        std::printf("NAN-CONTROL PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
