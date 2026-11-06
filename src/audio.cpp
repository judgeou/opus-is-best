#include "audio.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <opus/opus.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <system_error>

namespace fs = std::filesystem;

namespace {

template<class T, void (*Free)(T**)>
struct AvDeleter {
    void operator()(T* value) const { Free(&value); }
};

template<class T, void (*Free)(T**)>
using AvPtr = std::unique_ptr<T, AvDeleter<T, Free>>;

bool cancelled(HANDLE cancel) {
    return WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0;
}

void checkCancelled(HANDLE cancel) {
    if (cancelled(cancel))
        throw std::wstring(L"音频处理已取消。");
}

constexpr auto readError = L"无法读取第一条音轨。请检查文件是否包含可解码的音频。";

void require(bool success, const wchar_t* message = readError) {
    if (!success)
        throw std::wstring(message);
}

class ReferencePcm {
public:
    ReferencePcm() {
        require(resampler_ && output_);
    }

    bool full() const { return pcm.size() == limit; }

    void append(AVFrame* input) {
        if (input->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
            const int count = input->ch_layout.nb_channels;
            av_channel_layout_default(&input->ch_layout, count);
        }
        prepareOutput();
        int result = swr_convert_frame(resampler_.get(), output_.get(), input);
        if (result == AVERROR_INPUT_CHANGED) {
            flush();
            swr_close(resampler_.get());
            prepareOutput();
            result = swr_convert_frame(resampler_.get(), output_.get(), input);
        }
        require(result >= 0);
        appendOutput();
    }

    void flush() {
        while (!full() && swr_is_initialized(resampler_.get())) {
            prepareOutput();
            require(swr_convert_frame(resampler_.get(), output_.get(), nullptr) >= 0);
            if (output_->nb_samples == 0)
                break;
            appendOutput();
        }
    }

    Pcm pcm;

private:
    void prepareOutput() {
        av_frame_unref(output_.get());
        output_->format = AV_SAMPLE_FMT_FLT;
        output_->sample_rate = sampleRate;
        av_channel_layout_default(&output_->ch_layout, channels);
    }

    void appendOutput() {
        const auto count = std::min(limit - pcm.size(), size_t(output_->nb_samples) * channels);
        const auto* samples = reinterpret_cast<const float*>(output_->data[0]);
        constexpr float gain = 0.501187234f; // -6 dB
        for (size_t i = 0; i < count; ++i) {
            const float scaled = std::isfinite(samples[i]) ? samples[i] * gain * 32768.0f : 0.0f;
            pcm.push_back(static_cast<std::int16_t>(std::lround(
                std::clamp(scaled, -32768.0f, 32767.0f))));
        }
    }

    static constexpr size_t limit = sourceSeconds * sampleRate * channels;
    AvPtr<SwrContext, swr_free> resampler_{swr_alloc()};
    AvPtr<AVFrame, av_frame_free> output_{av_frame_alloc()};
};

} // namespace

Pcm readReference(const fs::path& input, HANDLE cancel) {
    checkCancelled(cancel);
    const auto path = input.u8string();
    auto* context = avformat_alloc_context();
    require(context != nullptr);
    context->interrupt_callback = {
        [](void* event) -> int { return cancelled(event); }, cancel
    };
    const int whitelist = av_opt_set(context, "protocol_whitelist", "file", 0);
    if (whitelist < 0) {
        avformat_free_context(context);
        require(false);
    }
    const int opened = avformat_open_input(&context, path.c_str(), nullptr, nullptr);
    AvPtr<AVFormatContext, avformat_close_input> media{context};
    checkCancelled(cancel);
    require(opened >= 0);
    const int info = avformat_find_stream_info(media.get(), nullptr);
    checkCancelled(cancel);
    require(info >= 0);

    unsigned stream = 0;
    while (stream < media->nb_streams
           && media->streams[stream]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
        ++stream;
    require(stream < media->nb_streams);
    const auto* parameters = media->streams[stream]->codecpar;
    const auto* decoder = avcodec_find_decoder(parameters->codec_id);
    require(decoder != nullptr);
    AvPtr<AVCodecContext, avcodec_free_context> codec{avcodec_alloc_context3(decoder)};
    AvPtr<AVPacket, av_packet_free> packet{av_packet_alloc()};
    AvPtr<AVFrame, av_frame_free> frame{av_frame_alloc()};
    require(codec && packet && frame);
    require(avcodec_parameters_to_context(codec.get(), parameters) >= 0);
    codec->pkt_timebase = media->streams[stream]->time_base;
    require(avcodec_open2(codec.get(), decoder, nullptr) >= 0);

    ReferencePcm reference;
    const auto decode = [&](const AVPacket* encoded) {
        require(avcodec_send_packet(codec.get(), encoded) >= 0);
        while (!reference.full()) {
            checkCancelled(cancel);
            const int result = avcodec_receive_frame(codec.get(), frame.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
                break;
            require(result >= 0);
            reference.append(frame.get());
        }
    };

    while (!reference.full()) {
        checkCancelled(cancel);
        const int result = av_read_frame(media.get(), packet.get());
        checkCancelled(cancel);
        if (result == AVERROR_EOF) {
            decode(nullptr);
            break;
        }
        require(result >= 0);
        if (static_cast<unsigned>(packet->stream_index) == stream)
            decode(packet.get());
        av_packet_unref(packet.get());
    }
    reference.flush();
    require(!reference.pcm.empty(), L"没有读到有效的音频片段。");
    return std::move(reference.pcm);
}

Pcm encodeExcerpt(const Pcm& reference, size_t firstFrame, size_t frameCount,
                  int bitrate, HANDLE cancel) {
    checkCancelled(cancel);
    constexpr auto encodeError = L"Opus 编码失败。";
    constexpr auto decodeError = L"无法解码 Opus 测试片段。";
    const size_t available = reference.size() / channels;
    require(frameCount && firstFrame <= available && frameCount <= available - firstFrame,
            L"试听片段超出音轨范围。");

    std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)> encoder{
        opus_encoder_create(sampleRate, channels, OPUS_APPLICATION_AUDIO, nullptr),
        opus_encoder_destroy
    };
    std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> decoder{
        opus_decoder_create(sampleRate, channels, nullptr), opus_decoder_destroy
    };
    require(encoder != nullptr, encodeError);
    require(decoder != nullptr, decodeError);
    opus_int32 lookahead = 0;
    require(opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(bitrate * 1000)) == OPUS_OK
            && opus_encoder_ctl(encoder.get(), OPUS_SET_VBR(0)) == OPUS_OK
            && opus_encoder_ctl(encoder.get(), OPUS_GET_LOOKAHEAD(&lookahead)) == OPUS_OK,
            encodeError);

    constexpr size_t packetFrames = sampleRate / 50; // 20 ms
    const size_t end = frameCount + lookahead;
    const auto* source = reference.data() + firstFrame * channels;
    std::array<opus_int16, packetFrames * channels> input{}, output{};
    std::array<unsigned char, 4000> packet{};
    Pcm pcm;
    pcm.reserve(frameCount * channels);

    // Pad the tail to let the encoder emit all samples, then trim its delay.
    for (size_t position = 0; position < end; position += packetFrames) {
        checkCancelled(cancel);
        const size_t count = position < frameCount
            ? std::min(packetFrames, frameCount - position) : 0;
        std::fill(input.begin(), input.end(), 0);
        if (count)
            std::copy_n(source + position * channels, count * channels, input.begin());
        const int bytes = opus_encode(encoder.get(), input.data(), packetFrames,
                                      packet.data(), packet.size());
        require(bytes > 0, encodeError);
        require(opus_decode(decoder.get(), packet.data(), bytes, output.data(),
                            packetFrames, 0) == packetFrames, decodeError);

        const size_t first = std::max(position, size_t(lookahead));
        const size_t last = std::min(position + packetFrames, end);
        if (first < last)
            pcm.insert(pcm.end(), output.begin() + (first - position) * channels,
                       output.begin() + (last - position) * channels);
    }
    return pcm;
}

namespace {

constexpr size_t switchFrames = sampleRate * 10 / 1000;
constexpr size_t edgeFrames = sampleRate * 5 / 1000;
constexpr WORD frameBytes = channels * sizeof(std::int16_t);

double smoothRamp(size_t frame, size_t length) {
    if (frame >= length)
        return 1.0;
    constexpr double pi = 3.14159265358979323846;
    return 0.5 - 0.5 * std::cos(pi * static_cast<double>(frame) / length);
}

class Mixer {
public:
    explicit Mixer(bool selectB)
        : targetB_(selectB), start_(selectB), weight_(selectB) {}

    void select(bool selectB) {
        if (targetB_ == selectB)
            return;
        targetB_ = selectB;
        start_ = weight_;
        elapsed_ = 0;
    }

    void render(const Pcm& a, const Pcm& b, std::int16_t* output,
                size_t startFrame, size_t count, size_t endFrame) {
        for (size_t frame = startFrame; frame < startFrame + count; ++frame) {
            if (elapsed_ < switchFrames) {
                const double progress = smoothRamp(++elapsed_, switchFrames);
                weight_ = start_ + (static_cast<double>(targetB_) - start_) * progress;
            }
            const double gain = smoothRamp(std::min(frame, endFrame - frame - 1), edgeFrames);
            for (size_t channel = 0; channel < channels; ++channel) {
                const size_t index = frame * channels + channel;
                const double mixed = a[index] + (b[index] - a[index]) * weight_;
                *output++ = static_cast<std::int16_t>(std::lround(mixed * gain));
            }
        }
    }

private:
    bool targetB_;
    double start_;
    double weight_;
    size_t elapsed_ = switchFrames;
};

template<class Interface>
class ComInterface {
public:
    ComInterface() = default;
    ComInterface(const ComInterface&) = delete;
    ComInterface& operator=(const ComInterface&) = delete;
    ~ComInterface() { if (value_) value_->Release(); }
    Interface** put() { return &value_; }
    Interface* operator->() const { return value_; }

private:
    Interface* value_ = nullptr;
};

} // namespace

Player::~Player() {
    stop();
    if (wake_)
        CloseHandle(wake_);
}

bool Player::play(const Pcm& a, const Pcm& b, bool selectB) {
    if (running_) {
        selectB_ = selectB;
        return true;
    }
    if (!wake_ || a.empty() || a.size() != b.size() || a.size() % channels != 0)
        return false;
    if (worker_.joinable())
        worker_.join();

    ResetEvent(wake_);
    stopping_ = false;
    selectB_ = selectB;
    running_ = true;
    try {
        std::promise<bool> ready;
        auto result = ready.get_future();
        worker_ = std::thread(&Player::stream, this, std::cref(a), std::cref(b),
                              selectB, std::move(ready));
        if (result.get())
            return true;
        worker_.join();
    } catch (const std::system_error&) {
        stop();
    }
    running_ = false;
    return false;
}

void Player::stop() {
    if (worker_.joinable()) {
        stopping_ = true;
        SetEvent(wake_);
        worker_.join();
    }
}

void Player::stream(const Pcm& a, const Pcm& b, bool selectB, std::promise<bool> ready) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    const auto render = [&] {
        ComInterface<IMMDeviceEnumerator> enumerator;
        ComInterface<IMMDevice> device;
        ComInterface<IAudioClient> client;
        ComInterface<IAudioRenderClient> output;
        WAVEFORMATEX format{WAVE_FORMAT_PCM, channels, sampleRate, sampleRate * frameBytes,
                            frameBytes, 16, 0};
        constexpr DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
            | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        UINT32 capacity = 0;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(enumerator.put())))
            || FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, device.put()))
            || FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void**>(client.put())))
            || FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, &format, nullptr))
            || FAILED(client->SetEventHandle(wake_))
            || FAILED(client->GetBufferSize(&capacity))
            || FAILED(client->GetService(IID_PPV_ARGS(output.put()))))
            return false;

        Mixer mixer(selectB);
        size_t cursor = 0;
        size_t end = a.size() / channels;
        bool started = false;
        for (;;) {
            // Refill according to the endpoint's clock and available frames.
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding)))
                break;
            if (stopping_)
                end = std::min(end, cursor + edgeFrames);
            if (cursor == end && padding == 0)
                break;
            const auto count = static_cast<UINT32>(std::min<size_t>(capacity - padding, end - cursor));
            if (count) {
                BYTE* buffer = nullptr;
                if (FAILED(output->GetBuffer(count, &buffer)))
                    break;
                mixer.select(selectB_);
                mixer.render(a, b, reinterpret_cast<std::int16_t*>(buffer), cursor, count, end);
                if (FAILED(output->ReleaseBuffer(count, 0)))
                    break;
                cursor += count;
            }
            if (!started) {
                if (FAILED(client->Start()))
                    break;
                started = true;
                ready.set_value(true);
            }
            if (WaitForSingleObject(wake_, 500) != WAIT_OBJECT_0)
                break;
        }
        if (started)
            client->Stop();
        return started;
    };

    bool started = false;
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        started = render();
        CoUninitialize();
    }
    running_ = false;
    if (!started)
        ready.set_value(false);
}
