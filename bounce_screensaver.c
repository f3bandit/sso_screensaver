/*
 * SuperStation One / Retro Remake -- animated intro screensaver
 * Same architecture as the earlier bounce_screensaver: draws directly to
 * the Linux framebuffer, bounces around like a DVD logo, exits on any
 * keyboard/controller input. This version plays back the full animated
 * intro (decoded GIF frames baked into an external asset file) instead
 * of a single static logo image.
 *
 * No runtime dependencies beyond the kernel framebuffer + evdev
 * interfaces: no SDL, no image libraries, no Python. Reads its frame
 * data from intro_frames.bin, which must sit next to this binary (or be
 * pointed to via argv[1]).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <linux/fb.h>
#include <linux/input.h>

#define MAX_INPUT_FDS 16

static volatile sig_atomic_t g_quit = 0;
static void on_signal(int sig) { (void) sig; g_quit = 1; }

typedef struct {
	int fd;
	uint8_t *fbmem;
	size_t fbsize;
	struct fb_var_screeninfo vinfo;
	struct fb_fix_screeninfo finfo;
	int bytes_per_pixel;
} Framebuffer;

static int fb_open(Framebuffer *fb, const char *path) {
	fb->fd = open(path, O_RDWR);
	if (fb->fd < 0) {
		perror("open framebuffer");
		return -1;
	}
	if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->vinfo) < 0) {
		perror("FBIOGET_VSCREENINFO");
		close(fb->fd);
		return -1;
	}
	if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->finfo) < 0) {
		perror("FBIOGET_FSCREENINFO");
		close(fb->fd);
		return -1;
	}
	fb->bytes_per_pixel = fb->vinfo.bits_per_pixel / 8;
	fb->fbsize = (size_t) fb->finfo.line_length * fb->vinfo.yres_virtual;
	fb->fbmem = mmap(NULL, fb->fbsize, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
	if (fb->fbmem == MAP_FAILED) {
		perror("mmap framebuffer");
		close(fb->fd);
		return -1;
	}
	fprintf(stdout, "fb: %dx%d, %dbpp, line_length=%u\n",
		fb->vinfo.xres, fb->vinfo.yres, fb->vinfo.bits_per_pixel, fb->finfo.line_length);
	fprintf(stdout, "fb: red(off=%d,len=%d) green(off=%d,len=%d) blue(off=%d,len=%d)\n",
		fb->vinfo.red.offset, fb->vinfo.red.length,
		fb->vinfo.green.offset, fb->vinfo.green.length,
		fb->vinfo.blue.offset, fb->vinfo.blue.length);
	fflush(stdout);
	return 0;
}

static void fb_close(Framebuffer *fb) {
	if (fb->fbmem && fb->fbmem != MAP_FAILED)
		munmap(fb->fbmem, fb->fbsize);
	if (fb->fd >= 0)
		close(fb->fd);
}

static inline uint32_t pack_pixel(const Framebuffer *fb, uint8_t r, uint8_t g, uint8_t b) {
	uint32_t rl = (r * ((1 << fb->vinfo.red.length) - 1)) / 255;
	uint32_t gl = (g * ((1 << fb->vinfo.green.length) - 1)) / 255;
	uint32_t bl = (b * ((1 << fb->vinfo.blue.length) - 1)) / 255;
	return (rl << fb->vinfo.red.offset) | (gl << fb->vinfo.green.offset) | (bl << fb->vinfo.blue.offset);
}

static void fb_clear(Framebuffer *fb) {
	memset(fb->fbmem, 0, fb->fbsize);
}

static void fb_blit_rgb565_scaled(
	Framebuffer *fb, const uint16_t *src, int sw, int sh,
	int dx, int dy, int dw, int dh,
	const int *sx_lut, const int *sy_lut
) {
	/* dx,dy,dw,dh are guaranteed fully on-screen by the caller (scale is
	   chosen so the destination rect always fits), so no per-pixel bounds
	   checks are needed here -- that and precomputed x/y lookup tables
	   (instead of a division per pixel) are what actually matter for
	   keeping this at frame rate on the HPS ARM core. */
	int fast565 = 0; /* disabled for now -- forcing the general per-channel
		conversion path below, which respects vinfo's actual bit
		offsets/lengths instead of assuming exact 5-6-5 layout. If this
		fixes the color corruption, the fast path's assumption was wrong
		for this hardware and should stay disabled or be fixed properly. */

	for (int y = 0; y < dh; y++) {
		int fy = dy + y;
		int sy = sy_lut[y];
		const uint16_t *srow = src + (size_t) sy * sw;
		uint8_t *row = fb->fbmem + (size_t) fy * fb->finfo.line_length;

		if (fast565) {
			uint16_t *drow = (uint16_t *) (row + (size_t) dx * 2);
			for (int x = 0; x < dw; x++)
				drow[x] = srow[sx_lut[x]];
			continue;
		}

		for (int x = 0; x < dw; x++) {
			uint16_t px = srow[sx_lut[x]];
			uint8_t r = ((px >> 11) & 0x1F) << 3;
			uint8_t g = ((px >> 5) & 0x3F) << 2;
			uint8_t b = (px & 0x1F) << 3;
			uint32_t out = pack_pixel(fb, r, g, b);
			uint8_t *p = row + (size_t) (dx + x) * fb->bytes_per_pixel;
			if (fb->bytes_per_pixel == 4) {
				*(uint32_t *) p = out;
			} else if (fb->bytes_per_pixel == 3) {
				p[0] = out & 0xFF;
				p[1] = (out >> 8) & 0xFF;
				p[2] = (out >> 16) & 0xFF;
			}
		}
	}
}

/* ---------------- input handling ---------------- */

static int find_input_devices(int *fds, int max_fds) {
	DIR *d = opendir("/dev/input");
	if (!d) return 0;
	struct dirent *ent;
	int count = 0;
	while ((ent = readdir(d)) != NULL && count < max_fds) {
		if (strncmp(ent->d_name, "event", 5) != 0) continue;
		char path[300];
		snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
		int fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd >= 0) fds[count++] = fd;
	}
	closedir(d);
	return count;
}

static void drain_stale_input(int *fds, int n) {
	/* Discard anything already sitting in the input queues -- e.g. the
	   keypress used to dismiss a preceding console prompt (like vmode's
	   "Press any key to continue") would otherwise be read as this
	   program's own "exit now" signal the instant it starts watching,
	   causing an almost-immediate exit before anything is visible. */
	for (int i = 0; i < n; i++) {
		struct input_event ev;
		while (read(fds[i], &ev, sizeof(ev)) == (ssize_t) sizeof(ev)) { }
	}
}

static int input_pending(int *fds, int n) {
	if (n == 0) return 0;
	struct pollfd pfds[MAX_INPUT_FDS];
	for (int i = 0; i < n; i++) {
		pfds[i].fd = fds[i];
		pfds[i].events = POLLIN;
		pfds[i].revents = 0;
	}
	int r = poll(pfds, n, 0);
	if (r <= 0) return 0;
	for (int i = 0; i < n; i++) {
		if (pfds[i].revents & POLLIN) {
			struct input_event ev;
			ssize_t rd;
			int got = 0;
			while ((rd = read(fds[i], &ev, sizeof(ev))) == (ssize_t) sizeof(ev)) {
				if (ev.type == EV_KEY && ev.value == 1) got = 1;
			}
			if (got) return 1;
		}
	}
	return 0;
}

/* ---------------- frame asset loading ---------------- */

typedef struct {
	uint32_t n_frames, w, h;
	uint16_t *durations;
	size_t frame_bytes;
	uint8_t *all_frames; /* whole file's frame data loaded into RAM up front */
} FrameSource;

static int frames_open(FrameSource *fs, const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		perror("open frame asset");
		return -1;
	}
	char magic[4];
	if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "SSIF", 4) != 0) {
		fprintf(stdout, "bad frame asset magic\n");
		fclose(f);
		return -1;
	}
	uint32_t hdr[3];
	if (fread(hdr, sizeof(uint32_t), 3, f) != 3) {
		fclose(f);
		return -1;
	}
	fs->n_frames = hdr[0];
	fs->w = hdr[1];
	fs->h = hdr[2];
	fs->durations = malloc(sizeof(uint16_t) * fs->n_frames);
	if (fread(fs->durations, sizeof(uint16_t), fs->n_frames, f) != fs->n_frames) {
		fclose(f);
		return -1;
	}
	fs->frame_bytes = (size_t) fs->w * fs->h * 2;
	size_t total = fs->frame_bytes * fs->n_frames;
	fs->all_frames = malloc(total);
	if (!fs->all_frames) {
		fprintf(stdout, "failed to allocate %zu bytes for frame data\n", total);
		fclose(f);
		return -1;
	}
	/* Read the whole animation into RAM once at startup so playback never
	   has to wait on SD card seek/read latency -- that stalling was the
	   main source of stutter during scrolling/fade sections. */
	size_t got = fread(fs->all_frames, 1, total, f);
	fclose(f);
	if (got != total) {
		fprintf(stdout, "short read loading frame data: got %zu of %zu\n", got, total);
		free(fs->all_frames);
		return -1;
	}
	fprintf(stdout, "frames: %u @ %ux%u (%zu bytes loaded)\n", fs->n_frames, fs->w, fs->h, total);
	return 0;
}

static inline const uint16_t *frames_get(FrameSource *fs, uint32_t idx) {
	return (const uint16_t *) (fs->all_frames + (size_t) idx * fs->frame_bytes);
}

static void frames_close(FrameSource *fs) {
	free(fs->durations);
	free(fs->all_frames);
}

/* ---------------- main ---------------- */

int main(int argc, char **argv) {
	/* Redirect our own diagnostics to a fixed log file at startup --
	   when launched via MiSTer.ini's main= mechanism (no controlling
	   terminal, unknown stdout destination), this is the only way to
	   actually see what happened after the fact. */
	FILE *log_out = freopen("/tmp/ssone_main_launch.log", "w", stdout);
	FILE *log_err = freopen("/tmp/ssone_main_launch.log", "a", stderr);
	(void) log_out;
	(void) log_err;
	setvbuf(stdout, NULL, _IONBF, 0);

	const char *asset_path = (argc > 1) ? argv[1] : "/media/fat/linux/sso/intro_frames_4x3.bin";
	fprintf(stdout, "main() entered, argc=%d, asset_path=%s\n", argc, asset_path);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	Framebuffer fb;
	if (fb_open(&fb, "/dev/fb0") < 0)
		return 1;

	FrameSource fs;
	memset(&fs, 0, sizeof(fs));
	if (frames_open(&fs, asset_path) < 0) {
		fb_close(&fb);
		return 1;
	}

	int input_fds[MAX_INPUT_FDS];
	int n_inputs = find_input_devices(input_fds, MAX_INPUT_FDS);
	drain_stale_input(input_fds, n_inputs);
	fprintf(stdout, "watching %d input device(s) for exit\n", n_inputs);
	fflush(stdout);

	fprintf(stdout, "starting in 5 seconds -- read the fb: lines above now\n");
	fflush(stdout);
	sleep(5);
	drain_stale_input(input_fds, n_inputs); /* discard anything that arrived during the pause too */

	int sw = (int) fs.w, sh = (int) fs.h;
	int screen_w = (int) fb.vinfo.xres, screen_h = (int) fb.vinfo.yres;

	/* Scale to fill the screen, preserving aspect ratio (letterboxed if the
	   screen's aspect ratio doesn't match the source frames exactly). */
	double scale_x = (double) screen_w / sw;
	double scale_y = (double) screen_h / sh;
	double scale = (scale_x < scale_y) ? scale_x : scale_y;
	int dest_w = (int) (sw * scale);
	int dest_h = (int) (sh * scale);
	int dest_x = (screen_w - dest_w) / 2;
	int dest_y = (screen_h - dest_h) / 2;

	int *sx_lut = malloc(sizeof(int) * dest_w);
	int *sy_lut = malloc(sizeof(int) * dest_h);
	for (int x = 0; x < dest_w; x++) sx_lut[x] = (x * sw) / dest_w;
	for (int y = 0; y < dest_h; y++) sy_lut[y] = (y * sh) / dest_h;

	fb_clear(&fb);
	fprintf(stdout, "draw loop starting now, blitting frame 0\n");
	fflush(stdout);

	uint32_t frame_idx = 0;

	while (!g_quit) {
		if (input_pending(input_fds, n_inputs))
			break;

		const uint16_t *pixels = frames_get(&fs, frame_idx);
		if (!pixels) break;

		fb_blit_rgb565_scaled(&fb, pixels, sw, sh, dest_x, dest_y, dest_w, dest_h, sx_lut, sy_lut);

		if (frame_idx == 0) {
			fprintf(stdout, "frame 0 blitted successfully\n");
			fflush(stdout);
		}

		uint16_t dur = fs.durations[frame_idx];
		if (dur < 10) dur = 10;
		struct timespec req = { .tv_sec = dur / 1000, .tv_nsec = (long) (dur % 1000) * 1000000L };
		nanosleep(&req, NULL);

		frame_idx = (frame_idx + 1) % fs.n_frames;
	}

	free(sx_lut);
	free(sy_lut);

	fb_clear(&fb);
	for (int i = 0; i < n_inputs; i++) close(input_fds[i]);
	frames_close(&fs);
	fb_close(&fb);
	return 0;
}
