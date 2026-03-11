/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#ifndef UTIL_HPP__
#define UTIL_HPP__


class Utils {
    public:
        /* print error message and exit the program */
        static void die(const char*, ...);

        /* get the hash of the current user */
        static const char* get_hash(void);

        /* constant-time memory comparison */
        static int timingsafe_bcmp(const void* a, const void* b, size_t len);
};

#ifdef __linux__
    /* prevent the process from being killed */
    void dont_kill_me();
#endif

#endif /* UTIL_HPP__ */
