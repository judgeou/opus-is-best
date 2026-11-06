#pragma once

#include <array>
#include <random>

inline constexpr std::array<int, 6> bitrates{20, 32, 64, 96, 128, 192};
inline constexpr size_t initialStage = 1;
inline constexpr int referenceBitrate = 256;
inline constexpr int advanceStreak = 3;
// Twelve reference choices reach the previous top-stage likelihood boundary.
inline constexpr int finalStreak = 12;

struct BlindTrial {
    bool aIsComparison = false;
    unsigned listened = 0;

    void randomize(std::mt19937& random) {
        std::bernoulli_distribution coin(0.5);
        aIsComparison = coin(random);
        listened = 0;
    }

    bool usesComparison(int sample) const {
        return aIsComparison != (sample == 1);
    }
};
