/* Test runner: `rtc_tests` runs everything, `rtc_tests <suite>` one suite. */
#include "t.h"

#include "../src/core/log.h"

#include <string.h>

int t_failures, t_checks;

#define SUITE(s) extern const t_case s##_cases[];
SUITE(record)
SUITE(msgpack)
SUITE(ring)
SUITE(config)
SUITE(arc_encode)
SUITE(arc_http)

static const t_case *suites[] = {record_cases, msgpack_cases,    ring_cases,
                                 config_cases, arc_encode_cases, arc_http_cases};

int main(int argc, char **argv)
{
	int ran = 0;
	for (size_t s = 0; s < sizeof(suites) / sizeof(suites[0]); s++)
		for (const t_case *c = suites[s]; c->fn; c++) {
			if (argc > 1 && strcmp(argv[1], c->suite))
				continue;
			if (argc > 2 && strcmp(argv[2], c->name))
				continue;
			int before = t_failures;
			c->fn();
			fprintf(stderr, "%s %s.%s\n", t_failures == before ? "ok  " : "FAIL", c->suite,
			        c->name);
			ran++;
		}
	fprintf(stderr, "%d tests, %d checks, %d failures\n", ran, t_checks, t_failures);
	return t_failures || ran == 0 ? 1 : 0;
}
