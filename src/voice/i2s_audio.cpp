#include "i2s_audio.h"
#include "hw/pins.h"
#include <Arduino.h>
#include <driver/i2s_std.h>
#include <driver/i2s_common.h>
#include <Wire.h>

static i2s_chan_handle_t s_txHandle = nullptr;
static bool s_txEnabled = false;
static bool s_paOn = false;

// ES8311 I2C address: the CE pin is pulled low on these boards -> 0x18.
static const uint8_t ES8311_ADDR = 0x18;

// MCLK for the codec is 256 * fs = 4.096 MHz at fs = 16 kHz (see audioInit).
// The clock-divider table below is for that exact pair.

static bool es8311WriteReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool es8311ReadReg(uint8_t reg, uint8_t* val) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;  // repeated start
    if (Wire.requestFrom((uint8_t)ES8311_ADDR, (uint8_t)1) != 1) return false;
    *val = (uint8_t)Wire.read();
    return true;
}

// Program the ES8311 as an I2S slave for 16 kHz / 16-bit playback.
//
// Ported from the official Waveshare 07_ES8311 example (es8311.c). The
// previous hand-rolled sequence was incomplete and, among other things,
// cleared reg01 bit5 (MCLK_ON) at the end, which disables the codec's clock
// manager — so even when the chip answered on I2C it produced no sound.
static bool initEs8311() {
    // The ES8311 occasionally misses the very first transaction after
    // power-up (also noted in esp_codec_dev), so probe a few times first.
    bool present = false;
    for (uint8_t attempt = 0; attempt < 3 && !present; ++attempt) {
        Wire.beginTransmission(ES8311_ADDR);
        present = (Wire.endTransmission() == 0);
        if (!present) delay(10);
    }
    if (!present) {
        Serial.println("i2s_audio: ES8311 not found on I2C");
        return false;
    }

    // Reset to defaults, then power on the chip.
    es8311WriteReg(0x00, 0x1F);
    delay(10);
    es8311WriteReg(0x00, 0x00);
    es8311WriteReg(0x00, 0x80);

    // Clock manager: take internal MCLK from the MCLK pin (reg01 bit7 = 0),
    // no inversion, all clocks enabled; SCLK not inverted (reg06 bit5 = 0).
    es8311WriteReg(0x01, 0x3F);
    uint8_t r = 0;
    if (es8311ReadReg(0x06, &r)) es8311WriteReg(0x06, (uint8_t)(r & ~0x20));

    // Clock dividers/OSR for (MCLK 4.096 MHz, fs 16 kHz):
    // pre_div=1, pre_multi=1x, adc_div=1, dac_div=1, bclk_div=4, lrck=0x0FF.
    if (es8311ReadReg(0x02, &r)) es8311WriteReg(0x02, (uint8_t)(r & 0x07));
    es8311WriteReg(0x03, 0x10);  // adc osr, single speed
    es8311WriteReg(0x04, 0x10);  // dac osr
    es8311WriteReg(0x05, 0x00);  // adc/dac dividers
    if (es8311ReadReg(0x06, &r)) es8311WriteReg(0x06, (uint8_t)((r & 0xE0) | 0x03));
    if (es8311ReadReg(0x07, &r)) es8311WriteReg(0x07, (uint8_t)(r & 0xC0));
    es8311WriteReg(0x08, 0xFF);

    // Serial digital port: slave mode, I2S format, 16-bit in/out.
    if (es8311ReadReg(0x00, &r)) es8311WriteReg(0x00, (uint8_t)(r & ~0x40));
    es8311WriteReg(0x09, 0x0C);
    es8311WriteReg(0x0A, 0x0C);

    // Power up the analogue blocks and DAC.
    es8311WriteReg(0x0D, 0x01);
    es8311WriteReg(0x0E, 0x02);
    es8311WriteReg(0x12, 0x00);
    es8311WriteReg(0x13, 0x10);
    es8311WriteReg(0x1C, 0x6A);
    es8311WriteReg(0x37, 0x08);

    // Microphone front-end (unused for playback, kept to match the reference).
    es8311WriteReg(0x17, 0xC8);
    es8311WriteReg(0x14, 0x1A);

    // DAC output volume. Reg 0x32 is dB-scaled, NOT a percentage:
    // 0x00 = -95.5 dB, 0xBF = 0 dB, 0xFF = +32 dB (0.5 dB/step). The Waveshare
    // reference "volume 85%" → 0xD8 is +12.5 dB of *digital* gain ahead of the
    // DAC, which hard-clips every waveform (this was the "呲音" on all chimes).
    // 0 dB + a near-full-scale PCM is the loudest clean setting; loudness past
    // that has to come from the analogue amp, not digital gain.
    es8311WriteReg(0x32, 0xBF);

    Serial.println("i2s_audio: ES8311 configured for DAC");
    return true;
}

bool audioInit() {
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chanCfg.auto_clear = true;

    esp_err_t err = i2s_new_channel(&chanCfg, &s_txHandle, nullptr);
    if (err != ESP_OK || !s_txHandle) {
        Serial.printf("i2s_audio: I2S channel create failed: %s\n", esp_err_to_name(err));
        return false;
    }

    i2s_std_config_t stdCfg = {};
    stdCfg.clk_cfg.sample_rate_hz = 16000;
    stdCfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    stdCfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    stdCfg.clk_cfg.ext_clk_freq_hz = 0;

    stdCfg.slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    stdCfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    stdCfg.gpio_cfg.mclk = (gpio_num_t)PIN_I2S_MCLK;
    stdCfg.gpio_cfg.bclk = (gpio_num_t)PIN_I2S_BCLK;
    stdCfg.gpio_cfg.ws   = (gpio_num_t)PIN_I2S_WS;
    stdCfg.gpio_cfg.dout = (gpio_num_t)PIN_I2S_DO;
    stdCfg.gpio_cfg.din  = (gpio_num_t)I2S_GPIO_UNUSED;

    err = i2s_channel_init_std_mode(s_txHandle, &stdCfg);
    if (err != ESP_OK) {
        Serial.printf("i2s_audio: I2S std mode init failed: %s\n", esp_err_to_name(err));
        return false;
    }

#if BOARD_HAS_PA_CTRL
    pinMode(PIN_PA_CTRL, OUTPUT);
    digitalWrite(PIN_PA_CTRL, LOW);
#endif

    // Enable I2S before configuring the codec so MCLK/BCLK/WS are already
    // running when the ES8311 is programmed. This matches the official
    // Waveshare 07_ES8311 example (I2S bus first, then es8311_init) and the
    // codec can NACK its first I2C access when it has no clock yet.
    err = i2s_channel_enable(s_txHandle);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.printf("i2s_audio: enable failed: %s\n", esp_err_to_name(err));
        return false;
    }
    s_txEnabled = true;

    if (!initEs8311()) {
        Serial.println("i2s_audio: init FAILED (ES8311 unreachable; no sound)");
        return false;
    }

    // I2S runs continuously; PA_CTRL is toggled to mute/unmute.
    Serial.println("i2s_audio: init OK (I2S running)");
    return true;
}

bool audioTxStart() {
    if (!s_txHandle || !s_txEnabled) return false;
    if (s_paOn) return true;
#if BOARD_HAS_PA_CTRL
    digitalWrite(PIN_PA_CTRL, HIGH);
    delay(10);
#endif
    s_paOn = true;
    Serial.println("i2s_audio: PA on");
    return true;
}

void audioTxWrite(const uint8_t* data, size_t len) {
    if (!s_paOn || !s_txEnabled) return;
    size_t written = 0;
    i2s_channel_write(s_txHandle, data, len, &written, portMAX_DELAY);
}

void audioTxStop() {
    if (!s_paOn) return;
    s_paOn = false;
#if BOARD_HAS_PA_CTRL
    digitalWrite(PIN_PA_CTRL, LOW);
#endif
    Serial.println("i2s_audio: PA off");
}

void audioPauseForScan() {
    // Disable the I2S DMA channel during WiFi scan to free EDMA/PSRAM
    // bandwidth. Safe to call even if audio is idle (no audioTxWrite runs
    // during the WiFi-setup flow). s_txEnabled flag tracks "channel has
    // been provisioned", so resume can re-enable it.
    if (!s_txHandle || !s_txEnabled) return;
    esp_err_t err = i2s_channel_disable(s_txHandle);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.printf("i2s_audio: pause failed: %s\n", esp_err_to_name(err));
    }
}

void audioResumeAfterScan() {
    if (!s_txHandle || !s_txEnabled) return;
    esp_err_t err = i2s_channel_enable(s_txHandle);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.printf("i2s_audio: resume failed: %s\n", esp_err_to_name(err));
    }
}
