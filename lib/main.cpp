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

    /* warm up so the droplet field is populated before timing */
    for (int i = 0; i < 300; i++)
        rain.step(w, h, cfg.mutate_chars);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int f = 0; f < frames; f++) {
        rain.step(w, h, cfg.mutate_chars);
        renderer.draw(rain, px.data(), w, h, 1, States::INIT);
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


const char* get_user_hash() {
	struct passwd *pwd;
	struct group *grp;
	uid_t duid;
	gid_t dgid;
    const char* hash;

	if (!(pwd = getpwnam(user)))
		Utils::die("%s: getpwnam %s: %s\n", NAME, user,
		    errno ? strerror(errno) : "user entry not found");
	duid = pwd->pw_uid;
	errno = 0;
	if (!(grp = getgrnam(group)))
		Utils::die("%s: getgrnam %s: %s\n", NAME, group,
		    errno ? strerror(errno) : "group entry not found");
	dgid = grp->gr_gid;

    #ifdef __linux__
	    dont_kill_me();
    #endif

	hash = Utils::get_hash();
	errno = 0;
	if (!crypt("", hash))
		Utils::die("%s: crypt: %s\n", NAME, strerror(errno));

	/* drop privileges */
	if (setgroups(0, NULL) < 0)
		Utils::die("%s: setgroups: %s\n", NAME, strerror(errno));
	if (setgid(dgid) < 0)
		Utils::die("%s: setgid: %s\n", NAME, strerror(errno));
	if (setuid(duid) < 0)
		Utils::die("%s: setuid: %s\n", NAME, strerror(errno));

    return hash;
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

    /* load /etc/matlock.yaml and the per-user override, and watch both for
     * live changes */
    Conf conf;

    /* headless render benchmark: no display server, no privilege drop */
    if (bench)
        return run_benchmark(conf.cfg(), bench_frames);

    /* pick the backend from the session environment and connect to the
     * display server with real credentials, before the privilege drop in
     * get_user_hash() */
    std::unique_ptr<Backend> backend;
    if (getenv("WAYLAND_DISPLAY"))
        backend = std::make_unique<WlBackend>(conf);
    else if (getenv("DISPLAY"))
        backend = std::make_unique<X11Backend>(conf);
    else
        Utils::die("%s: neither WAYLAND_DISPLAY nor DISPLAY is set\n", NAME);

    hash = get_user_hash();

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
