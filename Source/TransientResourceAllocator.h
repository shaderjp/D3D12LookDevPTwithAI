#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace rb
{
    struct TransientResourceLifetime
    {
        std::string name;
        std::uint64_t size = 0;
        std::uint64_t alignment = 1;
        std::uint32_t firstPass = 0;
        std::uint32_t lastPass = 0;
    };

    struct TransientResourcePlacement
    {
        std::string name;
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
        std::uint32_t firstPass = 0;
        std::uint32_t lastPass = 0;
    };

    struct TransientResourcePlan
    {
        std::vector<TransientResourcePlacement> placements;
        std::uint64_t heapSize = 0;

        const TransientResourcePlacement* Find(const std::string& name) const noexcept
        {
            const auto found = std::find_if(
                placements.begin(),
                placements.end(),
                [&](const TransientResourcePlacement& placement)
                {
                    return placement.name == name;
                });
            return found == placements.end() ? nullptr : &*found;
        }
    };

    inline bool LifetimesOverlap(
        const TransientResourceLifetime& left,
        const TransientResourceLifetime& right) noexcept
    {
        return left.firstPass <= right.lastPass &&
            right.firstPass <= left.lastPass;
    }

    inline std::uint64_t AlignTransientOffset(
        std::uint64_t value,
        std::uint64_t alignment)
    {
        if (alignment == 0 || (alignment & (alignment - 1)) != 0)
        {
            throw std::invalid_argument(
                "Transient resource alignment must be a non-zero power of two.");
        }
        return (value + alignment - 1) & ~(alignment - 1);
    }

    // Greedy interval-aware placed-resource allocator. Resources may reuse an
    // existing byte range only when their declared pass intervals do not
    // overlap. The returned plan is deterministic for a stable input order.
    inline TransientResourcePlan BuildTransientResourcePlan(
        const std::vector<TransientResourceLifetime>& lifetimes)
    {
        TransientResourcePlan plan;
        plan.placements.reserve(lifetimes.size());

        for (const TransientResourceLifetime& lifetime : lifetimes)
        {
            if (lifetime.name.empty() || lifetime.size == 0 ||
                lifetime.firstPass > lifetime.lastPass)
            {
                throw std::invalid_argument(
                    "Transient resource lifetime is incomplete or inverted.");
            }

            std::uint64_t candidate = 0;
            for (;;)
            {
                candidate = AlignTransientOffset(candidate, lifetime.alignment);
                std::uint64_t nextCandidate = candidate;
                bool conflict = false;
                for (std::size_t index = 0; index < plan.placements.size(); ++index)
                {
                    const TransientResourcePlacement& placed =
                        plan.placements[index];
                    const TransientResourceLifetime& placedLifetime =
                        lifetimes[index];
                    if (!LifetimesOverlap(lifetime, placedLifetime))
                    {
                        continue;
                    }
                    const bool byteRangesOverlap =
                        candidate < placed.offset + placed.size &&
                        placed.offset < candidate + lifetime.size;
                    if (byteRangesOverlap)
                    {
                        conflict = true;
                        nextCandidate = (std::max)(
                            nextCandidate,
                            placed.offset + placed.size);
                    }
                }
                if (!conflict)
                {
                    break;
                }
                candidate = nextCandidate;
            }

            plan.placements.push_back({
                lifetime.name,
                candidate,
                lifetime.size,
                lifetime.firstPass,
                lifetime.lastPass });
            plan.heapSize = (std::max)(
                plan.heapSize,
                candidate + lifetime.size);
        }
        return plan;
    }
}
