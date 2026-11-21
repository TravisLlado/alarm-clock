#pragma once

// Audio - beep patterns on an I2S amplifier (MAX98357A)
//
// A dedicated FreeRTOS task generates the samples and feeds the I2S DMA, so
// the sound never stutters while the main loop is blocked (e.g. waiting on an
// HTTP request to the bed sensor). Between alarms the I2S clocks are stopped
// and the amp is shut down through its SD pin.
//
// All functions are called from the loop task.

#include <Arduino.h>
#include "config.h"

namespace Audio {

  struct Pattern {
    Waveform wave;
    uint16_t freqHz;
    uint16_t beepMs;   // each beep
    uint16_t gapMs;    // between beeps in a burst
    uint8_t  beeps;    // beeps per burst
    uint16_t pauseMs;  // silence after each burst

    bool operator==(const Pattern& o) const {
      return wave == o.wave && freqHz == o.freqHz && beepMs == o.beepMs &&
             gapMs == o.gapMs && beeps == o.beeps && pauseMs == o.pauseMs;
    }
    bool operator!=(const Pattern& o) const { return !(*this == o); }
  };

  // Set up I2S and start the audio task. Silent until play().
  void begin();

  // Start playing `pattern`, or switch to it if already playing. Safe to
  // call every tick: an unchanged pattern carries on where it is.
  void play(const Pattern& pattern);

  // Output level, 0-100 on a perceptual scale (amplitude follows the square
  // of it). Changes are smoothed over a few ms, so ramps don't click.
  void setVolume(float percent);

  // Fade out and shut the amp down
  void stop();

  // True from play() until stop()
  bool playing();

}  // namespace Audio
