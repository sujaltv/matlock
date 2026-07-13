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


bool Utils::utf8_decode(const std::string& s, std::vector<char32_t>& out) {
    /**
     * Strict UTF-8 decoder: rejects stray continuation bytes, truncated
     * sequences, overlong encodings, surrogates and anything above U+10FFFF,
     * so a mangled configuration file can never reach the rasteriser as a
     * half-formed codepoint.
     */

    static const char32_t lowest[4] = {0x0, 0x80, 0x800, 0x10000};

    out.clear();
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        char32_t cp;
        int extra;

        if (c < 0x80)                { cp = c;          extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu;  extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu;  extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u;  extra = 3; }
        else                         { out.clear(); return false; }

        if (i + (size_t)extra >= s.size()) {
            out.clear();
            return false;
        }
        for (int k = 1; k <= extra; k++) {
            unsigned char cc = (unsigned char)s[i + (size_t)k];
            if ((cc & 0xC0) != 0x80) {
                out.clear();
                return false;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (cp < lowest[extra] || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            out.clear();
            return false;
        }

        out.push_back(cp);
        i += (size_t)extra + 1;
    }
    return true;
}


bool Utils::printable_codepoint(char32_t cp) {
    if (cp <= 0x20 || cp == 0x7F)               // C0 controls, space, DEL
        return false;
    if (cp >= 0x80 && cp <= 0xA0)               // C1 controls, no-break space
        return false;
    if (cp >= 0x2000 && cp <= 0x200F)           // Unicode spaces, zero widths
        return false;
    if (cp == 0x2028 || cp == 0x2029)           // line and paragraph separators
        return false;
    if (cp == 0x3000 || cp == 0xFEFF)           // ideographic space, BOM
        return false;
    return true;
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
