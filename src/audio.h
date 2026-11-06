#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "test_session.h"

using Pcm = std::vector<std::int16_t>;
inline constexpr unsigned sampleRate = 48000;
inline constexpr unsigned channels = 2;
inline constexpr unsigned excerptSeconds = 15;
inline constexpr unsigned sourceSeconds = 90;

Pcm readReference(const std::filesystem::path& input, HANDLE cancel);
Pcm encodeExcerpt(const Pcm& reference, size_t firstFrame, size_t frameCount,
                  int bitrate, HANDLE cancel);

class Player {
public:
    ~Player();
    // Both clips must remain valid until stop() returns or playback has drained.
    bool play(const Pcm& a, const Pcm& b, bool selectB);
    void stop();

private:
    void stream(const Pcm& a, const Pcm& b, bool selectB, std::promise<bool> ready);

    HANDLE wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> selectB_{false};
};
