#pragma once

#include <driver/gpio.h>

namespace stubuddy {

// These are ESP32-S3 GPIO numbers, not module package pin numbers.
inline constexpr gpio_num_t kSpeakerBclk = GPIO_NUM_46;
inline constexpr gpio_num_t kSpeakerLrclk = GPIO_NUM_9;
inline constexpr gpio_num_t kSpeakerData = GPIO_NUM_8;
inline constexpr gpio_num_t kAmplifierCtrl = GPIO_NUM_10;
inline constexpr gpio_num_t kMicrophoneClock = GPIO_NUM_3;
inline constexpr gpio_num_t kMicrophoneData = GPIO_NUM_42;
inline constexpr gpio_num_t kMainButton = GPIO_NUM_15;
inline constexpr gpio_num_t kStatusLed = GPIO_NUM_18;

// NS4168 CTRL high selects RIGHT; CTRL low shuts down the amplifier.
// Selecting LEFT needs an external 0.9-1.15 V CTRL bias, not a GPIO high.
inline constexpr int kAmplifierOff = 0;
inline constexpr int kAmplifierRight = 1;

}  // namespace stubuddy
