/* -*- Mode: C; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#include "rrutil.h"

int main(void) {
  cpu_set_t* cpus;
  cpu_set_t* cpus_out;

  ALLOCATE_GUARD(cpus, 'x');
  CPU_ZERO(cpus);
  CPU_SET(0, cpus);
  test_assert(0 == sched_setaffinity(0, sizeof(*cpus), cpus));
  VERIFY_GUARD(cpus);

  ALLOCATE_GUARD(cpus_out, 'x');
  test_assert(0 == sched_getaffinity(0, sizeof(*cpus_out), cpus_out));
  /* We can't assert this because rr assigns us random affinity itself.
  test_assert(0 == memcmp(cpus, cpus_out, sizeof(*cpus))); */
  VERIFY_GUARD(cpus_out);

  atomic_puts("EXIT-SUCCESS");
  return 0;
}
