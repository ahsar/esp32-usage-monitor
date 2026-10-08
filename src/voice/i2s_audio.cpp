#include "i2s_audio.h"
#include "hw/pins.h"
#include <Arduino.h>
#include <driver/i2s_std.h>
#include <driver/i2s_common.h>
#include <Wire.h>

static i2s_chan_handle_t s_txHandle = nullptr;
static bool s_txEnabled = false;
static bool s_paOn = false;

static void es8311WriteReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(0x18);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

static void initEs8311() {
    Wire.beginTransmission(0x18);
    if (Wire.endTransmission() != 0) {
        Serial.println("i2s_audio: ES8311 not found on I2C");
        return;
    }

    es8311WriteReg(0x00, 0x80);
    delay(50);
    es8311WriteReg(0x00, 0x00);
    delay(50);

    es8311WriteReg(0x01, 0x3F);
    es8311WriteReg(0x02, 0x00);
    es8311WriteReg(0x03, 0x10);
    es8311WriteReg(0x04, 0x03);
    es8311WriteReg(0x05, 0x00);
    es8311WriteReg(0x06, 0x2C);
    es8311WriteReg(0x07, 0x00);
    es8311WriteReg(0x08, 0x00);
    es8311WriteReg(0x09, 0x0C);
    es8311WriteReg(0x0A, 0x00);
    es8311WriteReg(0x0B, 0x00);
    es8311WriteReg(0x0C, 0x00);
    es8311WriteReg(0x0D, 0x01);
    es8311WriteReg(0x0E, 0x02);
    es8311WriteReg(0x0F, 0x7F);
    es8311WriteReg(0x10, 0x7F);
    es8311WriteReg(0x11, 0x40);
    es8311WriteReg(0x12, 0x04);
    es8311WriteReg(0x13, 0x00);
    es8311WriteReg(0x14, 0x1A);
    es8311WriteReg(0x15, 0x40);
    es8311WriteReg(0x16, 0x00);
    es8311WriteReg(0x17, 0x18);
    es8311WriteReg(0x18, 0x00);
    es8311WriteReg(0x19, 0x32);
    es8311WriteReg(0x1A, 0x06);
    es8311WriteReg(0x1B, 0x00);
    es8311WriteReg(0x1C, 0x0F);
    es8311WriteReg(0x1D, 0x0F);

    es8311WriteReg(0x01, 0x3F);
    es8311WriteReg(0x01, 0x00);
    es8311WriteReg(0x12, 0x01);
    es8311WriteReg(0x13, 0x00);
    es8311WriteReg(0x0D, 0x01);
    es8311WriteReg(0x14, 0x1A);
    es8311WriteReg(0x14, 0x1C);
    es8311WriteReg(0x0A, 0x00);
    es8311WriteReg(0x01, 0x00);
    Serial.println("i2s_audio: ES8311 configured for DAC");
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

    pinMode(PIN_PA_CTRL, OUTPUT);
    digitalWrite(PIN_PA_CTRL, LOW);

    initEs8311();

    // Enable I2S once, keep it running. Toggle PA_CTRL for mute.
    err = i2s_channel_enable(s_txHandle);
    if (err != ESP_OK) {
        Serial.printf("i2s_audio: enable failed: %s\n", esp_err_to_name(err));
        return false;
    }
    s_txEnabled = true;

    Serial.println("i2s_audio: init OK (I2S running)");
    return true;
}

bool audioTxStart() {
    if (!s_txHandle || !s_txEnabled) return false;
    if (s_paOn) return true;
    digitalWrite(PIN_PA_CTRL, HIGH);
    delay(10);
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
    digitalWrite(PIN_PA_CTRL, LOW);
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
