/* SPDX-License-Identifier: Apache-2.0 */
#include "ck_test.h"
#include "coldkeep.h"

static void same_seed_same_stream(void) {
  ck_rng_t a, b;
  ck_rng_seed(&a, 7); ck_rng_seed(&b, 7);
  int same = 1;
  for (int i = 0; i < 1000; i++) same &= ck_rng_next(&a) == ck_rng_next(&b);
  CHECK(same);
  ck_rng_seed(&b, 8);
  ck_rng_seed(&a, 7);
  CHECK(ck_rng_next(&a) != ck_rng_next(&b));
}

static void known_value(void) {
  /* splitmix64 reference: seed 0 -> first output 0xE220A8397B1DCDAF */
  ck_rng_t r; ck_rng_seed(&r, 0);
  CHECK(ck_rng_next(&r) == 0xE220A8397B1DCDAFULL);
}

static void ranges(void) {
  ck_rng_t r; ck_rng_seed(&r, 42);
  int ok = 1; double sum = 0, sq = 0;
  for (int i = 0; i < 100000; i++) {
    float u = ck_rng_uniform(&r); ok &= u >= 0.0f && u < 1.0f;
    uint32_t k = ck_rng_below(&r, 7); ok &= k < 7;
    float g = ck_rng_gauss(&r); sum += g; sq += (double)g * g;
  }
  CHECK(ok);
  CHECK_NEAR(sum / 100000.0, 0.0, 0.02);
  CHECK_NEAR(sq / 100000.0, 1.0, 0.03);
  CHECK(ck_rng_derive(7, 1) != ck_rng_derive(7, 2));
  CHECK(ck_rng_derive(7, 1) == ck_rng_derive(7, 1));
}

void suite_prng(void) {
  RUN(same_seed_same_stream);
  RUN(known_value);
  RUN(ranges);
}
