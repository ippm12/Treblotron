/**
 * scoped_timer.hpp
 *
 * Header-only RAII timing utility for per-frame profiling. Sync and
 * zero-alloc on the hot path — cheaper than the async logging module
 * for sub-millisecond scopes.
 *
 * Usage:
 *     FrameTimings m_timings;
 *     {
 *         VISION_PROFILE_SCOPE(m_timings, "warp");
 *         // ... work ...
 *     }
 *     m_timings.nextFrame();  // once per frame
 */

#ifndef SCOPED_TIMER_HPP
#define SCOPED_TIMER_HPP

#include <chrono>
#include <vector>

class FrameTimings
{
    public:
        struct Entry
        {
            const char* name;
            double      lastMs;
            double      avgMs;
        };

        // Accumulate one sample. Entries are keyed by const char* pointer
        // identity (tag literals) so there's no string comparison cost.
        void add(const char* name, double ms)
        {
            for(auto& e : m_entries)
            {
                if(e.name == name)
                {
                    e.currentMs += ms;
                    return;
                }
            }
            m_entries.push_back({name, 0.0, 0.0, ms});
        }

        // Roll the current-frame samples into the running average and reset.
        void nextFrame()
        {
            constexpr double ALPHA = 0.1;
            for(auto& e : m_entries)
            {
                e.lastMs   = e.currentMs;
                e.avgMs    = (e.avgMs == 0.0) ? e.currentMs
                                              : (e.avgMs * (1.0 - ALPHA) + e.currentMs * ALPHA);
                e.currentMs = 0.0;
            }
        }

        std::vector<Entry> snapshot() const
        {
            std::vector<Entry> out;
            out.reserve(m_entries.size());
            for(const auto& e : m_entries)
            {
                out.push_back({e.name, e.lastMs, e.avgMs});
            }
            return out;
        }

    private:
        struct InternalEntry
        {
            const char* name;
            double      lastMs;
            double      avgMs;
            double      currentMs;
        };
        std::vector<InternalEntry> m_entries;
};


class ScopedSample
{
    public:
        ScopedSample(FrameTimings& t, const char* name)
            : m_t(t)
            , m_name(name)
            , m_start(std::chrono::steady_clock::now())
        {
        }

        ~ScopedSample()
        {
            auto end = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(end - m_start).count();
            m_t.add(m_name, ms);
        }

    private:
        FrameTimings&                                 m_t;
        const char*                                   m_name;
        std::chrono::steady_clock::time_point         m_start;
};


#define VISION_PROFILE_CONCAT_INNER(a, b) a##b
#define VISION_PROFILE_CONCAT(a, b) VISION_PROFILE_CONCAT_INNER(a, b)
#define VISION_PROFILE_SCOPE(timings, name) \
    ScopedSample VISION_PROFILE_CONCAT(_scopedSample_, __LINE__)((timings), (name))

#endif // SCOPED_TIMER_HPP
