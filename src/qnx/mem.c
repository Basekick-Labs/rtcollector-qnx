/* mem: physical memory.
 *
 * Total is the sum of the system page's asinfo entries named "ram" (end is
 * inclusive). Free is the size of /proc: on QNX, stat("/proc").st_size is
 * the free system memory (the same figure pidin reports as FreeMem and
 * posix_typed_mem_get_info gives for "sysram").
 *
 *   mem total_bytes=...,free_bytes=...,used_bytes=...,used_percent=...
 */
#include "../core/collector.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/syspage.h>

static uint64_t total_ram;

static int init(rtc_collector *c)
{
	(void)c;
	const struct asinfo_entry *as = SYSPAGE_ENTRY(asinfo);
	const char *strings = SYSPAGE_ENTRY(strings)->data;
	unsigned n = _syspage_ptr->asinfo.entry_size / sizeof(*as);
	total_ram = 0;
	for (unsigned i = 0; i < n; i++)
		if (!strcmp(strings + as[i].name, "ram"))
			total_ram += as[i].end - as[i].start + 1;
	return 0;
}

static int gather(rtc_collector *c)
{
	struct stat s;
	if (stat("/proc", &s) != 0 || total_ram == 0)
		return -1;
	uint64_t free_b = (uint64_t)s.st_size;
	if (free_b > total_ram)
		free_b = total_ram;
	uint64_t used = total_ram - free_b;
	rtc_rec_builder b;
	rtc_rb_init(&b, c->scratch, c->cfg.max_record_bytes, "mem");
	rtc_rb_i64(&b, "total_bytes", (int64_t)total_ram);
	rtc_rb_i64(&b, "free_bytes", (int64_t)free_b);
	rtc_rb_i64(&b, "used_bytes", (int64_t)used);
	rtc_rb_f64(&b, "used_percent", 100.0 * (double)used / (double)total_ram);
	rtc_emit(c, &b);
	return 0;
}

const rtc_input_def rtc_input_mem = {"mem", init, gather};
