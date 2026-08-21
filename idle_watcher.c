/*
 * SuperStation One / Retro Remake -- idle screensaver watcher
 *
 * Runs continuously in the background (started from user-startup.sh at
 * boot). Watches input devices for genuine activity -- filtering out
 * analog-stick drift/electrical noise the same way MiSTer Super Attract
 * Mode does (a real per-axis deadzone, not "any event at all") -- and
 * once nothing real has happened for IDLE_SECONDS while sitting at the
 * menu core (not inside a running game), launches the already-proven
 * screensaver launcher script. Guards against launching it twice
 * concurrently, and against a second copy of itself ever running.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/poll.h>
#include <linux/input.h>

#define MAX_INPUT_FDS 16
#define MAX_ABS_AXES 64
#define STARTUP_DELAY_SEC 60
#define DEFAULT_IDLE_SECONDS 180
#define POLL_INTERVAL_SEC 2
#define PIDFILE "/tmp/ssone_idle_watcher.pid"

/* Same threshold MiSTer Super Attract Mode uses for analog stick noise
   (out of the usual +/-32767 raw axis range). */
#define AXIS_DEADZONE 2000

static volatile sig_atomic_t g_quit = 0;
static void on_signal(int sig) { (void) sig; g_quit = 1; }

/* Refuse to start a second copy: if the pidfile names a PID that's still
   alive, bail out instead of stacking another watcher on top of it. */
static int acquire_single_instance_lock(void) {
	FILE *f = fopen(PIDFILE, "r");
	if (f) {
		pid_t existing = 0;
		if (fscanf(f, "%d", &existing) == 1 && existing > 0) {
			if (kill(existing, 0) == 0 || errno == EPERM) {
				fclose(f);
				fprintf(stderr, "idle_watcher: already running as pid %d, exiting\n", existing);
				return -1;
			}
		}
		fclose(f);
	}
	f = fopen(PIDFILE, "w");
	if (!f) {
		fprintf(stderr, "idle_watcher: warning: couldn't write pidfile, continuing anyway\n");
		return 0;
	}
	fprintf(f, "%d\n", getpid());
	fclose(f);
	return 0;
}

static void release_single_instance_lock(void) {
	unlink(PIDFILE);
}

typedef struct {
	int fd;
	int16_t last_abs[MAX_ABS_AXES]; /* last known value per ABS axis code */
	int have_last_abs[MAX_ABS_AXES];
} InputDevice;

static InputDevice g_devices[MAX_INPUT_FDS];
static int g_n_devices = 0;

static void close_all_devices(void) {
	for (int i = 0; i < g_n_devices; i++)
		if (g_devices[i].fd >= 0) close(g_devices[i].fd);
	g_n_devices = 0;
}

static void find_input_devices(void) {
	close_all_devices();
	DIR *d = opendir("/dev/input");
	if (!d) return;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL && g_n_devices < MAX_INPUT_FDS) {
		if (strncmp(ent->d_name, "event", 5) != 0) continue;
		char path[300];
		snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
		int fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd >= 0) {
			InputDevice *dev = &g_devices[g_n_devices++];
			dev->fd = fd;
			memset(dev->have_last_abs, 0, sizeof(dev->have_last_abs));
		}
	}
	closedir(d);
}

/* Discard anything already queued -- e.g. the keypress used to dismiss a
   preceding console prompt, so it isn't misread as fresh "activity" the
   instant we start watching. */
static void drain_stale_input(void) {
	for (int i = 0; i < g_n_devices; i++) {
		struct input_event ev;
		while (read(g_devices[i].fd, &ev, sizeof(ev)) == (ssize_t) sizeof(ev)) { }
	}
}

/* Returns 1 if any device shows GENUINE activity: a real key press, real
   mouse movement, or an analog axis moving past the deadzone -- not just
   noise/drift. This is the actual fix for "reads static from inputs and
   never goes idle." */
static int had_real_input(void) {
	if (g_n_devices == 0) return 0;
	struct pollfd pfds[MAX_INPUT_FDS];
	for (int i = 0; i < g_n_devices; i++) {
		pfds[i].fd = g_devices[i].fd;
		pfds[i].events = POLLIN;
		pfds[i].revents = 0;
	}
	int r = poll(pfds, g_n_devices, 0);
	if (r <= 0) return 0;

	int any_real = 0;
	for (int i = 0; i < g_n_devices; i++) {
		if (!(pfds[i].revents & POLLIN)) continue;
		InputDevice *dev = &g_devices[i];
		struct input_event ev;
		ssize_t rd;
		while ((rd = read(dev->fd, &ev, sizeof(ev))) == (ssize_t) sizeof(ev)) {
			if (ev.type == EV_KEY && ev.value == 1) {
				/* a real press (not release, not autorepeat) */
				any_real = 1;
			} else if (ev.type == EV_REL) {
				/* mouse movement -- doesn't suffer the same drift
				   problem analog sticks do, count it directly */
				any_real = 1;
			} else if (ev.type == EV_ABS) {
				if (ev.code < MAX_ABS_AXES) {
					int16_t v = (int16_t) ev.value;
					if (dev->have_last_abs[ev.code]) {
						int delta = v - dev->last_abs[ev.code];
						if (delta < 0) delta = -delta;
						if (delta > AXIS_DEADZONE)
							any_real = 1;
					}
					dev->last_abs[ev.code] = v;
					dev->have_last_abs[ev.code] = 1;
				}
			}
		}
	}
	return any_real;
}

static int at_menu_core(void) {
	FILE *f = fopen("/tmp/CORENAME", "r");
	if (!f) return 0; /* if we can't tell, don't risk interrupting a game */
	char buf[64];
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return 0;
	}
	fclose(f);
	size_t len = strlen(buf);
	while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' || buf[len-1] == ' '))
		buf[--len] = 0;
	return strcmp(buf, "MENU") == 0;
}

int main(int argc, char **argv) {
	/* argv[1]: the proven launcher script (the one that already draws
	   correctly -- no vmode, no mbc, no SAM involved).
	   argv[2]: idle timeout in seconds (optional; 0 or missing/invalid
	   falls back to the default). A value of -1 disables the
	   screensaver entirely (watcher still runs, never triggers) --
	   used for the installer's "Never" timeout option. */
	const char *launcher_script = (argc > 1)
		? argv[1]
		: "/media/fat/linux/sso/intro_screensaver.sh";

	long idle_seconds = DEFAULT_IDLE_SECONDS;
	int disabled = 0;
	if (argc > 2) {
		char *endptr = NULL;
		long v = strtol(argv[2], &endptr, 10);
		if (endptr != argv[2] && *endptr == 0) {
			if (v < 0) {
				disabled = 1;
			} else if (v > 0) {
				idle_seconds = v;
			}
		}
	}

	if (acquire_single_instance_lock() < 0)
		return 1;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	fprintf(stderr, "idle_watcher: starting, %ds startup delay\n", STARTUP_DELAY_SEC);
	fprintf(stderr, "idle_watcher: launcher = %s\n", launcher_script);
	fprintf(stderr, "idle_watcher: idle timeout = %s\n", disabled ? "disabled (Never)" : "configured");
	if (!disabled)
		fprintf(stderr, "idle_watcher: idle seconds = %ld\n", idle_seconds);
	sleep(STARTUP_DELAY_SEC);

	find_input_devices();
	drain_stale_input();
	fprintf(stderr, "idle_watcher: watching %d input device(s)\n", g_n_devices);

	time_t last_activity = time(NULL);
	int rescan_counter = 0;
	int screensaver_active = 0;

	while (!g_quit) {
		sleep(POLL_INTERVAL_SEC);

		if (++rescan_counter >= 30) {
			rescan_counter = 0;
			find_input_devices();
		}

		if (had_real_input()) {
			last_activity = time(NULL);
			continue;
		}

		if (disabled)
			continue;

		if (screensaver_active) {
			/* the screensaver process itself handles its own
			   exit-on-input and returns control when it's done;
			   we just wait for it below via waitpid, so we should
			   never actually reach here while it's running -- this
			   is a safety net only. */
			continue;
		}

		time_t idle_for = time(NULL) - last_activity;
		if (idle_for < idle_seconds)
			continue;

		if (!at_menu_core())
			continue; /* a game/core is running -- don't interrupt it */

		fprintf(stderr, "idle_watcher: idle %lds at menu, launching screensaver\n", (long) idle_for);

		screensaver_active = 1;
		pid_t pid = fork();
		if (pid == 0) {
			execl(launcher_script, launcher_script, (char *) NULL);
			_exit(127);
		} else if (pid > 0) {
			int status;
			waitpid(pid, &status, 0); /* blocks until it exits on input */
		}
		screensaver_active = 0;

		/* input devices may have been reopened/renumbered while the
		   screensaver core was running; refresh and drain before
		   resuming idle tracking */
		find_input_devices();
		drain_stale_input();
		last_activity = time(NULL);
	}

	release_single_instance_lock();
	return 0;
}
