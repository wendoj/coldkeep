/* Minimal dependency-free test harness. SPDX-License-Identifier: Apache-2.0 */
#ifndef CK_TEST_H
#define CK_TEST_H
#include <stdio.h>
#include <string.h>

extern int ck_t_fail, ck_t_pass;
extern const char *ck_t_name;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (cond) ck_t_pass++;                                                       \
    else { ck_t_fail++;                                                          \
      fprintf(stderr, "  FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, ck_t_name, #cond); } \
  } while (0)

#define CHECK_MSG(cond, ...)                                                     \
  do {                                                                           \
    if (cond) ck_t_pass++;                                                       \
    else { ck_t_fail++;                                                          \
      fprintf(stderr, "  FAIL %s:%d [%s] %s: ", __FILE__, __LINE__, ck_t_name, #cond); \
      fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); }                      \
  } while (0)

#define CHECK_EQ_INT(a, b) CHECK_MSG((long long)(a) == (long long)(b), "%lld != %lld", (long long)(a), (long long)(b))
#define CHECK_NEAR(a, b, tol) CHECK_MSG(((a) - (b)) <= (tol) && ((b) - (a)) <= (tol), "%g vs %g", (double)(a), (double)(b))

#define RUN(fn) do { ck_t_name = #fn; fn(); } while (0)

#endif
