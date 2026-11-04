// Host-side logic test for PressureFillProcess (no ESP32 needed).
//
//   g++ -std=c++17 -I. -I../../src pressure_fill_host_test.cpp -o t && ./t
//
// Expected: full ~2.4 s, empty ~7 s, dry-run timeout 20 s, short spike ignored,
// hold=0 finishes on the first above-threshold sample.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <functional>

namespace {
unsigned long g_millis = 0;
float g_pressure = 0.f;
} // namespace

unsigned long millis() { return g_millis; }

#include "display/core/process/PressureFillProcess.h"

namespace {

constexpr float kThreshold = 8.0f;
constexpr unsigned long kHoldMs = 1500;
constexpr unsigned long kMaxMs = 20000;
constexpr unsigned long kStepMs = 100;

unsigned long runUntilDone(const std::function<float(unsigned long)> &curve, unsigned long holdMs = kHoldMs) {
    g_millis = 0;
    g_pressure = curve(0);
    PressureFillProcess proc([]() { return g_pressure; }, kThreshold, holdMs, kMaxMs);

    while (proc.isActive()) {
        g_millis += kStepMs;
        g_pressure = curve(g_millis);
        proc.progress();
        if (g_millis > kMaxMs + 1000)
            break;
    }
    return g_millis;
}

float fullBoiler(unsigned long t) {
    // Already near full: crosses 8 bar after ~0.9 s
    if (t < 900)
        return 3.7f + (8.0f - 3.7f) * (static_cast<float>(t) / 900.f);
    return 9.0f;
}

float emptyBoiler(unsigned long t) {
    // After steaming: crosses 8 bar after ~5.5 s
    if (t < 5500)
        return 2.5f + (8.0f - 2.5f) * (static_cast<float>(t) / 5500.f);
    return 9.0f;
}

float dryRun(unsigned long) { return 0.5f; }

float shortSpike(unsigned long t) {
    if (t >= 500 && t < 1000)
        return 9.0f; // 0.5 s spike, below hold time
    return 2.0f;
}

bool near(unsigned long value, unsigned long target, unsigned long tol) {
    return value + tol >= target && value <= target + tol;
}

} // namespace

int main() {
    const unsigned long fullMs = runUntilDone(fullBoiler);
    std::printf("full boiler: %lu ms\n", fullMs);
    assert(near(fullMs, 2400, 200));

    const unsigned long emptyMs = runUntilDone(emptyBoiler);
    std::printf("empty boiler: %lu ms\n", emptyMs);
    assert(near(emptyMs, 7000, 200));

    const unsigned long dryMs = runUntilDone(dryRun);
    std::printf("dry run: %lu ms\n", dryMs);
    assert(near(dryMs, 20000, 100));

    const unsigned long spikeMs = runUntilDone(shortSpike);
    std::printf("short spike: %lu ms (should time out)\n", spikeMs);
    assert(near(spikeMs, 20000, 100));

    // Hold 0: finishes on the first sample at/above threshold (900 ms), no extra cycle.
    const unsigned long zeroHoldMs = runUntilDone(fullBoiler, 0);
    std::printf("hold=0: %lu ms\n", zeroHoldMs);
    assert(near(zeroHoldMs, 900, 100));

    std::printf("all pressure-fill host tests passed\n");
    return 0;
}
