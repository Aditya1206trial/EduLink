// SPDX-License-Identifier: GPL-2.0
/*
 * Phase 3 user-space test for the EduLink driver.
 *
 * Usage: test_edu [-n count] [device]   (defaults: 2000, /dev/edu0)
 * Runs the functional checks (liveness, factorial, DMA), then two stress
 * runs of `count` factorials each: one plain, one with a periodic signal
 * that interrupts waits. Last, DMA_STRESS round trips with signals.
 * Exit status: 0 if every check passes, 1 otherwise.
 */
#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include "edu_ioctl.h"

static int failures;

static void check(int ok, const char *what)
{
	printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		failures++;
}

static void test_liveness(int fd)
{
	static const __u32 patterns[] = { 0x00000000, 0xffffffff, 0x12345678 };
	size_t i;

	for (i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
		__u32 v = patterns[i];
		char msg[96];

		if (ioctl(fd, EDU_IOC_LIVENESS, &v) < 0) {
			snprintf(msg, sizeof(msg), "liveness 0x%08x: ioctl: %s",
				 patterns[i], strerror(errno));
			check(0, msg);
			continue;
		}
		snprintf(msg, sizeof(msg), "liveness 0x%08x -> 0x%08x",
			 patterns[i], v);
		check(v == (__u32)~patterns[i], msg);
	}
}

static void test_factorial(int fd)
{
	/* 0 and 12 are the bounds of the accepted range. */
	static const struct { __u32 n, want; } cases[] = {
		{ 0, 1 }, { 10, 3628800u }, { EDU_FACTORIAL_MAX_INPUT, 479001600u },
	};
	char msg[96];
	size_t i;
	__u32 v;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		v = cases[i].n;
		if (ioctl(fd, EDU_IOC_FACTORIAL, &v) < 0) {
			snprintf(msg, sizeof(msg), "factorial %u: ioctl: %s",
				 cases[i].n, strerror(errno));
			check(0, msg);
			continue;
		}
		snprintf(msg, sizeof(msg), "factorial %u -> %u (want %u)",
			 cases[i].n, v, cases[i].want);
		check(v == cases[i].want, msg);
	}

	v = EDU_FACTORIAL_MAX_INPUT + 1;
	check(ioctl(fd, EDU_IOC_FACTORIAL, &v) < 0 && errno == ERANGE,
	      "factorial 13 rejected with ERANGE");
}

static void test_bad_ioctl(int fd)
{
	__u32 v = 0;

	check(ioctl(fd, _IOWR(EDU_IOC_MAGIC, 0x7f, __u32), &v) < 0 &&
	      errno == ENOTTY, "unknown ioctl rejected with ENOTTY");
	check(ioctl(fd, EDU_IOC_LIVENESS, (void *)8) < 0 && errno == EFAULT,
	      "bad user pointer rejected with EFAULT");
}

/*
 * Bytes 0..len-1 vary with seed and are never 0: the driver zeroes its
 * buffer before the read-back leg, so a byte the device did not write
 * fails. The rest is a sentinel the driver must keep.
 */
static void dma_fill(struct edu_dma *d, __u32 len, unsigned long seed)
{
	__u32 i;

	d->len = len;
	for (i = 0; i < EDU_DMA_MAX_LEN; i++) {
		__u8 b = (__u8)(seed * 131 + i * 7 + (i >> 8));

		d->data[i] = i >= len ? 0xa5 : b ? b : 0x5a;
	}
}

static void test_dma(int fd)
{
	/* 1 and 4096 are the bounds; the rest straddle byte/page-ish edges. */
	static const __u32 lens[] = { 1, 2, 255, 256, 257, 1000, 4095,
				      EDU_DMA_MAX_LEN };
	static const __u32 bad_lens[] = { 0, EDU_DMA_MAX_LEN + 1, 0xffffffff };
	static struct edu_dma d, want;
	char msg[96];
	size_t i;

	for (i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
		dma_fill(&d, lens[i], i);
		want = d;
		if (ioctl(fd, EDU_IOC_DMA, &d) < 0) {
			snprintf(msg, sizeof(msg), "dma len %u: ioctl: %s",
				 lens[i], strerror(errno));
			check(0, msg);
			continue;
		}
		snprintf(msg, sizeof(msg), "dma len %u round trip", lens[i]);
		check(!memcmp(&d, &want, sizeof(d)), msg);
	}

	for (i = 0; i < sizeof(bad_lens) / sizeof(bad_lens[0]); i++) {
		dma_fill(&d, 1, 0);
		d.len = bad_lens[i];
		snprintf(msg, sizeof(msg), "dma len %u rejected with EINVAL",
			 bad_lens[i]);
		check(ioctl(fd, EDU_IOC_DMA, &d) < 0 && errno == EINVAL, msg);
	}

	check(ioctl(fd, EDU_IOC_DMA, (void *)8) < 0 && errno == EFAULT,
	      "dma bad user pointer rejected with EFAULT");
}

static __u32 fact(__u32 n)
{
	__u32 r = 1;

	while (n > 1)
		r *= n--;
	return r;
}

static void on_alarm(int sig)
{
	(void)sig;
}

/*
 * SIGALRM without SA_RESTART, so interrupted waits fail EINTR. First after
 * `first` us, then every `every` us (0: once). first == 0 arms nothing.
 */
static int alarm_on(long first, long every)
{
	struct itimerval it = { { 0, every }, { first / 1000000, first % 1000000 } };
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_alarm;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGALRM, &sa, NULL))
		return -1;
	return setitimer(ITIMER_REAL, &it, NULL);
}

static void alarm_off(void)
{
	struct itimerval it;

	memset(&it, 0, sizeof(it));
	setitimer(ITIMER_REAL, &it, NULL);
	signal(SIGALRM, SIG_IGN);
}

/*
 * `count` factorials cycling n through 0..12, each checked. With signals,
 * an ioctl failing EINTR is retried; the abandoned factorial's late IRQ
 * then lands on the retry, which must still return its own result.
 */
static void stress(int fd, unsigned long count, int signals)
{
	unsigned long done = 0, eintr = 0;
	int bad = 0;
	char msg[160];

	if (signals && alarm_on(200, 200)) {
		snprintf(msg, sizeof(msg), "stress setup: %s", strerror(errno));
		check(0, msg);
		return;
	}

	while (done < count && !bad) {
		__u32 n = done % (EDU_FACTORIAL_MAX_INPUT + 1), v = n;

		if (ioctl(fd, EDU_IOC_FACTORIAL, &v) < 0) {
			if (signals && errno == EINTR) {
				/* Livelock guard if every attempt is interrupted. */
				bad = ++eintr > 20 * count;
				continue;
			}
			snprintf(msg, sizeof(msg), "stress #%lu factorial %u: %s",
				 done, n, strerror(errno));
			check(0, msg);
			bad = 1;
		} else if (v != fact(n)) {
			snprintf(msg, sizeof(msg),
				 "stress #%lu factorial %u -> %u (want %u)",
				 done, n, v, fact(n));
			check(0, msg);
			bad = 1;
		} else {
			done++;
		}
	}

	if (signals)
		alarm_off();

	snprintf(msg, sizeof(msg), "stress%s: %lu/%lu factorials correct, %lu EINTR",
		 signals ? " with signals" : "", done, count, eintr);
	check(!bad && done == count, msg);
	if (signals && !bad && !eintr)
		printf("NOTE: no wait was interrupted; late-IRQ path not exercised\n");
}

/* Each round trip takes at least 200 ms of QEMU virtual time. */
#define DMA_STRESS 50

/*
 * DMA_STRESS round trips with varied lengths, each checked. Each attempt
 * arms a one-shot SIGALRM at a different point (before, in leg 1, in leg
 * 2, after, or none), so retries start right after abandoned transfers and
 * can get their late IRQ. Every fifth attempt has no signal: no livelock.
 */
static void stress_dma(int fd)
{
	static const long delay_us[] = { 0, 5000, 50000, 150000, 250000 };
	static struct edu_dma d, want;
	unsigned long done = 0, eintr = 0, attempt = 0;
	int bad = 0, ret, err;
	char msg[160];

	while (done < DMA_STRESS && !bad) {
		__u32 len = 1 + (done * 509) % EDU_DMA_MAX_LEN;

		dma_fill(&d, len, done);
		want = d;
		if (alarm_on(delay_us[attempt++ % 5], 0)) {
			snprintf(msg, sizeof(msg), "dma stress setup: %s",
				 strerror(errno));
			check(0, msg);
			return;
		}
		ret = ioctl(fd, EDU_IOC_DMA, &d);
		err = errno; /* before alarm_off() can change it */
		alarm_off();
		if (ret < 0 && err == EINTR) {
			eintr++;
		} else if (ret < 0) {
			snprintf(msg, sizeof(msg), "dma stress #%lu len %u: %s",
				 done, len, strerror(err));
			check(0, msg);
			bad = 1;
		} else if (memcmp(&d, &want, sizeof(d))) {
			snprintf(msg, sizeof(msg),
				 "dma stress #%lu len %u: data mismatch", done, len);
			check(0, msg);
			bad = 1;
		} else {
			done++;
		}
	}

	snprintf(msg, sizeof(msg),
		 "dma stress with signals: %lu/%d round trips correct, %lu EINTR",
		 done, DMA_STRESS, eintr);
	check(!bad && done == DMA_STRESS, msg);
	if (!bad && !eintr)
		printf("NOTE: no DMA wait was interrupted; late-IRQ path not exercised\n");
}

int main(int argc, char **argv)
{
	const char *path = "/dev/edu0";
	unsigned long count = 2000;
	char *end;
	int fd, opt;

	while ((opt = getopt(argc, argv, "n:")) != -1) {
		if (opt != 'n')
			goto usage;
		errno = 0;
		count = strtoul(optarg, &end, 10);
		if (errno || end == optarg || *end || !count ||
		    count > 100000000ul)
			goto usage;
	}
	if (argc - optind > 1)
		goto usage;
	if (optind < argc)
		path = argv[optind];

	fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return 1;
	}

	test_liveness(fd);
	test_factorial(fd);
	test_bad_ioctl(fd);
	test_dma(fd);
	stress(fd, count, 0);
	stress(fd, count, 1);
	stress_dma(fd);

	close(fd);
	printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
	return failures ? 1 : 0;

usage:
	fprintf(stderr, "usage: %s [-n count] [device]\n", argv[0]);
	return 1;
}
