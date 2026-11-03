#include "t.h"

#include "../src/core/cpumath.h"

static void usage(void)
{
	CHECK(rtc_cpu_usage_percent(0, 0, 0, 1000) == 100.0);  /* never idle */
	CHECK(rtc_cpu_usage_percent(0, 1000, 0, 1000) == 0.0); /* always idle */
	CHECK(rtc_cpu_usage_percent(100, 350, 1000, 2000) == 75.0);
}

static void edges(void)
{
	CHECK(rtc_cpu_usage_percent(0, 10, 5, 5) == -1);          /* empty interval */
	CHECK(rtc_cpu_usage_percent(0, 10, 9, 5) == -1);          /* clock went back */
	CHECK(rtc_cpu_usage_percent(500, 100, 0, 1000) == 100.0); /* idle went back */
	CHECK(rtc_cpu_usage_percent(0, 1100, 0, 1000) == 0.0);    /* skew: idle > wall */
}

const t_case cpumath_cases[] = {
    {"cpumath", "usage", usage},
    {"cpumath", "edges", edges},
    {0},
};
