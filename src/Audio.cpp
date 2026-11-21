#include "Audio.h"

#include <driver/i2s.h>
#include <math.h>

namespace {

constexpr i2s_port_t I2S_PORT {I2S_NUM_0};

// One cycle of a sine wave, indexed by the top bits of the phase accumulator
constexpr uint32_t SINE_BITS {10};
constexpr uint32_t SINE_LEN  {1U << SINE_BITS};
int16_t s_sine[SINE_LEN];

// Gain smoothing: each sample moves 1/2^N of the way to the target, a time
// constant of 2^N samples (16 ms at 16 kHz)
constexpr int GAIN_SMOOTH_SHIFT {8};

constexpr int32_t FULL_SCALE {32767};
constexpr uint32_t EDGE_SAMPLES {AUDIO_SAMPLE_RATE * BEEP_EDGE_MS / 1000};

// Shared with the audio task
portMUX_TYPE       s_mux = portMUX_INITIALIZER_UNLOCKED;
Audio::Pattern     s_pattern {};           // guarded by s_mux
bool               s_patternChanged {false};  // guarded by s_mux
volatile bool      s_run {false};
volatile int32_t   s_targetGain {0};       // Q15 amplitude, 0-FULL_SCALE
TaskHandle_t       s_task {nullptr};

// The loop task's copy, so play() can tell whether anything changed
Audio::Pattern     s_lastPattern {};
bool               s_playing {false};

// A pattern converted to sample counts
struct Timing {
  Waveform wave;
  uint32_t phaseInc;  // per sample, 2^32 = one cycle
  uint32_t beep;
  uint32_t slot;      // beep + gap
  uint32_t beeps;
  uint32_t period;    // whole burst plus pause; 0 = silent
};

uint32_t msToSamples(uint32_t ms) { return ms * AUDIO_SAMPLE_RATE / 1000; }

Timing toTiming(const Audio::Pattern& p) {
  Timing t {};
  t.wave     = p.wave;
  t.phaseInc = (uint32_t)(((uint64_t)p.freqHz << 32) / AUDIO_SAMPLE_RATE);
  t.beep     = msToSamples(p.beepMs);
  t.slot     = t.beep + msToSamples(p.gapMs);
  t.beeps    = p.beeps;
  const uint32_t burst{t.beeps == 0 ? 0 : t.beeps * t.slot - msToSamples(p.gapMs)};
  t.period   = (t.beep == 0 || t.beeps == 0) ? 0 : burst + msToSamples(p.pauseMs);
  return t;
}

// Beep envelope at sample `pos` of the pattern, Q15: 0 between beeps, with
// a short linear fade in and out at each beep's edges
int32_t envelope(const Timing& t, uint32_t pos) {
  const uint32_t k{pos / t.slot};
  const uint32_t off{pos % t.slot};
  if (k >= t.beeps || off >= t.beep) return 0;
  uint32_t edge{off < t.beep - 1 - off ? off : t.beep - 1 - off};
  if (edge >= EDGE_SAMPLES) return FULL_SCALE;
  return (int32_t)(edge * FULL_SCALE / EDGE_SAMPLES);
}

int32_t oscillator(const Timing& t, uint32_t phase) {
  if (t.wave == WAVE_SQUARE) return (phase & 0x80000000U) ? FULL_SCALE : -FULL_SCALE;
  return s_sine[phase >> (32 - SINE_BITS)];
}

void ampEnable(bool on) {
  if (AMP_SD_PIN >= 0) digitalWrite(AMP_SD_PIN, on ? HIGH : LOW);
}

void audioTask(void*) {
  static int16_t buf[AUDIO_CHUNK_FRAMES * 2];  // interleaved L/R
  Timing   timing {};
  uint32_t pos {0};
  uint32_t phase {0};
  int32_t  gain {0};
  bool     running {false};

  for (;;) {
    if (!running) {
      if (!s_run) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        continue;
      }
      i2s_zero_dma_buffer(I2S_PORT);
      i2s_start(I2S_PORT);
      ampEnable(true);
      running = true;
      pos = 0;
      phase = 0;
      gain = 0;
    }

    portENTER_CRITICAL(&s_mux);
    if (s_patternChanged) {
      timing = toTiming(s_pattern);
      s_patternChanged = false;
      if (pos >= timing.period) pos = 0;
    }
    portEXIT_CRITICAL(&s_mux);

    const int32_t target{s_run ? s_targetGain : 0};
    for (size_t i{0}; i < AUDIO_CHUNK_FRAMES; i++) {
      gain += (target - gain) >> GAIN_SMOOTH_SHIFT;
      int32_t v{0};
      if (timing.period > 0) {
        v = (oscillator(timing, phase) * envelope(timing, pos)) >> 15;
        v = (v * gain) >> 15;
        if (++pos >= timing.period) pos = 0;
      }
      phase += timing.phaseInc;
      buf[2 * i]     = (int16_t)v;
      buf[2 * i + 1] = (int16_t)v;
    }

    size_t written;
    i2s_write(I2S_PORT, buf, sizeof(buf), &written, portMAX_DELAY);

    // Faded all the way out: shut down until the next play()
    if (!s_run && gain == 0) {
      ampEnable(false);
      i2s_zero_dma_buffer(I2S_PORT);
      i2s_stop(I2S_PORT);
      running = false;
    }
  }
}

}  // namespace

namespace Audio {

void begin() {
  for (uint32_t i{0}; i < SINE_LEN; i++) {
    s_sine[i] = (int16_t)lroundf(sinf(2.0F * (float)M_PI * i / SINE_LEN) * FULL_SCALE);
  }

  if (AMP_SD_PIN >= 0) {
    pinMode(AMP_SD_PIN, OUTPUT);
    ampEnable(false);
  }

  i2s_config_t cfg {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate          = AUDIO_SAMPLE_RATE;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = 0;
  cfg.dma_buf_count        = AUDIO_DMA_BUF_COUNT;
  cfg.dma_buf_len          = AUDIO_DMA_BUF_LEN;
  cfg.use_apll             = false;
  cfg.tx_desc_auto_clear   = true;  // silence, not repeated audio, on underrun

  i2s_pin_config_t pins {};
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
  pins.bck_io_num   = I2S_BCLK_PIN;
  pins.ws_io_num    = I2S_LRCLK_PIN;
  pins.data_out_num = I2S_DOUT_PIN;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;

  if (i2s_driver_install(I2S_PORT, &cfg, 0, nullptr) != ESP_OK ||
      i2s_set_pin(I2S_PORT, &pins) != ESP_OK) {
    Serial.println("I2S init failed -- no sound");
    return;
  }
  i2s_zero_dma_buffer(I2S_PORT);
  i2s_stop(I2S_PORT);

  xTaskCreate(audioTask, "audio", AUDIO_TASK_STACK, nullptr, AUDIO_TASK_PRIORITY, &s_task);
}

void play(const Pattern& pattern) {
  if (!s_task) return;
  if (!s_playing || pattern != s_lastPattern) {
    s_lastPattern = pattern;
    portENTER_CRITICAL(&s_mux);
    s_pattern = pattern;
    s_patternChanged = true;
    portEXIT_CRITICAL(&s_mux);
  }
  if (!s_playing) {
    s_playing = true;
    s_run = true;
    xTaskNotifyGive(s_task);
  }
}

void setVolume(float percent) {
  if (percent < 0.0F) percent = 0.0F;
  if (percent > 100.0F) percent = 100.0F;
  const float level{percent / 100.0F};
  s_targetGain = (int32_t)(level * level * AUDIO_HEADROOM * FULL_SCALE);
}

void stop() {
  s_playing = false;
  s_run = false;
}

bool playing() { return s_playing; }

}  // namespace Audio
