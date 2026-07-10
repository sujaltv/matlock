/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#ifndef BACKEND_HPP__
#define BACKEND_HPP__

#include "auth.hpp"


class Backend {
    public:
        virtual ~Backend() = default;

        /* create lock surfaces on every output; false = could not lock all */
        virtual bool lock() = 0;

        /* display connection fd, to close in the post-lock command child */
        virtual int conn_fd() const = 0;

        /* event loop: feeds Auth, steps Rain every UPDATE_INTERVAL, redraws,
         * hot-reloads the configuration, returns when auth.unlocked() */
        virtual void run(Auth& auth) = 0;
};


#endif /* BACKEND_HPP__ */
