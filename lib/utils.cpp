/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

/* $OpenBSD: explicit_bzero.c,v 1.3 2014/06/21 02:34:26 matthew Exp $
 * 'explicit_bzero' originally written by Matthew Dempsky. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <pwd.h>
#include <errno.h>
#if HAVE_SHADOW_H
    #include <shadow.h>
#endif

#include "../include/utils.hpp"


void Utils::die(const char* errstr, ...) {
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(1);
}

const char* Utils::get_hash(void) {
	const char* hash;
	struct passwd* pw;

	/* Check if the current user has a password entry */
	errno = 0;
	if (!(pw = getpwuid(getuid()))) {
		if (errno)
			Utils::die("%s: getpwuid: %s\n", NAME, strerror(errno));
		else
			Utils::die("%s: cannot retrieve password entry\n", NAME);
	}
	hash = pw->pw_passwd;

    #if HAVE_SHADOW_H
    	if (!strcmp(hash, "x")) {
    		struct spwd* sp;
    		if (!(sp = getspnam(pw->pw_name)))
    			Utils::die("%s: getspnam: cannot retrieve shadow entry. "
    			    "Make sure to suid or sgid %s.\n", NAME, NAME);
    		hash = sp->sp_pwdp;
    	}
    #else
    	if (!strcmp(hash, "*")) {
            #ifdef __OpenBSD__
    	    	if (!(pw = getpwuid_shadow(getuid())))
    	    		Utils::die("%s: getpwnam_shadow: cannot retrieve shadow entry. "
    	    		    "Make sure to suid or sgid %s.\n", NAME, NAME);
    	    	hash = pw->pw_passwd;
            #else
                Utils::die("%s: getpwuid: cannot retrieve shadow entry. "
    	    	    "Make sure to suid or sgid %s.\n", NAME, NAME);
            #endif /* __OpenBSD__ */
    	}
    #endif /* HAVE_SHADOW_H */

	return hash;
}


__attribute__((weak)) void __explicit_bzero_hook(void* buf, size_t len) {}

void explicit_bzero(void* buf, size_t len) {
	memset(buf, 0, len);
	__explicit_bzero_hook(buf, len);
}


#ifdef __linux__
    #include <fcntl.h>
    #include <linux/oom.h>
    #include <cstdio>
    #include <errno.h>

    void dont_kill_me() {
    	FILE* f;
    	const char oomfile[] = "/proc/self/oom_score_adj";

    	if (!(f = fopen(oomfile, "w"))) {
    		if (errno == ENOENT)
    			return;
            Utils::die("%s: fopen %s: %s\n", NAME, oomfile, strerror(errno));
    	}
    	fprintf(f, "%d", OOM_SCORE_ADJ_MIN);
    	if (fclose(f)) {
    		if (errno == EACCES)
    			Utils::die("%s: unable to disable OOM killer. "
    			    "Make sure to suid or sgid %s.\n", NAME, NAME);
    		else
    			Utils::die("%s: fclose %s: %s\n", NAME, oomfile, strerror(errno));
    	}
    }
#endif
