
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <unistd.h>

static double timeval_diff(struct timeval start, struct timeval end)
{
	double diff;
	
	diff = (end.tv_sec - start.tv_sec)
		+ ((end.tv_usec - start.tv_usec) / 1000000.0);
	
	return diff;
}


int main(int argc, char **argv)
{
	time_t tick;
	time_t previous_tick;
	struct timespec sleep_req;
	struct timeval sleep_start, sleep_end;
	struct tm lt;
	
	time(&previous_tick);
	
	sleep_req.tv_sec = 0;
	sleep_req.tv_nsec = 210 * 1000 * 1000; /* 210 ms */
	
	while (1) {
		gettimeofday(&sleep_start, NULL);
		nanosleep(&sleep_req, NULL);
		gettimeofday(&sleep_end, NULL);
		time(&tick);
		gmtime_r(&sleep_start.tv_sec, &lt);
		
		double slept = timeval_diff(sleep_start, sleep_end);
		if (slept > 0.50) {
			printf("%4d/%02d/%02d %02d:%02d:%02d.%06d ", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec, (int)sleep_start.tv_usec);
			printf("time keeping: sleep of %ld ms took %.6f s!\n", sleep_req.tv_nsec / 1000 / 1000, slept);
		}
		
		/* catch some oddities with time keeping */
		if (tick != previous_tick) {
			if (previous_tick > tick) {
				printf("%4d/%02d/%02d %02d:%02d:%02d.%06d ", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec, (int)sleep_start.tv_usec);
				printf("time keeping: Time jumped backward by %ld seconds!\n", previous_tick - tick);
			} else if (previous_tick < tick-1) {
				printf("%4d/%02d/%02d %02d:%02d:%02d.%06d ", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec, (int)sleep_start.tv_usec);
				printf("time keeping: Time jumped forward by %ld seconds!\n", tick - previous_tick);
			}
			
			previous_tick = tick;
		}
		
	}
}
