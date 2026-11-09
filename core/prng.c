/* SPDX-License-Identifier: Apache-2.0
 * Seeded PRNG (splitmix64). Uses only integer ops and exact float scaling so the
 * stream is identical on every platform. */
#include "coldkeep.h"

void ck_rng_seed(ck_rng_t *r, uint64_t seed) { r->s = seed; }

uint64_t ck_rng_next(ck_rng_t *r) {
  uint64_t z = (r->s += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

float ck_rng_uniform(ck_rng_t *r) {
  /* top 24 bits -> exactly representable float in [0,1) */
  return (float)(ck_rng_next(r) >> 40) * (1.0f / 16777216.0f);
}

float ck_rng_range(ck_rng_t *r, float lo, float hi) {
  return lo + (hi - lo) * ck_rng_uniform(r);
}

uint32_t ck_rng_below(ck_rng_t *r, uint32_t n) {
  if (n == 0) return 0;
  /* Lemire's multiply-shift; bias is negligible for our n and deterministic anyway */
  return (uint32_t)(((ck_rng_next(r) >> 32) * (uint64_t)n) >> 32);
}

float ck_rng_gauss(ck_rng_t *r) {
  /* Irwin–Hall: sum of 12 uniforms minus 6. Avoids libm (log/cos differ per platform). */
  float s = 0.0f;
  for (int i = 0; i < 12; i++) s += ck_rng_uniform(r);
  return s - 6.0f;
}

uint64_t ck_rng_derive(uint64_t seed, uint64_t stream) {
  ck_rng_t r; ck_rng_seed(&r, seed ^ (stream * 0xD1B54A32D192ED03ULL));
  ck_rng_next(&r);
  return ck_rng_next(&r);
}
