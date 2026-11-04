/* CPU hog with a duty cycle: busy for B ms, sleep S ms, for T seconds.
 *   rtc_hog -b 20 -s 5 -t 10
 * The sleeps let lower-priority threads run (so a low-priority writer really
 * writes while the hog competes with its handler). */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv)
{
	pthread_setname_np(pthread_self(), "hog"); /* the trace validator looks for it */
	long busy = 20, sleep_ms = 5, secs = 10;
	for (int i = 1; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "-b"))
			busy = atol(argv[i + 1]);
		else if (!strcmp(argv[i], "-s"))
			sleep_ms = atol(argv[i + 1]);
		else if (!strcmp(argv[i], "-t"))
			secs = atol(argv[i + 1]);
	}
	uint64_t end = now_ns() + (uint64_t)secs * 1000000000u;
	volatile uint64_t x = 0;
	while (now_ns() < end) {
		uint64_t until = now_ns() + (uint64_t)busy * 1000000u;
		while (now_ns() < until)
			x++;
		usleep((useconds_t)(sleep_ms * 1000));
	}
	return (int)(x & 0);
}
