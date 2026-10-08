#pragma once
#include <stdint.h>
#include <stddef.h>

bool audioInit();
bool audioTxStart();
void audioTxWrite(const uint8_t* data, size_t len);
void audioTxStop();
void audioPauseForScan();   // disable I2S DMA channel (frees EDMA/PSRAM bandwidth for WiFi scan)
void audioResumeAfterScan(); // re-enable I2S DMA channel after scan
