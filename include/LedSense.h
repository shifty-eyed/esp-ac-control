#pragma once

#include <Arduino.h>

class LedSense {
public:
  LedSense() = default;

  inline void begin(uint8_t pin) {
    cfg_.pin = pin;

    filteredOn_ = false;
    acc_ = 0;
    lastSampleMs_ = millis();

    wait(cfg_.primeMs);
  }

  inline void update() {
    const uint32_t now = millis();
    if (now - lastSampleMs_ < cfg_.samplePeriodMs) return;
    lastSampleMs_ = now;

    const bool rawOn = digitalRead(cfg_.pin) == LOW;

    if (rawOn) {
      acc_ = (acc_ > static_cast<uint8_t>(255 - cfg_.stepUp)) ? 255 : static_cast<uint8_t>(acc_ + cfg_.stepUp);
    } else {
      acc_ = (acc_ < cfg_.stepDown) ? 0 : static_cast<uint8_t>(acc_ - cfg_.stepDown);
    }

    // Hysteresis thresholds
    if (!filteredOn_ && acc_ >= cfg_.onThreshold) filteredOn_ = true;
    if ( filteredOn_ && acc_ <= cfg_.offThreshold) filteredOn_ = false;
  }

  bool isOn() const { return filteredOn_; }

  inline void wait(uint32_t durationMs) {
    const uint32_t start = millis();
    while (millis() - start < durationMs) {
      update();
      delay(1);
    }
  }

private:
  struct Config {
    uint8_t pin = 32;

    uint32_t samplePeriodMs = 10;          // how often we sample input

    uint8_t stepUp = 18;                   // accumulator increment when raw==ON
    uint8_t stepDown = 8;                  // accumulator decrement when raw==OFF

    uint8_t onThreshold = 200;             // hysteresis high threshold
    uint8_t offThreshold = 55;             // hysteresis low threshold

    uint32_t primeMs = 300;                // prime duration at begin()
  };

  Config cfg_{};

  bool filteredOn_ = false;
  uint8_t acc_ = 0;
  uint32_t lastSampleMs_ = 0;
};


