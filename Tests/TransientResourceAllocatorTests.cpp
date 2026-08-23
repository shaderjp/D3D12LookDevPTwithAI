#include "../Source/TransientResourceAllocator.h"

#include <iostream>
#include <stdexcept>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }
}

int main()
{
    try
    {
        const rb::TransientResourcePlan aliasPlan =
            rb::BuildTransientResourcePlan({
                { "diScratch", 96, 64, 0, 1 },
                { "giCurrent", 128, 64, 2, 3 },
                { "immutableHistory", 80, 64, 0, 3 },
            });
        Require(aliasPlan.Find("diScratch")->offset == 0, "DI offset changed.");
        Require(aliasPlan.Find("giCurrent")->offset == 0, "Disjoint GI did not alias DI.");
        Require(aliasPlan.Find("immutableHistory")->offset == 128, "Overlapping history was aliased.");
        Require(aliasPlan.heapSize == 208, "Unexpected transient heap size.");

        const rb::TransientResourceLifetime a{ "a", 64, 64, 1, 2 };
        const rb::TransientResourceLifetime b{ "b", 64, 64, 2, 3 };
        const rb::TransientResourceLifetime c{ "c", 64, 64, 3, 4 };
        Require(rb::LifetimesOverlap(a, b), "Inclusive pass intervals must overlap.");
        Require(!rb::LifetimesOverlap(a, c), "Disjoint pass intervals overlap.");

        bool invalidRejected = false;
        try
        {
            (void)rb::BuildTransientResourcePlan({
                { "bad", 64, 48, 0, 1 },
            });
        }
        catch (const std::invalid_argument&)
        {
            invalidRejected = true;
        }
        Require(invalidRejected, "Invalid alignment was accepted.");
        std::cout << "Transient resource allocator tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
