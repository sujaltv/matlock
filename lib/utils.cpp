/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

/* $OpenBSD: explicit_bzero.c,v 1.3 2014/06/21 02:34:26 matthew Exp $
 * 'explicit_bzero' originally written by Matthew Dempsky. */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <unistd.h>
#include <pwd.h>
#include <cerrno>
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


int Utils::timingsafe_bcmp(const void* a, const void* b, size_t len) {
	const volatile unsigned char* p1 = (const volatile unsigned char*) a;
	const volatile unsigned char* p2 = (const volatile unsigned char*) b;
	volatile unsigned char result = 0;

	for (size_t i = 0; i < len; i++)
		result |= p1[i] ^ p2[i];

	return result != 0;
}


#ifdef __linux__
    #include <fcntl.h>
    #include <linux/oom.h>
    #include <cstdio>
    #include <cerrno>

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
