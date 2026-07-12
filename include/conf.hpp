/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#ifndef CONF_HPP__
#define CONF_HPP__

#include <string>
#include <vector>

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

    /* structural rain parameters */
    int         depth_levels   = 4;
    int         max_droplets   = 1000;
    int         droplet_length = 50;
    int         spawn_attempts = 1;
    int         fps            = 20;
    std::string charset        = MATRIX_CHARS;

    /* pacing and resource behaviour */
    int         idle_timeout   = 0;     // seconds; 0 disables the idle pause
    bool        hidpi          = true;  // render HiDPI outputs at buffer scale

    bool operator==(const Config&) const = default;
};


/* the structural subset that requires Rain::configure when it changes */
static inline RainParams rain_params(const Config& c) {
    RainParams p;
    p.depth_levels = c.depth_levels;
    p.max_droplets = c.max_droplets;
    p.droplet_length = c.droplet_length;
    p.spawn_attempts = c.spawn_attempts;
    p.fps = c.fps;
    p.charset = c.charset;
    return p;
}


/* per-depth pixel sizes: layer 0 is base, the last layer is
 * FARTHEST_FONT_SIZE, the layers between interpolate linearly (floored) */
static inline std::vector<int> depth_font_sizes(int base, int n) {
    std::vector<int> sizes(n);
    for (int i = 0; i < n; i++)
        sizes[i] = n <= 1 ? base
                          : (base * (n - 1 - i) + FARTHEST_FONT_SIZE * i) / (n - 1);
    return sizes;
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
