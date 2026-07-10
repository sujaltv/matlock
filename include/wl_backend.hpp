/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#ifndef WL_BACKEND_HPP__
#define WL_BACKEND_HPP__

#include <memory>

#include "auth.hpp"
#include "backend.hpp"
#include "conf.hpp"


class WlBackend : public Backend {
    public:
        explicit WlBackend(Conf& conf);
        ~WlBackend() override;

        bool lock() override;
        int conn_fd() const override;
        void run(Auth& auth) override;

        struct Impl;

    private:
        std::unique_ptr<Impl> impl_;
};


#endif /* WL_BACKEND_HPP__ */
