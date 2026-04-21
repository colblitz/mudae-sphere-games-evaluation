/**
 * progress.h — Progress reporting helpers for harness evaluators.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>

namespace sphere {

// Print a timestamp prefix [HH:MM:SS]
static inline void print_ts() {
    time_t t = time(nullptr);
    struct tm tm_s;
    localtime_r(&t, &tm_s);
    printf("[%02d:%02d:%02d] ", tm_s.tm_hour, tm_s.tm_min, tm_s.tm_sec);
}

struct ProgressReporter {
    uint64_t total;
    uint64_t interval;  // print every N items
    std::chrono::steady_clock::time_point start;

    explicit ProgressReporter(uint64_t total_, uint64_t interval_ = 500000)
        : total(total_), interval(interval_),
          start(std::chrono::steady_clock::now()) {}

    void report(uint64_t done, double running_ev = -1.0) const {
        using namespace std::chrono;
        auto now     = steady_clock::now();
        double secs  = duration_cast<milliseconds>(now - start).count() / 1000.0;
        double rate  = done > 0 ? static_cast<double>(done) / secs : 0.0;
        double eta   = (rate > 0 && total > done)
                     ? static_cast<double>(total - done) / rate : 0.0;
        print_ts();
        if (running_ev >= 0.0) {
            printf("  %llu / %llu (%.1f%%)  elapsed=%.1fs  rate=%.0f/s  eta=%.0fs"
                   "  ev=%.3f\n",
                   (unsigned long long)done, (unsigned long long)total,
                   100.0 * static_cast<double>(done) / static_cast<double>(total),
                   secs, rate, eta, running_ev);
        } else {
            printf("  %llu / %llu (%.1f%%)  elapsed=%.1fs  rate=%.0f/s  eta=%.0fs\n",
                   (unsigned long long)done, (unsigned long long)total,
                   100.0 * static_cast<double>(done) / static_cast<double>(total),
                   secs, rate, eta);
        }
        fflush(stdout);
    }

    void done(double final_ev = -1.0) const {
        report(total, final_ev);
    }
};

}  // namespace sphere
