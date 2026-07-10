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
#include <unistd.h>
#ifdef __linux__
    #include <crypt.h>
#endif

#include "../include/arg.hpp"
#include "../include/utils.hpp"
#include "../include/auth.hpp"
#include "../include/backend.hpp"
#include "../include/conf.hpp"
#include "../include/x11_backend.hpp"
#include "../include/wl_backend.hpp"
#include "../include/config.hpp"


char *argv0;


static void usage(void) {
    /**
     * Print usage.
     */

	Utils::die("usage: %s [-v] [cmd [arg ...]]\n", NAME);
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

    /* parse command line arguments and handle cases accordingly */
	ARGBEGIN {
        case 'v':
            fprintf(stderr, "%s-" VERSION "\n", NAME);
            return 0;
        default:
            usage();
	} ARGEND

    /* load /etc/matlock.yaml and the per-user override, and watch both for
     * live changes */
    Conf conf;

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
