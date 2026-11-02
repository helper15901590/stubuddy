#pragma once

#include <driver/gpio.h>

namespace stubuddy {
inline constexpr gpio_num_t kSpeakerBclk = GPIO_NUM_46;
inline constexpr gpio_num_t kSpeakerLrclk = GPIO_NUM_9;
inline constexpr gpio_num_t kSpeakerData = GPIO_NUM_8;
inline constexpr gpio_num_t kAmplifierCtrl = GPIO_NUM_10;
inline constexpr gpio_num_t kMicrophoneClock = GPIO_NUM_3;
inline constexpr gpio_num_t kMicrophoneData = GPIO_NUM_42;
inline constexpr gpio_num_t kMainButton = GPIO_NUM_15;
inline constexpr gpio_num_t kStatusLed = GPIO_NUM_18;
inline constexpr int kAmplifierOff = 0;
inline constexpr int kAmplifierRight = 1;
inline constexpr int kLedOff = 1;  // GPIO18 is active-low on the accepted hardware.
inline constexpr unsigned kSampleRate = 16000;
inline constexpr unsigned kDmaDescriptors = 4;
inline constexpr unsigned kFramesPerBlock = 128;
}  // namespace stubuddy
