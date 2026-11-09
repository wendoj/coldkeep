/* SPDX-License-Identifier: Apache-2.0
 *
 * The plant: physics of every case, seeded shopper load and probe noise.
 * Used in-process by store.c (host) and by plantsim (target), so both run the
 * same model with the same random draws.
 */
#include "coldkeep.h"

#include <string.h>

void ck_plant_init(ck_plant_t *pl, uint64_t seed, uint16_t n, const ck_case_type_t *types,
                   float ambient_c, float dt_s) {
  memset(pl, 0, sizeof *pl);
  pl->n = n > CK_MAX_CASES ? CK_MAX_CASES : n;
  pl->dt_s = dt_s;
  pl->start_hour = 9.0f;
  for (uint16_t i = 0; i < pl->n; i++) {
    ck_thermal_init(&pl->st[i], (uint16_t)(i + 1), types[i], ambient_c);
    pl->p[i] = *ck_thermal_defaults(types[i]);
    /* streams depend on seed + case index only, never on mode */
    ck_rng_seed(&pl->door_rng[i], ck_rng_derive(seed, 100u + i));
    ck_rng_seed(&pl->noise_rng[i], ck_rng_derive(seed, 200u + i));
  }
}

uint64_t ck_plant_dt_ns(const ck_plant_t *pl) {
  return (uint64_t)(pl->dt_s * 1000.0f + 0.5f) * 1000000ULL;
}

static float door_factor(const ck_plant_t *pl, uint64_t now) {
  float h = pl->start_hour + (float)(now / CK_NS_PER_S) / 3600.0f;
  while (h >= 24.0f) h -= 24.0f;
  if (h >= 8.0f && h < 21.0f) return 1.0f;
  if (h >= 7.0f && h < 22.0f) return 0.4f;
  return 0.05f;
}

void ck_plant_sense(ck_plant_t *pl, uint64_t now) {
  const float f = door_factor(pl, now);
  for (uint16_t i = 0; i < pl->n; i++) {
    const float u = ck_rng_uniform(&pl->door_rng[i]); /* drawn every tick */
    if (pl->st[i].door_open && now >= pl->door_close_ns[i]) pl->st[i].door_open = 0;
    const float p = pl->p[i].door_rate_h / 3600.0f * f * pl->dt_s;
    if (!pl->st[i].door_open && u < p) {
      const float dur = ck_rng_range(&pl->door_rng[i], pl->p[i].door_min_s, pl->p[i].door_max_s);
      pl->st[i].door_open = 1;
      pl->door_close_ns[i] = now + (uint64_t)(dur * 1000.0f) * 1000000ULL;
    }
  }
  for (uint16_t i = 0; i < pl->n; i++) {
    const float na = 0.05f * ck_rng_gauss(&pl->noise_rng[i]);
    const float np = 0.05f * ck_rng_gauss(&pl->noise_rng[i]);
    const float ne = 0.05f * ck_rng_gauss(&pl->noise_rng[i]);
    pl->probe[i].air_c = pl->st[i].air_c + na;
    pl->probe[i].product_c = pl->st[i].product_c + np;
    pl->probe[i].evap_c = pl->st[i].evap_c + ne;
  }
}

void ck_plant_advance(ck_plant_t *pl) {
  for (uint16_t i = 0; i < pl->n; i++) ck_thermal_step(&pl->st[i], &pl->p[i], pl->dt_s);
}
