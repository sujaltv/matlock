/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#define _XOPEN_SOURCE 500
#ifndef NAME
    #define NAME "matlock"
#endif

#include <cerrno>
#include <grp.h>
#include <pwd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <vector>
#include <unistd.h>
#ifdef __linux__
    #include <crypt.h>
#endif

#include "../include/arg.hpp"
#include "../include/utils.hpp"
#include "../include/auth.hpp"
#include "../include/backend.hpp"
#include "../include/conf.hpp"
#include "../include/rain.hpp"
#include "../include/render.hpp"
#include "../include/x11_backend.hpp"
#include "../include/wl_backend.hpp"
#include "../include/config.hpp"


char *argv0;


static void usage(void) {
    /**
     * Print usage.
     */

	Utils::die("usage: %s [-v] [-b [frames]] [cmd [arg ...]]\n", NAME);
}


static long peak_rss_kib(void) {
    /* VmHWM (peak resident set size) from /proc/self/status, in KiB */
    FILE* f = fopen("/proc/self/status", "r");
    if (!f)
        return -1;
    char line[256];
    long kib = -1;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "VmHWM: %ld kB", &kib) == 1)
            break;
    }
    fclose(f);
    return kib;
}


static int run_benchmark(const Config& cfg, int frames) {
    /**
     * Headless render benchmark: build the renderer and one simulation from
     * the loaded config and render `frames` frames into a heap buffer, with
     * no display connection and no privileges. Reports ms/frame and peak RSS
     * so each optimisation has before/after numbers.
     */

    int w = 1920, h = 1080;
    const char* sz = getenv("MATLOCK_BENCH_SIZE");
    if (sz) {
        int pw = 0, ph = 0;
        if (sscanf(sz, "%dx%d", &pw, &ph) == 2 && pw > 0 && ph > 0) {
            w = pw;
            h = ph;
        } else {
            fprintf(stderr, "%s: ignoring invalid MATLOCK_BENCH_SIZE '%s'\n",
                    NAME, sz);
        }
    }

    Renderer renderer;
    renderer.configure(cfg);
    if (!renderer.has_face())
        Utils::die("%s: benchmark: no usable font for pattern '%s'\n", NAME,
                   cfg.font_pattern.c_str());

    Rain rain;
    rain.configure(renderer.rain_params(), (uint32_t)rand());
    rain.metrics = renderer.atlas_for(1).metrics;

    std::vector<uint32_t> px((size_t)w * h);
    RenderTarget target;
    std::vector<DirtyRect> damage;

    /* warm up so the droplet field is populated before timing */
    for (int i = 0; i < 300; i++)
        rain.step(w, h, cfg.mutate_chars);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int f = 0; f < frames; f++) {
        rain.step(w, h, cfg.mutate_chars);
        renderer.draw(rain, px.data(), w, h, 1, States::INIT, target, damage);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double total_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                      (t1.tv_nsec - t0.tv_nsec) / 1e6;
    double ms = total_ms / frames;

    printf("%s benchmark: %dx%d, %d frames, config fps=%d\n",
           NAME, w, h, frames, cfg.fps);
    printf("  %.4f ms/frame  (%.0f frames/s achievable)\n", ms,
           ms > 0 ? 1000.0 / ms : 0.0);
    printf("  peak RSS: %ld KiB\n", peak_rss_kib());
    return 0;
}


struct TargetIds {
    uid_t uid;
    gid_t gid;
};


static TargetIds lookup_drop_target() {
    /**
     * Resolve the unprivileged user and group to drop to. Looked up before
     * any privileges are shed so a bad name fails fast.
     */

	struct passwd *pwd;
	struct group *grp;

	errno = 0;
	if (!(pwd = getpwnam(user)))
		Utils::die("%s: getpwnam %s: %s\n", NAME, user,
		    errno ? strerror(errno) : "user entry not found");
	errno = 0;
	if (!(grp = getgrnam(group)))
		Utils::die("%s: getgrnam %s: %s\n", NAME, group,
		    errno ? strerror(errno) : "group entry not found");
	return TargetIds{pwd->pw_uid, grp->gr_gid};
}


static void drop_privileges(uid_t uid, gid_t gid) {
    /**
     * Permanently drop to uid:gid. The effective uid may have been switched
     * to the invoking user (see main), so the saved euid 0 is regained first;
     * without privileges that fails, which is fine as long as the target is
     * the invoking user already (the unprivileged benchmark path).
     */

	if (geteuid() != 0)
		seteuid(0);
	if (geteuid() != 0) {
		if (getuid() == uid)
			return;                 // nothing runs elevated, nothing to drop
		Utils::die("%s: cannot drop to uid %d without privileges\n", NAME,
		    (int)uid);
	}
	if (setgroups(0, NULL) < 0)
		Utils::die("%s: setgroups: %s\n", NAME, strerror(errno));
	if (setgid(gid) < 0)
		Utils::die("%s: setgid: %s\n", NAME, strerror(errno));
	if (setuid(uid) < 0)
		Utils::die("%s: setuid: %s\n", NAME, strerror(errno));
}


int main(int argc, char* argv[]) {
    srand(time(NULL) ^ getpid());
	const char *hash;
	errno = 0;

    bool bench = false;
    int bench_frames = 1000;

    /* parse command line arguments and handle cases accordingly */
	ARGBEGIN {
        case 'v':
            fprintf(stderr, "%s-" VERSION "\n", NAME);
            return 0;
        case 'b': {
            bench = true;
            char* a = ARGF();
            if (a)
                bench_frames = atoi(a);
            if (bench_frames <= 0)
                usage();
            break;
        }
        default:
            usage();
	} ARGEND

    /* headless render benchmark: it only parses configuration and fonts, so
     * it never runs with privileges */
    if (bench) {
        drop_privileges(getuid(), getgid());
        Conf conf;
        return run_benchmark(conf.cfg(), bench_frames);
    }

    /* the work that needs euid 0 (a suid install) happens first: the OOM
     * adjust and the shadow hash */
    TargetIds target = lookup_drop_target();
    #ifdef __linux__
        dont_kill_me();
    #endif
    hash = Utils::get_hash();
    errno = 0;
    if (!crypt("", hash))
        Utils::die("%s: crypt: %s\n", NAME, strerror(errno));

    /* Everything from here on parses input the invoking user controls: the
     * configuration files, fontconfig's lookup (FONTCONFIG_FILE, user font
     * dirs), FreeType's font parsing and the display protocol. None of that
     * may run with euid 0, so switch to the invoking user now; the saved
     * euid 0 is kept for the permanent drop below. */
    if (geteuid() == 0 && getuid() != 0 && seteuid(getuid()) < 0)
        Utils::die("%s: seteuid: %s\n", NAME, strerror(errno));

    /* load /etc/matlock.yaml and the per-user override, and watch both for
     * live changes */
    Conf conf;

    /* pick the backend from the session environment; the constructor
     * connects to the display server and resolves the rain font while
     * fontconfig still sees the invoking user's configuration */
    std::unique_ptr<Backend> backend;
    if (getenv("WAYLAND_DISPLAY"))
        backend = std::make_unique<WlBackend>(conf);
    else if (getenv("DISPLAY"))
        backend = std::make_unique<X11Backend>(conf);
    else
        Utils::die("%s: neither WAYLAND_DISPLAY nor DISPLAY is set\n", NAME);

    /* fonts and colours are resolved: drop to the unprivileged user for the
     * lifetime of the lock */
    drop_privileges(target.uid, target.gid);

	/* did we manage to lock everything? */
	if (!backend->lock())
		return 1;

	/* run post-lock command */
	if (argc > 0) {
		switch (fork()) {
		case -1:
			Utils::die("%s: fork failed: %s\n", NAME, strerror(errno));
		case 0:
			if (close(backend->conn_fd()) < 0)
				Utils::die("%s: close: %s\n", NAME, strerror(errno));
			execvp(argv[0], argv);
			fprintf(stderr, "%s: execvp %s: %s\n", NAME, argv[0], strerror(errno));
			_exit(1);
		}
	}

	/* everything is now blank. Wait for the correct password */
    Auth auth(hash, conf.cfg().failonclear);
    backend->run(auth);

	return 0;
}
