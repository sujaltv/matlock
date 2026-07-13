/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#ifndef UTIL_HPP__
#define UTIL_HPP__

#include <cstddef>
#include <string>
#include <vector>


class Utils {
    public:
        /* print error message and exit the program */
        static void die(const char*, ...);

        /* get the hash of the current user */
        static const char* get_hash(void);

        /* constant-time memory comparison */
        static int timingsafe_bcmp(const void* a, const void* b, size_t len);

        /* decode UTF-8 into codepoints; false (and `out` cleared) if `s` is
         * not well-formed UTF-8 */
        static bool utf8_decode(const std::string& s,
                               std::vector<char32_t>& out);

        /* whether a codepoint is a character the rain can draw: not a control
         * character, and not blank (a space of any width) */
        static bool printable_codepoint(char32_t cp);
};

#ifdef __linux__
    /* prevent the process from being killed */
    void dont_kill_me();
#endif

#endif /* UTIL_HPP__ */
