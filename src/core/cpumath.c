#include "cpumath.h"

double rtc_cpu_usage_percent(uint64_t idle0, uint64_t idle1, uint64_t wall0, uint64_t wall1)
{
	if (wall1 <= wall0)
		return -1;
	uint64_t dwall = wall1 - wall0;
	uint64_t didle = idle1 > idle0 ? idle1 - idle0 : 0;
	if (didle > dwall)
		didle = dwall;
	return 100.0 * (double)(dwall - didle) / (double)dwall;
}
