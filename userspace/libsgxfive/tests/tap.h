/* SPDX-License-Identifier: MIT */
/*
 * A minimal TAP producer for the libsgxfive C tests: each CHECK is one test
 * point, and tap_done() returns the exit status (1 if any point failed).
 */
#ifndef SGXFIVE_TAP_H
#define SGXFIVE_TAP_H

#include <inttypes.h>
#include <stdio.h>

static int tap_n, tap_failed;

#define CHECK(cond) tap_check(!!(cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b) tap_check_eq((uint64_t)(a), (uint64_t)(b), #a " == " #b, \
				    __FILE__, __LINE__)

static inline int tap_check(int ok, const char *what, const char *file, int line)
{
	tap_n++;
	if (!ok) {
		tap_failed++;
		printf("not ok %d - %s (%s:%d)\n", tap_n, what, file, line);
	} else {
		printf("ok %d - %s\n", tap_n, what);
	}
	return ok;
}

static inline int tap_check_eq(uint64_t a, uint64_t b, const char *what, const char *file,
			       int line)
{
	int ok = tap_check(a == b, what, file, line);

	if (!ok)
		printf("#   got 0x%" PRIx64 ", expected 0x%" PRIx64 "\n", a, b);
	return ok;
}

static inline int tap_done(void)
{
	printf("1..%d\n", tap_n);
	return tap_failed ? 1 : 0;
}

#endif
