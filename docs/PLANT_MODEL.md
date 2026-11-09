# Plant model

**Illustrative, not engineering-grade.** The model exists so that the legacy and protected stores can be compared under the same physics, seed and attack. Only qualitative conclusions should be drawn from it. Code: [`core/thermal.c`](../core/thermal.c).

## Equations (per case, explicit Euler, dt = 0.1 s in sim-host, 1 s in property tests)

```
C_air  dT_air/dt  = UA_amb·(T_amb − T_air) + UA_door·door·(T_amb − T_air) − Q_ea
C_evap dT_evap/dt = Q_ea + Q_heater·heater − Q_comp
Q_ea   = UA_ea·eff·(T_air − T_evap)        UA_ea = fans ? UA_ea_fan : UA_ea_nat
Q_comp = compressor ? UA_ref·max(0, T_evap − T_sat) : 0
eff    = 1 / (1 + ice / ice_ref)           frost insulates the coil
dT_prod/dt = k_prod·(T_air − T_prod)
```

**Frost.** Frost (`ice`, stored as latent heat in J) grows at `ice_grow` while the compressor runs with the coil below 0 °C. When heat would take the coil above 0 °C with frost present, the surplus melts frost first. So a normal defrost spends its energy on ice, not on the food. Chilled coils also clear frost during compressor-off periods (as medium-temperature cases do); frozen coils do not, so skipping defrost slowly degrades cooling there.

Modelling the coil separately from the air is what lets *terminated* defrosts be gentle while *forced* defrosts are dangerous.

**Shopper load.** Seeded random door/reach-in events. The rate depends on time of day: full rate 08:00–21:00, 40 % in the shoulder hours, 5 % at night. The sim starts at 09:00 by default.

**Sensor noise.** Probes add N(0, 0.05 °C), approximated by Irwin–Hall so that no libm is needed.

## Parameters

| | Chilled multideck | Frozen well | Walk-in cold room |
|---|---|---|---|
| C_air (J/K) | 600 000 | 400 000 | 1 500 000 |
| UA_amb (W/K) | 150 | 60 | 100 |
| UA_door (W/K) | 150 | 60 | 400 |
| UA_ea fan / natural (W/K) | 800 / 100 | 900 / 80 | 1000 / 120 |
| C_evap (J/K) | 20 000 | 20 000 | 25 000 |
| UA_ref (W/K), T_sat (°C) | 1000, −10 | 1200, −32 | 1200, −8 |
| Heater (W) | 4500 | 4000 | 5000 |
| Product time constant | 40 min | 3 h | 1.5 h |
| Door events/h, duration | 60, 5–25 s | 30, 5–20 s | 10, 20–60 s |
| Thermostat half-band | 1.0 °C | 1.0 °C | 0.8 °C |
| Illustrative food mass | 400 kg | 300 kg | 1500 kg |

Ambient is 20 °C unless the scenario sets `ambient_c`.

## Calibration checks

These are encoded as unit tests in [`core/tests/test_thermal.c`](../core/tests/test_thermal.c). They are **host simulator** results, not QNX measurements.

- Steady state: air within setpoint ± (half-band + 0.5 °C), product within ±0.5 °C of setpoint, for all three types.
- Forced continuous defrost, chilled: air passes 20 °C between 30 and 60 sim-minutes. In the calibration run it crossed at about 32 min and kept rising towards ~45 °C.
- Terminated defrost at the G3 maximum: chilled product stays below 4 °C and frozen product below −21 °C.
- Frozen coil without defrost for 24 h: cooling efficiency drops below 0.6, and compressor duty rises by more than 20 %. A defrost then clears the frost.
