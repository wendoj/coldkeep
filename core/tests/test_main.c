/* SPDX-License-Identifier: Apache-2.0 */
#include "ck_test.h"

int ck_t_fail, ck_t_pass;
const char *ck_t_name = "";

void suite_prng(void);
void suite_thermal(void);
void suite_control(void);
void suite_governor(void);
void suite_broker(void);
void suite_journal(void);
void suite_metrics(void);
void suite_store(void);

int main(void) {
  struct { const char *name; void (*fn)(void); } suites[] = {
    {"prng", suite_prng},         {"thermal", suite_thermal}, {"control", suite_control},
    {"governor", suite_governor}, {"broker", suite_broker},   {"journal", suite_journal},
    {"metrics", suite_metrics},   {"store", suite_store},
  };
  for (size_t i = 0; i < sizeof suites / sizeof suites[0]; i++) {
    int before = ck_t_fail;
    suites[i].fn();
    printf("%-10s %s\n", suites[i].name, ck_t_fail == before ? "ok" : "FAILED");
  }
  printf("unit: %d checks passed, %d failed\n", ck_t_pass, ck_t_fail);
  return ck_t_fail ? 1 : 0;
}
