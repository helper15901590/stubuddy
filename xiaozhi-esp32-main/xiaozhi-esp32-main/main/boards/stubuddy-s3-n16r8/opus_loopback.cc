#include "opus_loopback.h"

#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_opus_dec.h>
#include <esp_opus_enc.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <cstring>
#include <new>

namespace stubuddy {
namespace {
constexpr char kTag[] = "STUBUDDY_OPUS";
constexpr unsigned kFrameUs = 60000;
}  // namespace

OpusLoopback::~OpusLoopback() {
    Erase();
    if (encoder_)
        esp_opus_enc_close(encoder_);
    if (decoder_)
        esp_opus_dec_close(decoder_);
    heap_caps_free(storage_);
    heap_caps_free(packet_);
}

esp_err_t OpusLoopback::Check(int result, const char* operation) {
    if (result == ESP_AUDIO_ERR_OK)
        return ESP_OK;
    ++errors_;
    ESP_LOGE(kTag, "CODEC ERROR: %s; code=%d; errors=%u; no PCM bypass", operation, result,
             errors_);
    return ESP_FAIL;
}

esp_err_t OpusLoopback::Initialize() {
    ESP_RETURN_ON_FALSE(!encoder_ && !decoder_, ESP_ERR_INVALID_STATE, kTag, "Already initialized");
    esp_opus_enc_config_t encoder = {};
    encoder.sample_rate = 16000;
    encoder.channel = 1;
    encoder.bits_per_sample = 16;
    encoder.bitrate = ESP_OPUS_BITRATE_AUTO;
    encoder.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    encoder.application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO;
    encoder.complexity = 0;
    encoder.enable_fec = false;
    encoder.enable_dtx = false;  // Keep every silent frame and its duration.
    encoder.enable_vbr = true;
    ESP_RETURN_ON_ERROR(
        Check(esp_opus_enc_open(&encoder, sizeof(encoder), &encoder_), "open encoder"), kTag,
        "Encoder unavailable");
    int input_bytes = 0, output_bytes = 0;
    ESP_RETURN_ON_ERROR(Check(esp_opus_enc_get_frame_size(encoder_, &input_bytes, &output_bytes),
                              "query frame sizes"),
                        kTag, "Frame query failed");
    ESP_RETURN_ON_FALSE(input_bytes == int(input_.size() * sizeof(int16_t)) && output_bytes > 0 &&
                            output_bytes <= int(kOpusMaxPacketBytes),
                        ESP_ERR_INVALID_SIZE, kTag, "Unexpected frame sizes: in=%d out=%d",
                        input_bytes, output_bytes);
    packet_capacity_ = size_t(output_bytes);
    const size_t capacity = kOpusMaxPackets * (packet_capacity_ + 2);
    storage_ =
        static_cast<uint8_t*>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    packet_ = static_cast<uint8_t*>(
        heap_caps_malloc(packet_capacity_, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(storage_ && packet_, ESP_ERR_NO_MEM, kTag,
                        "Cannot allocate bounded packet storage: %u bytes", unsigned(capacity));
    packets_.reset(new (std::nothrow) OpusPacketStore(storage_, capacity, packet_capacity_));
    ESP_RETURN_ON_FALSE(packets_ != nullptr, ESP_ERR_NO_MEM, kTag, "Packet store unavailable");
    esp_opus_dec_cfg_t decoder = {};
    decoder.sample_rate = 16000;
    decoder.channel = 1;
    decoder.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    decoder.self_delimited = false;
    ESP_RETURN_ON_ERROR(
        Check(esp_opus_dec_open(&decoder, sizeof(decoder), &decoder_), "open decoder"), kTag,
        "Decoder unavailable");
    ESP_LOGI(
        kTag,
        "CONFIG: 16000Hz mono PCM16; frame=60ms/960 samples; AUDIO complexity=0; VBR=ON DTX=OFF");
    ESP_LOGI(kTag, "STORAGE: max_packets=%u; packet_limit=%u; PSRAM=%u bytes; allocated once",
             unsigned(kOpusMaxPackets), unsigned(packet_capacity_), unsigned(capacity));
    ESP_RETURN_ON_ERROR(MeasureDelay(), kTag, "Silent codec self-test failed");
    Erase();
    return ESP_OK;
}

esp_err_t OpusLoopback::Begin(unsigned round) {
    ESP_RETURN_ON_FALSE(encoder_ && decoder_ && packets_, ESP_ERR_INVALID_STATE, kTag,
                        "Codec not initialized");
    // No handle recreation or bulk erasure on the interrupt-to-record path.
    ESP_RETURN_ON_ERROR(Check(esp_opus_enc_reset(encoder_), "reset encoder"), kTag,
                        "Encoder reset failed");
    ESP_RETURN_ON_ERROR(Check(esp_opus_dec_reset(decoder_), "reset decoder"), kTag,
                        "Decoder reset failed");
    packets_->Reset();
#ifdef CONFIG_STUBUDDY_WS_ECHO
    encoded_frames_ = 0;
#endif
    pending_ = samples_ = 0;
    round_ = round;
    encode_us_ = encode_max_us_ = decode_max_us_ = 0;
    input_.fill(0);
    capturing_ = true;
    return ESP_OK;
}

esp_err_t OpusLoopback::EncodeFrame() {
    esp_audio_enc_in_frame_t in = {};
    in.buffer = reinterpret_cast<uint8_t*>(input_.data());
    in.len = input_.size() * sizeof(int16_t);
    esp_audio_enc_out_frame_t out = {};
    out.buffer = packet_;
    out.len = packet_capacity_;
    const int64_t start = esp_timer_get_time();
    const int result = esp_opus_enc_process(encoder_, &in, &out);
    const unsigned elapsed = static_cast<unsigned>(esp_timer_get_time() - start);
    encode_us_ += elapsed;
    encode_max_us_ = std::max(encode_max_us_, elapsed);
    ESP_RETURN_ON_ERROR(Check(result, "encode frame"), kTag, "Encoding failed");
    ESP_RETURN_ON_FALSE(out.encoded_bytes > 0 && out.encoded_bytes <= packet_capacity_,
                        ESP_ERR_INVALID_SIZE, kTag, "Invalid packet or bounded storage exhausted");
#ifdef CONFIG_STUBUDDY_WS_ECHO
    ++encoded_frames_;
    if (sink_)
        sink_(sink_context_, packet_, out.encoded_bytes);  // Non-blocking bounded queue only.
    else
#endif
        ESP_RETURN_ON_FALSE(packets_->Push(packet_, out.encoded_bytes), ESP_ERR_NO_MEM, kTag,
                            "Bounded packet storage exhausted");
    pending_ = 0;
    return ESP_OK;
}

esp_err_t OpusLoopback::Append(const int16_t* pcm, size_t samples) {
    ESP_RETURN_ON_FALSE(
        capturing_ && pcm && samples_ <= kOpusMaxSamples && samples <= kOpusMaxSamples - samples_,
        ESP_ERR_INVALID_STATE, kTag, "Capture length/state invalid");
    samples_ += samples;
    while (samples) {
        const size_t count = std::min(samples, input_.size() - pending_);
        std::memcpy(input_.data() + pending_, pcm, count * sizeof(int16_t));
        pending_ += count;
        pcm += count;
        samples -= count;
        if (pending_ == input_.size())
            ESP_RETURN_ON_ERROR(EncodeFrame(), kTag, "Incremental encode failed");
    }
    return ESP_OK;
}

esp_err_t OpusLoopback::DecodePacket(OpusPacketView packet) {
    esp_audio_dec_in_raw_t raw = {};
    raw.buffer = packet.data;
    raw.len = packet.size;
    raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;
    esp_audio_dec_out_frame_t out = {};
    out.buffer = reinterpret_cast<uint8_t*>(decoded_.data());
    out.len = decoded_.size() * sizeof(int16_t);
    esp_audio_dec_info_t info = {};
    const int64_t start = esp_timer_get_time();
    const int result = esp_opus_dec_decode(decoder_, &raw, &out, &info);
    decode_max_us_ = std::max(decode_max_us_, static_cast<unsigned>(esp_timer_get_time() - start));
    ESP_RETURN_ON_ERROR(Check(result, "decode packet"), kTag, "Decoding failed");
    ESP_RETURN_ON_FALSE(
        raw.consumed == raw.len && out.decoded_size == out.len && info.sample_rate == 16000 &&
            info.channel == 1 && info.bits_per_sample == 16,
        ESP_ERR_INVALID_SIZE, kTag, "Incomplete packet or unexpected decoded format");
    return ESP_OK;
}

esp_err_t OpusLoopback::MeasureDelay() {
    constexpr size_t reference_size = kOpusFrameSamples * 3;
    constexpr size_t decoded_size = reference_size + kOpusFrameSamples;
    auto* test = static_cast<int16_t*>(heap_caps_malloc(
        (reference_size + decoded_size) * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(test, ESP_ERR_NO_MEM, kTag, "Self-test memory unavailable");
    struct TestMemory {
        int16_t* data;
        ~TestMemory() {
            std::memset(data, 0, (reference_size + decoded_size) * sizeof(int16_t));
            heap_caps_free(data);
        }
    } cleanup{test};
    uint32_t random = 0x53545542;
    int32_t filtered = 0;
    for (size_t i = 0; i < reference_size; ++i) {
        random = random * 1664525U + 1013904223U;
        const int32_t white = int32_t(random >> 16) - 32768;
        filtered = (7 * filtered + white / 2) / 8;
        test[i] = static_cast<int16_t>(filtered);
    }
    ESP_RETURN_ON_ERROR(Begin(0), kTag, "Self-test reset failed");
    ESP_RETURN_ON_ERROR(Append(test, reference_size), kTag, "Self-test encode failed");
    input_.fill(0);
    ESP_RETURN_ON_ERROR(EncodeFrame(), kTag, "Self-test tail flush failed");
    size_t cursor = 0;
    for (size_t index = 0; index < packets_->count(); ++index) {
        OpusPacketView packet;
        ESP_RETURN_ON_FALSE(packets_->Next(cursor, packet), ESP_FAIL, kTag,
                            "Self-test packet invalid");
        ESP_RETURN_ON_ERROR(DecodePacket(packet), kTag, "Self-test decode failed");
        std::memcpy(test + reference_size + index * kOpusFrameSamples, decoded_.data(),
                    decoded_.size() * sizeof(int16_t));
        vTaskDelay(1);
    }
    const auto estimate =
        EstimateOpusDelay(test, reference_size, test + reference_size, decoded_size);
    ESP_RETURN_ON_FALSE(estimate.correlation >= 0.70 && estimate.samples < kOpusFrameSamples,
                        ESP_FAIL, kTag, "Unreliable codec delay: samples=%u correlation=%.3f",
                        unsigned(estimate.samples), estimate.correlation);
    delay_ = estimate.samples;
    ESP_LOGI(
        kTag,
        "SELFTEST PASS: silent RAM encode/decode; delay=%u samples; correlation=%.3f; errors=%u",
        unsigned(delay_), estimate.correlation, errors_);
    return ESP_OK;
}

esp_err_t OpusLoopback::FinishAndDecode(int16_t* pcm, size_t samples) {
    ESP_RETURN_ON_FALSE(capturing_ && pcm && samples == samples_ && samples > 0,
                        ESP_ERR_INVALID_STATE, kTag, "Cannot finish this recording");
    const size_t needed = OpusPacketsNeeded(samples, delay_);
    ESP_RETURN_ON_FALSE(needed > 0, ESP_ERR_INVALID_SIZE, kTag, "Invalid original length");
    // Pad the last partial frame; add a zero frame only when it is necessary
    // to flush the measured codec delay, including an exact 60-second capture.
    while (packets_->count() < needed) {
        std::fill(input_.begin() + pending_, input_.end(), 0);
        ESP_RETURN_ON_ERROR(EncodeFrame(), kTag, "Tail encode failed");
        vTaskDelay(1);
    }
    capturing_ = false;
    ESP_RETURN_ON_FALSE(encode_us_ / packets_->count() < kFrameUs, ESP_ERR_TIMEOUT, kTag,
                        "Encoding cannot sustain realtime capture; no replay");
    size_t cursor = 0, copied = 0;
    for (size_t index = 0; index < packets_->count(); ++index) {
        OpusPacketView packet;
        ESP_RETURN_ON_FALSE(packets_->Next(cursor, packet), ESP_FAIL, kTag, "Malformed RAM packet");
        ESP_RETURN_ON_ERROR(DecodePacket(packet), kTag, "Loopback decode failed");
        copied += CopyOpusPcm(decoded_.data(), decoded_.size(), index * kOpusFrameSamples, delay_,
                              samples, pcm, samples);
        // Yield in bounded batches; one tick per frame would add ten seconds
        // to a 60-second clip when FreeRTOS uses a 100Hz tick.
        if (index % 16 == 15)
            vTaskDelay(1);
    }
    ESP_RETURN_ON_FALSE(cursor == packets_->used() && copied == samples, ESP_ERR_INVALID_SIZE, kTag,
                        "Decoded sample count mismatch: wanted=%u copied=%u", unsigned(samples),
                        unsigned(copied));
    ESP_LOGI(kTag,
             "LOOPBACK: round=%u; raw=%u bytes; opus=%u bytes; packets=%u; decoded_samples=%u; "
             "delay=%u; errors=%u",
             round_, unsigned(samples * sizeof(int16_t)), unsigned(packets_->payload_bytes()),
             unsigned(packets_->count()), unsigned(copied), unsigned(delay_), errors_);
    ESP_LOGI(kTag, "TIMING: enc_avg=%u us; enc_max=%u us; dec_max=%u us; stack_min_free=%u",
             unsigned(encode_us_ / packets_->count()), encode_max_us_, decode_max_us_,
             unsigned(uxTaskGetStackHighWaterMark(nullptr)));
    return ESP_OK;
}

void OpusLoopback::Discard() {
    capturing_ = false;
    if (packets_)
        packets_->Reset();
    pending_ = samples_ = 0;
}

void OpusLoopback::Erase() {
    Discard();
    if (packets_)
        packets_->Erase();
    input_.fill(0);
    decoded_.fill(0);
    if (packet_)
        std::memset(packet_, 0, packet_capacity_);
}

#ifdef CONFIG_STUBUDDY_WS_ECHO
esp_err_t OpusLoopback::FinishCapture(size_t samples) {
    ESP_RETURN_ON_FALSE(capturing_ && sink_ && samples == samples_ && samples > 0,
                        ESP_ERR_INVALID_STATE, kTag, "Invalid remote capture state");
    const size_t needed = OpusPacketsNeeded(samples, delay_);
    ESP_RETURN_ON_FALSE(needed > 0, ESP_ERR_INVALID_SIZE, kTag, "Invalid capture length");
    while (encoded_frames_ < needed) {
        std::fill(input_.begin() + pending_, input_.end(), 0);
        ESP_RETURN_ON_ERROR(EncodeFrame(), kTag, "Remote tail encode failed");
        vTaskDelay(1);
    }
    capturing_ = false;
    ESP_RETURN_ON_FALSE(encode_us_ / encoded_frames_ < kFrameUs, ESP_ERR_TIMEOUT, kTag,
                        "Encoding cannot sustain realtime capture");
    ESP_LOGI(kTag, "UPLINK ENCODED: round=%u; samples=%u; frames=%u; enc_max=%u us; errors=%u",
             round_, unsigned(samples), unsigned(encoded_frames_), encode_max_us_, errors_);
    return ESP_OK;
}

esp_err_t OpusLoopback::DecodeEcho(int16_t* pcm, size_t samples, bool (*proceed)(void*),
                                   void* context) {
    ESP_RETURN_ON_FALSE(!capturing_ && pcm && samples == samples_ && samples > 0 &&
                            packets_->count() == OpusPacketsNeeded(samples, delay_),
                        ESP_ERR_INVALID_STATE, kTag, "Incomplete remote echo");
    ESP_RETURN_ON_ERROR(Check(esp_opus_dec_reset(decoder_), "reset echo decoder"), kTag,
                        "Echo decoder reset failed");
    size_t cursor = 0, copied = 0;
    for (size_t index = 0; index < packets_->count(); ++index) {
        if (proceed && !proceed(context))
            return ESP_ERR_INVALID_STATE;  // User interruption, not a codec error.
        OpusPacketView packet;
        ESP_RETURN_ON_FALSE(packets_->Next(cursor, packet), ESP_FAIL, kTag, "Bad returned packet");
        ESP_RETURN_ON_ERROR(DecodePacket(packet), kTag, "Remote echo decode failed");
        copied += CopyOpusPcm(decoded_.data(), decoded_.size(), index * kOpusFrameSamples, delay_,
                              samples, pcm, samples);
        if (index % 16 == 15)
            vTaskDelay(1);
    }
    ESP_RETURN_ON_FALSE(copied == samples && cursor == packets_->used(), ESP_ERR_INVALID_SIZE, kTag,
                        "Echo sample count mismatch");
    ESP_LOGI(kTag, "REMOTE DECODE: round=%u; decoded_samples=%u; errors=%u; dec_max=%u us", round_,
             unsigned(copied), errors_, decode_max_us_);
    return ESP_OK;
}
#endif
}  // namespace stubuddy
