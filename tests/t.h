/* Minimal test harness: no framework dependency. */
#ifndef RTC_T_H
#define RTC_T_H

#include <stdio.h>

extern int t_failures;
extern int t_checks;

#define CHECK(cond)                                                                                          \
	do {                                                                                                 \
		t_checks++;                                                                                  \
		if (!(cond)) {                                                                               \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);             \
			t_failures++;                                                                        \
		}                                                                                            \
	} while (0)

#define CHECK_EQ_U(a, b)                                                                                     \
	do {                                                                                                 \
		unsigned long long _a = (unsigned long long)(a), _b = (unsigned long long)(b);               \
		t_checks++;                                                                                  \
		if (_a != _b) {                                                                              \
			fprintf(stderr, "%s:%d: CHECK failed: %s == %s (%llu != %llu)\n", __FILE__,          \
			        __LINE__, #a, #b, _a, _b);                                                   \
			t_failures++;                                                                        \
		}                                                                                            \
	} while (0)

typedef struct {
	const char *suite;
	const char *name;
	void (*fn)(void);
} t_case;

#endif
