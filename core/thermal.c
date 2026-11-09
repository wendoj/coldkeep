/* SPDX-License-Identifier: Apache-2.0
 *
 * Lumped, ILLUSTRATIVE thermal model (not engineering-grade). See docs/PLANT_MODEL.md.
 *
 *   C_air  dT_air/dt  = UA_amb(T_amb-T_air) + UA_door·door(T_amb-T_air) - Q_ea
 *   C_evap dT_evap/dt = Q_ea + Q_heater·heater - Q_comp
 *   Q_ea   = UA_ea·eff·(T_air - T_evap)       UA_ea = fans ? fan : natural
 *   Q_comp = compressor ? UA_ref·max(0, T_evap - T_sat) : 0
 *   eff    = 1 / (1 + ice/ice_ref)            (frost insulates the coil)
 *   dT_prod/dt = k_prod (T_air - T_prod)
 *
 * Frost grows while the compressor runs with the coil below 0 °C. When the coil
 * would rise above 0 °C with frost present, the surplus energy melts frost first
 * (latent sink), so a normal defrost spends its heat on ice, not on the food.
 *
 * Only + - * / are used, so results are bit-identical across platforms when built
 * with -ffp-contract=off.
 */
#include "coldkeep.h"

static const ck_thermal_params_t k_chilled = {
  .c_air = 600000.0f, .ua_amb = 150.0f, .ua_door = 150.0f,
  .ua_ea_fan = 800.0f, .ua_ea_nat = 100.0f, .c_evap = 20000.0f,
  .ua_ref = 1000.0f, .t_sat = -10.0f, .q_heater = 4500.0f,
  .k_prod = 1.0f / 2400.0f, .ice_grow = 60.0f, .ice_ref = 3.0e6f, .ice_init = 4.0e5f,
  .door_rate_h = 60.0f, .door_min_s = 5.0f, .door_max_s = 25.0f,
  .hyst = 1.0f, .setpoint = 3.0f, .limit = 8.0f, .food_kg = 400.0f,
};

static const ck_thermal_params_t k_frozen = {
  .c_air = 400000.0f, .ua_amb = 60.0f, .ua_door = 60.0f,
  .ua_ea_fan = 900.0f, .ua_ea_nat = 80.0f, .c_evap = 20000.0f,
  .ua_ref = 1200.0f, .t_sat = -32.0f, .q_heater = 4000.0f,
  .k_prod = 1.0f / 10800.0f, .ice_grow = 60.0f, .ice_ref = 3.0e6f, .ice_init = 4.0e5f,
  .door_rate_h = 30.0f, .door_min_s = 5.0f, .door_max_s = 20.0f,
  .hyst = 1.0f, .setpoint = -22.0f, .limit = -18.0f, .food_kg = 300.0f,
};

static const ck_thermal_params_t k_coldroom = {
  .c_air = 1500000.0f, .ua_amb = 100.0f, .ua_door = 400.0f,
  .ua_ea_fan = 1000.0f, .ua_ea_nat = 120.0f, .c_evap = 25000.0f,
  .ua_ref = 1200.0f, .t_sat = -8.0f, .q_heater = 5000.0f,
  .k_prod = 1.0f / 5400.0f, .ice_grow = 60.0f, .ice_ref = 3.0e6f, .ice_init = 4.0e5f,
  .door_rate_h = 10.0f, .door_min_s = 20.0f, .door_max_s = 60.0f,
  .hyst = 0.8f, .setpoint = 2.0f, .limit = 5.0f, .food_kg = 1500.0f,
};

const ck_thermal_params_t *ck_thermal_defaults(ck_case_type_t t) {
  switch (t) {
    case CASE_FROZEN: return &k_frozen;
    case CASE_COLDROOM: return &k_coldroom;
    case CASE_CHILLED:
    default: return &k_chilled;
  }
}

void ck_thermal_init(ck_case_state_t *s, uint16_t id, ck_case_type_t t, float ambient_c) {
  const ck_thermal_params_t *p = ck_thermal_defaults(t);
  *s = (ck_case_state_t){0};
  s->id = id; s->type = t;
  s->ambient_c = ambient_c;
  s->setpoint_c = p->setpoint;
  s->air_c = p->setpoint;
  s->product_c = p->setpoint;
  s->evap_c = p->setpoint - 4.0f;
  s->fans_on = 1;
  s->ice_j = p->ice_init;
}

float ck_thermal_cool_eff(const ck_case_state_t *s, const ck_thermal_params_t *p) {
  return 1.0f / (1.0f + s->ice_j / p->ice_ref);
}

void ck_thermal_step(ck_case_state_t *s, const ck_thermal_params_t *p, float dt) {
  const float eff = ck_thermal_cool_eff(s, p);
  const float ua_ea = (s->fans_on ? p->ua_ea_fan : p->ua_ea_nat) * eff;
  const float q_ea = ua_ea * (s->air_c - s->evap_c);
  float q_amb = p->ua_amb * (s->ambient_c - s->air_c);
  if (s->door_open) q_amb += p->ua_door * (s->ambient_c - s->air_c);
  float q_comp = 0.0f;
  if (s->compressor_on) {
    const float d = s->evap_c - p->t_sat;
    if (d > 0.0f) q_comp = p->ua_ref * d;
  }
  const float q_heat = s->heater_on ? p->q_heater : 0.0f;

  const float air_next = s->air_c + dt * (q_amb - q_ea) / p->c_air;
  float evap_next = s->evap_c + dt * (q_ea + q_heat - q_comp) / p->c_evap;

  /* frost: melt with surplus heat above 0 °C, grow while cooling below 0 °C */
  if (evap_next > 0.0f && s->ice_j > 0.0f) {
    const float surplus = evap_next * p->c_evap;
    const float melt = surplus < s->ice_j ? surplus : s->ice_j;
    s->ice_j -= melt;
    evap_next = (surplus - melt) / p->c_evap;
  } else if (s->compressor_on && evap_next < 0.0f) {
    s->ice_j += p->ice_grow * dt;
  }

  s->product_c += dt * p->k_prod * (s->air_c - s->product_c);
  s->air_c = air_next;
  s->evap_c = evap_next;
}
