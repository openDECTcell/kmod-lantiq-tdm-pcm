// SPDX-License-Identifier: GPL-2.0-only
/*
 * tdm-pcm-test - step the Lantiq VR9 TDM/PCM transport up and look at the
 * bus. Speaks kmod-lantiq-ssc-pcm's uapi v3, so it also runs against
 * /dev/ssc-pcm0 (dev=PATH).
 *
 * Usage: tdm-pcm-test [option...]
 *   dev=PATH      device node (default /dev/tdm-pcm0)
 *   slots=N       16-bit slots per frame (default: the driver's, 8)
 *   fpb=N         frames per block
 *   ndesc=N       blocks in the receive ring
 *   step=N        stop after step N (1 clock, 2 pins, 3 framing); no rx
 *   regs          dump the registers after each step
 *   idle=00|ff    idle frame byte (only 0x00 and 0xFF are ever sent)
 *   idle=PATTERN  idle frame, one character per byte: 'f' 0xFF, '0' 0x00
 *                 (e.g. idle=f0000000000000000 marks byte 0); the rest of
 *                 the frame is 0xFF
 *   rx=SECS       run the engine, read blocks for SECS seconds, then print
 *                 which bytes of the frame change and the frames seen most
 *   dump=N        print the first N frames received (hex)
 *   tx=N          queue N test frames (alternately all 0xFF and all 0x00)
 *   txalt         keep the transmit queue filled with such frames
 *                 (pattern: P frames 0xFF, P frames 0x00; txalt=P, default 1)
 *   save=FILE     append every block (header + payload) to FILE
 *   stats=SECS    print the counters every SECS seconds while running
 *   wait=SECS     after the steps, keep the device open SECS seconds
 *                 (for looking at debugfs from another shell)
 * Exits nonzero if a step or the read fails, or if any block was lost.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "ssc_pcm.h"

#define MAX_FB		SSC_PCM_MAX_FRAME_BYTES
#define TOP_N		8
#define TABLE_SIZE	4096

static int fd;

static const char *reg_names[8] = {
	"CFG", "STAT", "RX_STAT", "TX_STAT", "CH_EN", "CH_MODE", "FREQ",
	"RX_IEN"
};

static void dump_regs(void)
{
	struct ssc_pcm_regs r;
	unsigned int i;

	memset(&r, 0, sizeof(r));
	if (ioctl(fd, SSC_PCM_IOC_REGS, &r) < 0) {
		perror("REGS");
		return;
	}
	printf("regs (step %u, valid %u):", r.step, r.valid);
	for (i = 0; r.valid && i < 8; i++)
		printf(" %s=0x%08x", reg_names[i], r.reg[i]);
	printf("\n");
}

static int do_step(unsigned int n, int regs)
{
	int ret = ioctl(fd, SSC_PCM_IOC_STEP, &n);

	printf("step %u: %s\n", n, ret ? strerror(errno) : "ok");
	if (!ret && regs)
		dump_regs();
	return ret;
}

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void print_stats(const char *tag)
{
	struct ssc_pcm_stats st;

	memset(&st, 0, sizeof(st));
	if (ioctl(fd, SSC_PCM_IOC_STATS, &st) < 0) {
		perror("STATS");
		return;
	}
	printf("%s: frames=%llu blocks=%llu dropped=%llu overruns=%u tx_frames=%llu tx_idle=%llu tx_queued=%u tx_underruns=%u rx_irqs=%u polls=%u\n",
	       tag, (unsigned long long)st.frames,
	       (unsigned long long)st.blocks, (unsigned long long)st.dropped,
	       st.overruns, (unsigned long long)st.tx_frames,
	       (unsigned long long)st.tx_idle, st.tx_queued, st.tx_underruns,
	       st.rx_irqs, st.polls);
	fflush(stdout);
}

/* distinct frames, counted (open addressing on a simple hash) */
struct entry {
	unsigned char f[MAX_FB];
	unsigned long n;
	int used;
};
static struct entry table[TABLE_SIZE];
static unsigned long table_full;

static void count_frame(const unsigned char *f, unsigned int fb)
{
	unsigned int h = 2166136261u, i, k;

	for (i = 0; i < fb; i++)
		h = (h ^ f[i]) * 16777619u;
	for (k = 0; k < TABLE_SIZE; k++) {
		struct entry *e = &table[(h + k) % TABLE_SIZE];

		if (!e->used) {
			e->used = 1;
			memcpy(e->f, f, fb);
			e->n = 1;
			return;
		}
		if (!memcmp(e->f, f, fb)) {
			e->n++;
			return;
		}
	}
	table_full++;
}

static void print_frame(const unsigned char *f, unsigned int fb)
{
	unsigned int i;

	for (i = 0; i < fb; i++)
		printf("%s%02x", i && !(i % 2) ? " " : "", f[i]);
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/tdm-pcm0", *save = NULL, *idle_pat = NULL;
	unsigned int slots = 0, fpb = 0, ndesc = 0, last = SSC_PCM_STEP_DMA;
	unsigned int rx_secs = 0, dump = 0, tx_n = 0, stats_secs = 0, wait = 0;
	int regs = 0, idle = -1, rc = 0, i, txalt = 0;
	struct ssc_pcm_info info;
	struct ssc_pcm_config cfg;
	unsigned int s;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (!strncmp(a, "dev=", 4))
			dev = a + 4;
		else if (!strncmp(a, "slots=", 6))
			slots = strtoul(a + 6, NULL, 0);
		else if (!strncmp(a, "fpb=", 4))
			fpb = strtoul(a + 4, NULL, 0);
		else if (!strncmp(a, "ndesc=", 6))
			ndesc = strtoul(a + 6, NULL, 0);
		else if (!strncmp(a, "step=", 5))
			last = strtoul(a + 5, NULL, 0);
		else if (!strcmp(a, "regs"))
			regs = 1;
		else if (!strcmp(a, "idle=00"))
			idle = 0x00;
		else if (!strcmp(a, "idle=ff"))
			idle = 0xff;
		else if (!strncmp(a, "idle=", 5)) {
			const char *q = a + 5;

			if (strspn(q, "f0") != strlen(q) ||
			    strlen(q) > SSC_PCM_MAX_FRAME_BYTES) {
				fprintf(stderr, "idle pattern: only f and 0\n");
				return 1;
			}
			idle_pat = q;
			idle = 0x100;
		}
		else if (!strncmp(a, "rx=", 3))
			rx_secs = strtoul(a + 3, NULL, 0);
		else if (!strncmp(a, "dump=", 5))
			dump = strtoul(a + 5, NULL, 0);
		else if (!strncmp(a, "tx=", 3))
			tx_n = strtoul(a + 3, NULL, 0);
		else if (!strncmp(a, "save=", 5))
			save = a + 5;
		else if (!strcmp(a, "txalt"))
			txalt = 1;
		else if (!strncmp(a, "txalt=", 6))
			txalt = strtoul(a + 6, NULL, 0);
		else if (!strncmp(a, "wait=", 5))
			wait = strtoul(a + 5, NULL, 0);
		else if (!strncmp(a, "stats=", 6))
			stats_secs = strtoul(a + 6, NULL, 0);
		else {
			fprintf(stderr, "unknown option %s\n", a);
			return 1;
		}
	}
	if (last > SSC_PCM_STEP_DMA)
		last = SSC_PCM_STEP_DMA;

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		perror(dev);
		return 1;
	}
	memset(&info, 0, sizeof(info));
	if (ioctl(fd, SSC_PCM_IOC_INFO, &info) == 0)
		printf("info: v%u name=%s step=%u slots=%u frame_bytes=%u\n",
		       info.version, info.name, info.step, info.slots,
		       info.frame_bytes);
	else
		perror("INFO");

	memset(&cfg, 0, sizeof(cfg));
	cfg.slots = slots;
	cfg.frames_per_block = fpb;
	cfg.n_desc = ndesc;
	if (ioctl(fd, SSC_PCM_IOC_CONFIG, &cfg) < 0) {
		perror("CONFIG");
		return 1;
	}
	printf("config: slots=%u frames_per_block=%u n_desc=%u\n",
	       cfg.slots, cfg.frames_per_block, cfg.n_desc);

	if (idle >= 0) {
		struct ssc_pcm_tx_idle ti;

		memset(ti.frame, idle & 0xff, sizeof(ti.frame));
		if (idle_pat) {
			memset(ti.frame, 0xff, sizeof(ti.frame));
			for (s = 0; idle_pat[s]; s++)
				ti.frame[s] = idle_pat[s] == 'f' ? 0xff : 0x00;
		}
		if (ioctl(fd, SSC_PCM_IOC_TX_IDLE, &ti) < 0)
			perror("TX_IDLE");
		else if (idle_pat)
			printf("idle frame: pattern %s\n", idle_pat);
		else
			printf("idle frame: all 0x%02x\n", idle);
	}

	for (s = 1; s <= last; s++)
		if (do_step(s, regs)) {
			rc = 1;
			break;
		}

	if (!rc && wait) {
		printf("holding the device open for %u s\n", wait);
		fflush(stdout);
		sleep(wait);
	}

	if (!rc && last == SSC_PCM_STEP_DMA) {
		unsigned int fb = cfg.slots * 2;
		size_t need = sizeof(struct ssc_pcm_block) +
			      (size_t)fb * cfg.frames_per_block;
		unsigned char *buf = calloc(1, need), prev[MAX_FB];
		unsigned long changes[MAX_FB] = { 0 }, nframes = 0;
		unsigned char first[MAX_FB];
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		unsigned long blocks = 0, gaps = 0, flagged = 0, dumped = 0;
		double t0 = now_s(), tstat = t0, end = t0 + (rx_secs ?: 2);
		uint32_t next_seq = 0;
		FILE *out = save ? fopen(save, "ab") : NULL;
		unsigned int f, b;
		unsigned long alt_n = 0;
		unsigned char *alt = calloc(64, fb);

		if (save && !out)
			perror(save);
		if (tx_n) {
			unsigned char *tx = calloc(tx_n, fb);
			ssize_t n;

			for (f = 0; f < tx_n; f++)
				memset(tx + (size_t)f * fb, f & 1 ? 0x00 : 0xff,
				       fb);
			n = write(fd, tx, (size_t)tx_n * fb);
			printf("tx: queued %zd of %u bytes\n", n, tx_n * fb);
			free(tx);
		}
		while (now_s() < end) {
			ssize_t n;
			struct ssc_pcm_block *h = (struct ssc_pcm_block *)buf;

			if (stats_secs && now_s() >= tstat + stats_secs) {
				char tag[32];

				tstat = now_s();
				snprintf(tag, sizeof(tag), "t=%.0f",
					 tstat - t0);
				print_stats(tag);
			}
			pfd.events = POLLIN | (txalt ? POLLOUT : 0);
			if (poll(&pfd, 1, 1000) <= 0)
				continue;
			if (txalt && (pfd.revents & POLLOUT)) {
				ssize_t w;

				for (f = 0; f < 64; f++)
					memset(alt + (size_t)f * fb,
					       (alt_n + f) / txalt & 1 ? 0x00 : 0xff,
					       fb);
				w = write(fd, alt, (size_t)64 * fb);
				if (w > 0)
					alt_n += w / fb;
			}
			if (!(pfd.revents & POLLIN))
				continue;
			n = read(fd, buf, need);
			if (n < 0) {
				perror("read");
				rc = 2;
				break;
			}
			if (h->magic != SSC_PCM_BLOCK_MAGIC) {
				printf("bad magic 0x%08x\n", h->magic);
				rc = 2;
				break;
			}
			if (blocks && h->seq != next_seq)
				gaps += h->seq - next_seq;
			if (h->flags & SSC_PCM_BF_DROPPED)
				flagged++;
			next_seq = h->seq + 1;
			blocks++;
			if (out)
				fwrite(buf, 1, n, out);
			for (f = 0; f < cfg.frames_per_block; f++) {
				unsigned char *fr = buf + sizeof(*h) + f * fb;

				if (dumped < dump) {
					printf("frame %4lu: ", dumped);
					print_frame(fr, fb);
					printf("\n");
					dumped++;
				}
				if (!nframes)
					memcpy(first, fr, fb);
				for (b = 0; nframes && b < fb; b++)
					changes[b] += fr[b] != prev[b];
				memcpy(prev, fr, fb);
				count_frame(fr, fb);
				nframes++;
			}
		}
		if (out)
			fclose(out);
		if (txalt)
			printf("txalt: %lu frames written\n", alt_n);
		free(alt);
		printf("rx: %lu blocks, %lu frames in %.1f s, seq gaps %lu, dropped flags %lu\n",
		       blocks, nframes, now_s() - t0, gaps, flagged);
		if (nframes) {
			unsigned int k, j;

			printf("first frame: ");
			print_frame(first, fb);
			printf("\nchanges per byte (frame to frame):");
			for (b = 0; b < fb; b++)
				printf(" %lu", changes[b]);
			printf("\n");
			/* the most frequent frames */
			for (k = 0; k < TOP_N; k++) {
				struct entry *best = NULL;

				for (j = 0; j < TABLE_SIZE; j++)
					if (table[j].used && table[j].n &&
					    (!best || table[j].n > best->n))
						best = &table[j];
				if (!best)
					break;
				printf("top %u (%lu x): ", k, best->n);
				print_frame(best->f, fb);
				printf("\n");
				best->n = 0;
			}
			if (table_full)
				printf("(%lu frames not tabulated)\n",
				       table_full);
		}
		print_stats("stats");
		if (gaps || flagged)
			rc = 3;
		free(buf);
	}

	do_step(SSC_PCM_STEP_OFF, 0);
	close(fd);
	return rc;
}
