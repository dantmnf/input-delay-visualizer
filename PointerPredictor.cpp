#include "PointerPredictor.hpp"
#include <SDL3/SDL_timer.h>
#include <cmath>

// WARNING: AI slop

SDL_FPoint PointerPredictor::predict(uint64_t timestamp) const {
  if (samples.size() < 2) {
    return samples.empty() ? SDL_FPoint{0, 0} : samples.back().first;
  }

  const auto [last_pos, last_ts] = samples.back();

  // ── 1. Pairwise velocities ──────────────────────────────────────────
  struct Vel {
    double vx, vy, sp;
    double ts;
  };
  Vel vbuf[max_samples];
  size_t vn = 0;

  for (size_t i = 1; i < samples.size(); ++i) {
    double dt = ((double)samples[i].second - samples[i - 1].second) / SDL_NS_PER_SECOND;
    if (dt < 1e-6)
      continue; // skip duplicates / sub-microsecond gaps
    double vx = (samples[i].first.x - samples[i - 1].first.x) / dt;
    double vy = (samples[i].first.y - samples[i - 1].first.y) / dt;
    vbuf[vn++] = {vx, vy, std::hypot(vx, vy), (double)samples[i].second};
  }

  if (vn == 0)
    return last_pos;

  // ── 2. Exponentially-weighted average velocity ──────────────────────
  // Half-life of ~30 ms keeps the estimate responsive while smoothing
  // single-sample jitter.
  constexpr double HALF_LIFE = 0.030;
  const double lam = std::log(2.0) / HALF_LIFE;

  double ws = 0, wvx = 0, wvy = 0;
  for (size_t i = 0; i < vn; ++i) {
    double age = ((double)last_ts - vbuf[i].ts) / SDL_NS_PER_SECOND;
    if (age < 0)
      age = 0;
    double w = std::exp(-lam * age); // age ≥ 0  →  w ∈ (0, 1]
    ws += w;
    wvx += w * vbuf[i].vx;
    wvy += w * vbuf[i].vy;
  }

  if (ws < 1e-15)
    return last_pos;

  double avg_vx = wvx / ws;
  double avg_vy = wvy / ws;

  // dt_pred: time from last sample to prediction target (for extrapolation).
  // Cursor-stopped detection is handled by the caller (skip calling
  // predict() when no motion events arrive).
  double dt_pred = ((double)timestamp - last_ts) / SDL_NS_PER_SECOND;
  if (dt_pred < 0.0)
    dt_pred = 0.0;

  // ── 4. Velocity magnitude cap ───────────────────────────────────────
  constexpr double MAX_SPEED = 8000.0;
  double sp = std::hypot(avg_vx, avg_vy);
  if (sp > MAX_SPEED) {
    double s = MAX_SPEED / sp;
    avg_vx *= s;
    avg_vy *= s;
  }

  // ── 5. Linear extrapolation (capped horizon) ────────────────────────
  constexpr double MAX_DT_PRED = 0.100; // seconds
  if (dt_pred > MAX_DT_PRED)
    dt_pred = MAX_DT_PRED;

  double pred_x = last_pos.x + avg_vx * dt_pred;
  double pred_y = last_pos.y + avg_vy * dt_pred;

  // ── 6. Displacement clamp (safety net) ──────────────────────────────
  constexpr double MAX_DISP = 800.0;
  double dx = pred_x - last_pos.x;
  double dy = pred_y - last_pos.y;
  double disp = std::hypot(dx, dy);
  if (disp > MAX_DISP) {
    double s = MAX_DISP / disp;
    pred_x = last_pos.x + dx * s;
    pred_y = last_pos.y + dy * s;
  }

  return {(float)pred_x, (float)pred_y};
}
