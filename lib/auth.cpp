/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __linux__
    #include <crypt.h>
#endif

#include "../include/auth.hpp"
#include "../include/utils.hpp"


Auth::Auth(const char* hash, int failonclear)
    : hash_(hash), failonclear_(failonclear) {
    explicit_bzero(&this->passwd_, sizeof(this->passwd_));
    /* keep the cleartext password off swap; not fatal if it fails */
    mlock(this->passwd_, sizeof(this->passwd_));
}


Auth::~Auth() {
    explicit_bzero(&this->passwd_, sizeof(this->passwd_));
    munlock(this->passwd_, sizeof(this->passwd_));
}


void Auth::feed(Key k, const char* utf8, int len) {
    /**
     * Feed one key event into the password state machine.
     */

    char* inputhash;

    switch (k) {
    case Key::Enter:
        this->passwd_[this->len_] = '\0';
        errno = 0;
        if (!(inputhash = crypt(this->passwd_, this->hash_)))
            fprintf(stderr, "%s: crypt: %s\n", NAME, strerror(errno));
        else {
            size_t hlen = strlen(this->hash_);
            this->unlocked_ = strlen(inputhash) == hlen &&
                              !Utils::timingsafe_bcmp(inputhash, this->hash_, hlen);
        }
        if (!this->unlocked_)
            this->failure_ = 1;
        explicit_bzero(&this->passwd_, sizeof(this->passwd_));
        this->len_ = 0;
        break;
    case Key::Escape:
        explicit_bzero(&this->passwd_, sizeof(this->passwd_));
        this->len_ = 0;
        this->failure_ = 0;
        break;
    case Key::Backspace:
        /* remove one character, not one byte: strip the UTF-8 continuation
         * bytes, then the lead byte */
        while (this->len_) {
            unsigned char b = (unsigned char)this->passwd_[--this->len_];
            this->passwd_[this->len_] = '\0';
            if ((b & 0xC0) != 0x80)
                break;
        }
        break;
    case Key::Char:
        if (len && !iscntrl((unsigned char)utf8[0]) &&
            (this->len_ + len < sizeof(this->passwd_))) {
            memcpy(this->passwd_ + this->len_, utf8, len);
            this->len_ += len;
        }
        break;
    case Key::Ignore:
        break;
    }
}


int Auth::state() const {
    return this->len_ ? States::INPUT
                      : ((this->failure_ || this->failonclear_) ? States::FAILED
                                                                : States::INIT);
}
