/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#ifndef CONF_HPP__
#define CONF_HPP__

#include <string>

#include "auth.hpp"
#include "rain.hpp"

/* pixel size of the farthest rain layer; the closest layer is
 * Config::font_size and the layers between interpolate linearly */
#define FARTHEST_FONT_SIZE 10


struct Config {
    std::string background   = "#141D1A";
    std::string font_pattern = "monospace";
    int         font_size    = 20;
    bool        mutate_chars = true;
    bool        failonclear  = false;
    std::string fontcolour[States::NUMSTATES] = {
        "#1cff7b",      /* States::INIT */
        "#28a99e",      /* States::INPUT */
        "#E28743",      /* States::FAILED */
    };

    bool operator==(const Config&) const = default;
};


/* per-depth pixel sizes: layer 0 is base, the last layer is
 * FARTHEST_FONT_SIZE, the layers between interpolate linearly (floored) */
static inline void depth_font_sizes(int base, int sizes[DEPTH_LEVELS]) {
    for (int i = 0; i < DEPTH_LEVELS; i++)
        sizes[i] = (base * (DEPTH_LEVELS - 1 - i) + FARTHEST_FONT_SIZE * i)
                   / (DEPTH_LEVELS - 1);
}


class Conf {
    public:
        /* loads /etc/matlock.yaml, then the per-user override
         * ($XDG_CONFIG_HOME/matlock/matlock.yaml), and starts watching both
         * for changes; invalid values warn and keep their defaults */
        Conf();
        ~Conf();

        const Config& cfg() const { return this->cfg_; }

        /* inotify fd to poll for configuration changes; -1 if unavailable */
        int watch_fd() const { return this->inotify_fd_; }

        /* drain the watch fd and re-read the files; true if cfg() changed.
         * A file that can no longer be read aborts the reload and keeps the
         * current configuration. */
        bool reload_if_changed();

    private:
        bool load(Config& into, bool initial);

        Config cfg_;
        std::string user_dir_;
        std::string user_path_;
        int inotify_fd_ = -1;
};


#endif /* CONF_HPP__ */
