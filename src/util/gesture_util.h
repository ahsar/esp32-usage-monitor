#pragma once
#include <stdint.h>
#include <math.h>

// Pure, hardware-free gesture classifier for the QMI8658 IMU.
//
// Header-only so the host tests can drive it with synthetic samples (see
// test/test_util). gestures.cpp feeds it real samples at ~33 Hz and turns the
// returned event flags into pager/touch/audio actions.
//
// All thresholds are in the IMU's own units: acceleration in g with the screen
// facing up giving az ≈ +1, angular rate in degrees/second.
namespace gesture {

// Event bits returned by Engine::feed().
enum : uint8_t {
  EV_NONE       = 0,
  EV_SHAKE      = 1u << 0,
  EV_DOUBLE_TAP = 1u << 1,
  EV_PICKUP     = 1u << 2,
  EV_FACE_DOWN  = 1u << 3,
  EV_FACE_UP    = 1u << 4,
  EV_ROT_CW     = 1u << 5,
  EV_ROT_CCW    = 1u << 6,
};

struct Sample {
  float    ax, ay, az;   // g
  float    gx, gy, gz;   // deg/s (0 when the gyro is unavailable)
  uint32_t t;            // ms, monotonically increasing
};

struct Params {
  // Shake: consecutive samples with |a|² above `shakeMag2` (≈ 2 g) start a
  // shake; the whole run must fit in `shakeWindow`.
  float    shakeMag2   = 4.0f;
  uint8_t  shakeCount  = 3;
  uint32_t shakeWindow = 700;

  // Double tap: a jolt (|a|² above `tapMag2`) whose rising edges are
  // `tapMinGap`..`tapMaxGap` apart.
  float    tapMag2   = 1.6f;
  uint32_t tapMinGap = 80;
  uint32_t tapMaxGap = 450;

  // Pickup: after `restMs` of near-stillness, an orientation change larger
  // than acos(pickupCos) (≈ 30° for the default) fires once.
  uint32_t restMs    = 1500;
  float    pickupCos = 0.87f;
  float    quietTol2 = 0.12f;   // |a|² must be within this of 1.0 to count as rest
  float    restAlpha = 0.02f;   // rest-reference tracking rate (slow on purpose)

  // Orientation from the low-passed gravity vector, with hysteresis.
  float faceDownZ = -0.70f;
  float faceUpZ   =  0.55f;

  // Rotate: the integral of gz over `rotWindow` exceeding `rotThresh` degrees.
  float    rotThresh = 70.0f;
  uint32_t rotWindow = 700;
};

class Engine {
 public:
  Engine() = default;
  explicit Engine(const Params& p) : p_(p) {}

  void setParams(const Params& p) { p_ = p; }
  void reset() { *this = Engine(p_); }

  uint8_t feed(const Sample& s);

  // True while the device is resting face-down (used to blank the panel).
  bool faceDown() const { return down_; }

 private:
  Params   p_{};
  bool     started_ = false;
  uint32_t lastT_   = 0;
  float    lastMag2_ = 0.0f;

  // Low-passed gravity vector (≈ 4 Hz at a 33 Hz input rate).
  float gx_ = 0, gy_ = 0, gz_ = 1;

  // Shake run.
  uint8_t  shakeN_     = 0;
  uint32_t shakeFirst_ = 0;

  // Double tap.
  bool     tapPending_ = false;
  uint32_t tapLast_    = 0;

  // Rest / pickup.
  bool     resting_   = false;
  uint32_t restSince_ = 0;
  float    restX_ = 0, restY_ = 0, restZ_ = 1;

  // Orientation.
  bool down_ = false;

  // Rotate.
  float    rotAccum_ = 0.0f;
  uint32_t rotSince_ = 0;
};

inline uint8_t Engine::feed(const Sample& s) {
  const float mag2 = s.ax * s.ax + s.ay * s.ay + s.az * s.az;

  if (!started_) {
    started_ = true;
    gx_ = s.ax; gy_ = s.ay; gz_ = s.az;
    restX_ = gx_; restY_ = gy_; restZ_ = gz_;
    restSince_ = s.t;
    lastT_ = s.t;
    lastMag2_ = mag2;
    down_ = gz_ < p_.faceDownZ;
    return EV_NONE;
  }

  uint32_t dt = (uint32_t)(s.t - lastT_);
  lastT_ = s.t;
  uint8_t ev = EV_NONE;

  // ---- low-passed gravity ----
  const float a = 0.25f;
  gx_ += a * (s.ax - gx_);
  gy_ += a * (s.ay - gy_);
  gz_ += a * (s.az - gz_);

  // ---- shake: consecutive high-magnitude samples ----
  if (mag2 > p_.shakeMag2) {
    if (shakeN_ == 0) shakeFirst_ = s.t;
    shakeN_++;
    if (shakeN_ >= p_.shakeCount && (uint32_t)(s.t - shakeFirst_) <= p_.shakeWindow) {
      ev |= EV_SHAKE;
      shakeN_ = 0;
      tapPending_ = false;   // a shake is not a double tap
    }
  } else {
    shakeN_ = 0;
  }

  // ---- double tap: rising edge of a jolt ----
  if ((ev & EV_SHAKE) == 0 && mag2 > p_.tapMag2 && lastMag2_ <= p_.tapMag2) {
    if (tapPending_ && (uint32_t)(s.t - tapLast_) >= p_.tapMinGap &&
        (uint32_t)(s.t - tapLast_) <= p_.tapMaxGap) {
      ev |= EV_DOUBLE_TAP;
      tapPending_ = false;
    } else {
      tapPending_ = true;
      tapLast_ = s.t;
    }
  }
  if (tapPending_ && (uint32_t)(s.t - tapLast_) > p_.tapMaxGap) tapPending_ = false;
  lastMag2_ = mag2;

  // ---- orientation ----
  if (gz_ < p_.faceDownZ && !down_) {
    down_ = true;
    ev |= EV_FACE_DOWN;
  } else if (gz_ > p_.faceUpZ && down_) {
    down_ = false;
    ev |= EV_FACE_UP;
  }

  // ---- rest tracking / pickup ----
  // The rest reference is tracked very slowly (restAlpha) so a deliberate lift
  // is detected, while the reference still follows a slow desk drift. Note the
  // pickup test is deliberately *outside* the quiet branch: a smooth lift keeps
  // |a| ≈ 1 g, so it never leaves "quiet".
  bool quiet = fabsf(mag2 - 1.0f) < p_.quietTol2;
  if (quiet) {
    if (!resting_) {
      resting_ = true;
      restSince_ = s.t;
      restX_ = gx_; restY_ = gy_; restZ_ = gz_;
    } else {
      restX_ += p_.restAlpha * (gx_ - restX_);
      restY_ += p_.restAlpha * (gy_ - restY_);
      restZ_ += p_.restAlpha * (gz_ - restZ_);
    }
  } else {
    resting_ = false;
  }

  if (resting_ && (ev & (EV_FACE_DOWN | EV_FACE_UP)) == 0 &&
      (uint32_t)(s.t - restSince_) >= p_.restMs) {
    float dot = restX_ * gx_ + restY_ * gy_ + restZ_ * gz_;
    float n1 = sqrtf(restX_ * restX_ + restY_ * restY_ + restZ_ * restZ_);
    float n2 = sqrtf(gx_ * gx_ + gy_ * gy_ + gz_ * gz_);
    if (n1 > 0.1f && n2 > 0.1f && (dot / (n1 * n2)) < p_.pickupCos) {
      ev |= EV_PICKUP;
      resting_ = false;   // needs another rest period before it can fire again
    }
  }

  // ---- gyro rotate ----
  if (s.gz != 0.0f && dt > 0) {
    if (rotSince_ == 0 || (uint32_t)(s.t - rotSince_) > p_.rotWindow) {
      rotSince_ = s.t;
      rotAccum_ = 0.0f;
    }
    rotAccum_ += s.gz * ((float)dt / 1000.0f);
    if (rotAccum_ > p_.rotThresh) {
      ev |= EV_ROT_CW;
      rotAccum_ = 0.0f; rotSince_ = s.t;
    } else if (rotAccum_ < -p_.rotThresh) {
      ev |= EV_ROT_CCW;
      rotAccum_ = 0.0f; rotSince_ = s.t;
    }
  }

  return ev;
}

}  // namespace gesture
