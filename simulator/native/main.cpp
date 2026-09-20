#include <chrono>
#include <cstdio>
#include <thread>

extern "C" {
#include "../../firmware/main/countdown.h"
}

int main() {
    countdown_reset();
    countdown_input_t seed{
        .distance_meters = 250,
        .timestamp_ms = 0,
        .speed_kmh = 54.0f,
        .speed_valid = true,
        .maneuver_sequence = 1,
    };
    countdown_accept(&seed);

    std::puts("OpenApex native terminal simulator");
    for (uint32_t elapsed = 0; elapsed <= 12000; elapsed += 1000) {
        const countdown_output_t output = countdown_estimate(elapsed);
        std::printf("t=%5ums distance=%3um stale=%s\n", elapsed, output.distance_meters,
                    output.stale ? "yes" : "no");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return 0;
}
