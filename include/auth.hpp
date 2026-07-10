/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#ifndef AUTH_HPP__
#define AUTH_HPP__


/* possible states (plus number of states) in which matlock can be */
struct States {
    enum {
        INIT,
        INPUT,
        FAILED,
        NUMSTATES
    };
};


/* abstract key classes fed into the password state machine */
enum class Key {
    Char,
    Enter,
    Escape,
    Backspace,
    Ignore
};


class Auth {
    public:
        Auth(const char* hash, int failonclear);
        ~Auth();

        /* feed one key event; utf8/len are only used when k == Key::Char */
        void feed(Key k, const char* utf8 = nullptr, int len = 0);

        /* whether crypt matched the hash */
        bool unlocked() const { return this->unlocked_; }

        /* current States:: value derived from input length and failure */
        int state() const;

        /* hot-reloadable configuration */
        void set_failonclear(bool v) { this->failonclear_ = v; }

    private:
        const char* hash_;
        int failonclear_;
        char passwd_[256];
        unsigned int len_ = 0;
        int failure_ = 0;
        bool unlocked_ = false;
};


#endif /* AUTH_HPP__ */
