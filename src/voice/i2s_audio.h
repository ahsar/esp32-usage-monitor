#pragma once
#include <stdint.h>
#include <stddef.h>

bool audioInit();
bool audioTxStart();
// Queue `len` bytes for playback, blocking until the DMA has room for them
// (that is what paces a stream at the sample rate). False = nothing was queued
// because the codec is down or a scan took the channel away; the caller should
// stop and mute.
bool audioTxWrite(const uint8_t* data, size_t len);
void audioTxStop();
void audioPauseForScan();   // disable I2S DMA channel (frees EDMA/PSRAM bandwidth for WiFi scan)
void audioResumeAfterScan(); // re-enable I2S DMA channel after scan
