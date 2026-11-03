#include "stubuddy_audio_codec.h"
#include "config.h"

#include <driver/i2s_pdm.h>
#include <esp_log.h>
#include <freertos/task.h>
#include <algorithm>
#include <array>

namespace {
constexpr char kTag[] = "STUBUDDY_CODEC";
constexpr unsigned kWriteTimeoutMs = 1000;
using StereoBlock = std::array<int32_t, stubuddy::kFramesPerBlock * 2>;
}  // namespace

StubuddyAudioCodec::StubuddyAudioCodec() {
    input_sample_rate_ = stubuddy::kSampleRate;
    output_sample_rate_ = stubuddy::kSampleRate;
    input_channels_ = output_channels_ = 1;
}

StubuddyAudioCodec::~StubuddyAudioCodec() {
    ShutdownInput();
    Shutdown();
}

esp_err_t StubuddyAudioCodec::Initialize() {
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(XIAOZHI_I2S_PORT(1), I2S_ROLE_MASTER);
    channel.dma_desc_num = stubuddy::kDmaDescriptors;
    channel.dma_frame_num = stubuddy::kFramesPerBlock;
    channel.auto_clear_after_cb = true;
    esp_err_t result = i2s_new_channel(&channel, &tx_handle_, nullptr);
    if (result != ESP_OK)
        return result;
    i2s_std_config_t config = {};
    config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(stubuddy::kSampleRate);
    config.slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    config.gpio_cfg.bclk = stubuddy::kSpeakerBclk;
    config.gpio_cfg.ws = stubuddy::kSpeakerLrclk;
    config.gpio_cfg.dout = stubuddy::kSpeakerData;
    config.gpio_cfg.din = I2S_GPIO_UNUSED;
    return i2s_channel_init_std_mode(tx_handle_, &config);
}

void StubuddyAudioCodec::Start() {
    if (tx_handle_ || failed_)
        return;
    // Fixed diagnostic level. Do not read or overwrite persisted user volume.
    output_volume_ = 100;
    const esp_err_t result = Initialize();
    if (result != ESP_OK)
        Fail(result, "initialize I2S1");
    else
        ESP_LOGI(kTag, "AudioCodec ready: I2S1, 16000 Hz, 32-bit stereo; LEFT=0, RIGHT=PCM");
}

void StubuddyAudioCodec::EnableInput(bool enable) {
    if (!enable) {
        const esp_err_t result = ShutdownInput();
        if (result != ESP_OK)
            Fail(result, "stop input");
        else
            ESP_LOGI(kTag, "INPUT OFF: I2S0 released");
        return;
    }
    if (input_enabled_ || failed_)
        return;
    // This checkpoint deliberately uses sequential capture/playback.
    if (output_enabled_ || running_) {
        Fail(ESP_ERR_INVALID_STATE, "input requested during output");
        return;
    }
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(XIAOZHI_I2S_PORT(0), I2S_ROLE_MASTER);
    channel.dma_desc_num = 6;
    channel.dma_frame_num = 256;
    esp_err_t result = i2s_new_channel(&channel, nullptr, &rx_handle_);
    if (result == ESP_OK) {
        i2s_pdm_rx_config_t config = {};
        config.clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(stubuddy::kSampleRate);
        config.slot_cfg =
            I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
        config.slot_cfg.slot_mask = I2S_PDM_SLOT_LEFT;
        config.gpio_cfg.clk = stubuddy::kMicrophoneClock;
        config.gpio_cfg.din = stubuddy::kMicrophoneData;
        result = i2s_channel_init_pdm_rx_mode(rx_handle_, &config);
    }
#ifdef CONFIG_STUBUDDY_OPUS_LOOPBACK
    if (result == ESP_OK) {
        i2s_event_callbacks_t callbacks = {};
        callbacks.on_recv_q_ovf = OnReceiveOverflow;
        result = i2s_channel_register_event_callback(rx_handle_, &callbacks, this);
    }
#endif
    if (result == ESP_OK)
        result = i2s_channel_enable(rx_handle_);
    if (result != ESP_OK) {
        Fail(result, "enable PDM input");
        return;
    }
    input_running_ = true;
    AudioCodec::EnableInput(true);
    ESP_LOGI(kTag, "INPUT ON: I2S0 PDM CLK=3 DATA=42; 16000 Hz PCM16 mono, LEFT slot");
}

int StubuddyAudioCodec::Read(int16_t* data, int samples) {
    if (!input_enabled_ || !data || samples <= 0 || failed_)
        return 0;
    // AudioCodec::InputData only returns a bool. Fill the entire requested block
    // or fail; never report success with a partially initialized vector.
    for (int offset = 0; offset < samples;) {
        const size_t requested = std::min(samples - offset, 256) * sizeof(int16_t);
        size_t received = 0;
        const esp_err_t result =
            i2s_channel_read(rx_handle_, data + offset, requested, &received, 1000);
        if (result != ESP_OK || received == 0 || received > requested || received % 2 != 0) {
            Fail(result != ESP_OK ? result : ESP_ERR_INVALID_SIZE, "read PDM input");
            return 0;
        }
        offset += received / sizeof(int16_t);
    }
#ifdef CONFIG_STUBUDDY_OPUS_LOOPBACK
    if (rx_overflow_.load(std::memory_order_relaxed)) {
        Fail(ESP_ERR_TIMEOUT, "PDM DMA queue overflow during Opus capture");
        return 0;
    }
#endif
    return samples;
}

#ifdef CONFIG_STUBUDDY_OPUS_LOOPBACK
bool IRAM_ATTR StubuddyAudioCodec::OnReceiveOverflow(i2s_chan_handle_t, i2s_event_data_t*,
                                                     void* context) {
    static_cast<StubuddyAudioCodec*>(context)->rx_overflow_.store(true, std::memory_order_relaxed);
    return false;
}
#endif

esp_err_t StubuddyAudioCodec::ShutdownInput() {
    input_enabled_ = false;
    if (input_running_) {
        const esp_err_t result = i2s_channel_disable(rx_handle_);
        if (result != ESP_OK)
            return result;
        input_running_ = false;
    }
    if (rx_handle_) {
        const esp_err_t result = i2s_del_channel(rx_handle_);
        if (result != ESP_OK)
            return result;
        rx_handle_ = nullptr;
    }
    return ESP_OK;
}

esp_err_t StubuddyAudioCodec::WriteSlots(const int32_t* data, unsigned frames) {
    const size_t expected = frames * 2 * sizeof(int32_t);
    size_t written = 0;
    const esp_err_t result =
        i2s_channel_write(tx_handle_, data, expected, &written, kWriteTimeoutMs);
    return result != ESP_OK ? result : written == expected ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t StubuddyAudioCodec::WriteSilence(unsigned duration_ms) {
    const StereoBlock silence = {};
    unsigned remaining = duration_ms * (stubuddy::kSampleRate / 1000);
    while (remaining) {
        const unsigned frames = std::min(remaining, stubuddy::kFramesPerBlock);
        const esp_err_t result = WriteSlots(silence.data(), frames);
        if (result != ESP_OK)
            return result;
        remaining -= frames;
    }
    return ESP_OK;
}

void StubuddyAudioCodec::EnableOutput(bool enable) {
    if (!enable) {
        esp_err_t result = ESP_OK;
        if (running_) {
            result = WriteSilence(150);
            if (result == ESP_OK) {
                constexpr unsigned drain_ms =
                    (stubuddy::kDmaDescriptors * stubuddy::kFramesPerBlock * 1000 +
                     stubuddy::kSampleRate - 1) /
                        stubuddy::kSampleRate +
                    20;
                vTaskDelay(pdMS_TO_TICKS(drain_ms));
            }
        }
        const esp_err_t shutdown = Shutdown();
        if (result != ESP_OK || shutdown != ESP_OK)
            Fail(result != ESP_OK ? result : shutdown, "stop output");
        else
            ESP_LOGI(kTag, "OUTPUT OFF: CTRL=LOW; I2S1 released");
        return;
    }
    if (output_enabled_ || failed_)
        return;
    if (rx_handle_ || input_running_) {
        Fail(ESP_ERR_INVALID_STATE, "output requested before input shutdown");
        return;
    }
    Start();
    if (!tx_handle_ || failed_)
        return;
    StereoBlock silence = {};
    size_t loaded = 0;
    esp_err_t result =
        i2s_channel_preload_data(tx_handle_, silence.data(), sizeof(silence), &loaded);
    if (result == ESP_OK && loaded != sizeof(silence))
        result = ESP_ERR_INVALID_SIZE;
    if (result == ESP_OK)
        result = i2s_channel_enable(tx_handle_);
    if (result == ESP_OK) {
        running_ = true;
        result = WriteSilence(100);
    }
    if (result == ESP_OK)
        result = gpio_set_level(stubuddy::kAmplifierCtrl, stubuddy::kAmplifierRight);
    if (result == ESP_OK)
        result = WriteSilence(20);
    if (result != ESP_OK) {
        Fail(result, "enable output");
        return;
    }
    AudioCodec::EnableOutput(true);
    ESP_LOGI(kTag, "OUTPUT ON: CTRL=HIGH, RIGHT channel");
}

void StubuddyAudioCodec::AbortOutput() {
    // Mute first, then destroy TX/DMA. No trailing-silence drain: the old
    // response must not keep playing while the child begins speaking.
    const esp_err_t result = Shutdown();
    if (result != ESP_OK)
        Fail(result, "abort output");
    else
        ESP_LOGI(kTag, "OUTPUT ABORT: CTRL=LOW; I2S1/DMA discarded without drain");
}

int StubuddyAudioCodec::Write(const int16_t* data, int samples) {
    if (!output_enabled_ || !data || samples <= 0)
        return 0;
    const int volume = std::clamp(output_volume_, 0, 100);
    StereoBlock block = {};
    for (int offset = 0; offset < samples;) {
        const unsigned frames = std::min<unsigned>(samples - offset, stubuddy::kFramesPerBlock);
        for (unsigned i = 0; i < frames; ++i) {
            // Multiplication is defined for negative PCM; signed shifts are not.
            const int64_t value = int64_t(data[offset + i]) * 65536 * volume * volume / 10000;
            block[2 * i] = 0;
            block[2 * i + 1] = static_cast<int32_t>(value);
        }
        const esp_err_t result = WriteSlots(block.data(), frames);
        if (result != ESP_OK) {
            Fail(result, "write PCM");
            return offset;
        }
        offset += frames;
    }
    return samples;
}

esp_err_t StubuddyAudioCodec::Shutdown() {
    esp_err_t result = gpio_set_level(stubuddy::kAmplifierCtrl, stubuddy::kAmplifierOff);
    output_enabled_ = false;
    if (running_) {
        vTaskDelay(pdMS_TO_TICKS(10));
        const esp_err_t disabled = i2s_channel_disable(tx_handle_);
        if (result == ESP_OK)
            result = disabled;
        if (disabled == ESP_OK)
            running_ = false;
    }
    if (tx_handle_ && !running_) {
        const esp_err_t deleted = i2s_del_channel(tx_handle_);
        if (result == ESP_OK)
            result = deleted;
        if (deleted == ESP_OK)
            tx_handle_ = nullptr;
    }
    return result;
}

void StubuddyAudioCodec::Fail(esp_err_t error, const char* operation) {
    failed_ = true;
    ESP_LOGE(kTag, "%s: %s; amplifier shutting down", operation, esp_err_to_name(error));
    const esp_err_t result = Shutdown();
    if (result != ESP_OK)
        ESP_LOGE(kTag, "Cleanup failed: %s", esp_err_to_name(result));
    const esp_err_t input_result = ShutdownInput();
    if (input_result != ESP_OK)
        ESP_LOGE(kTag, "Input cleanup failed: %s", esp_err_to_name(input_result));
}
