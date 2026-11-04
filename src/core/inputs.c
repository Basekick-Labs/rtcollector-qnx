/* The inputs this build has. Config validation and the collector both read
 * this one table, so a name is either available everywhere or nowhere. */
#include "collector.h"

#include <string.h>

static const rtc_input_def *const all_inputs[] = {
    &rtc_input_internal,
#ifdef RTC_QNX
    &rtc_input_cpu,      &rtc_input_mem, &rtc_input_processes, &rtc_input_rmgr,
#endif
};

const rtc_input_def *rtc_input_find(const char *name)
{
	for (size_t i = 0; i < sizeof(all_inputs) / sizeof(all_inputs[0]); i++)
		if (!strcmp(all_inputs[i]->name, name))
			return all_inputs[i];
	return NULL;
}
