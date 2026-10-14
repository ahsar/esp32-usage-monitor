#pragma once

bool hwImuInit();
void hwImuAccel(float* ax, float* ay, float* az);
// Angular rate in degrees/second. Returns false when the gyro is unavailable.
bool hwImuGyro(float* gx, float* gy, float* gz);
