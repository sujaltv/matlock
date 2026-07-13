/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#ifndef RAIN_HPP__
#define RAIN_HPP__

/* default charset for the raining characters */
#define MATRIX_CHARS "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789~`!@#$%^&*()_+-=[]{}\\;:'\",<.>/?"

/* upper bound on the number of characters in a charset; the droplets store an
 * index into it, so it must stay within a uint16_t */
#define MAX_CHARSET 1024

/* the historical implicit tick rate (1e6 / 10042 us == 99.6 Hz); the
 * per-second visual behaviour is anchored to this so that any configured
 * fps looks identical in velocity, spawn density and mutation rate */
#define RAIN_REF_HZ 100

#include <cstdint>
#include <string>
#include <vector>


struct Droplet {
    int active;                         // Whether this stream is currently active
    int depth;                          // Depth level (0 = closest)
    int x;                              // x position
    int y;                              // y position
    int speed;                          // How many pixels to move per update
    int length;                         // Actual length of this stream
};


/* per-depth glyph cell metrics, filled in by the backend */
struct DepthMetrics {
    int char_width;
    int char_height;
};


/* The classic four-level depth look, kept as anchor curves and resampled to
 * the configured number of levels: exact at four, interpolated otherwise. */
static const float DEPTH_ALPHA_CURVE[4]  = {1.0f, 0.65f, 0.4f, 0.2f};
static const float DEPTH_LENGTH_CURVE[4] = {1.0f, 0.7f, 0.45f, 0.25f};
static const float DEPTH_SPEED_CURVE[4]  = {5.0f, 3.0f, 2.0f, 1.0f};

static inline float sample_curve(const float a[4], int n, int i) {
    if (n <= 1) return a[0];
    float t = (float)i * 3.0f / (float)(n - 1);
    int k = (int)t;
    if (k >= 3) return a[3];
    return a[k] + (a[k + 1] - a[k]) * (t - (float)k);
}


/* xorshift32 PRNG */
static inline uint32_t fast_rand(uint32_t& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s;
}


/* MATRIX_CHARS as codepoints; the literal is ASCII, so the bytes are the
 * codepoints */
static inline std::vector<char32_t> default_charset() {
    std::vector<char32_t> cs;
    for (const char* p = MATRIX_CHARS; *p; p++)
        cs.push_back((char32_t)(unsigned char)*p);
    return cs;
}


/* structural simulation parameters (from the configuration); changing any
 * of these requires Rain::configure */
struct RainParams {
    int depth_levels = 4;
    int max_droplets = 1000;
    int droplet_length = 50;            // longest stream, in characters
    int spawn_attempts = 1;             // spawn attempts per step, 1-in-5 each
    int fps = 20;                       // simulation/redraw rate (10..120)
    std::vector<char32_t> charset = default_charset();

    bool operator==(const RainParams&) const = default;
};


struct Rain {
    public:
        RainParams p;
        std::vector<Droplet> droplets;
        /* p.droplet_length characters per droplet, each held as an index into
         * p.charset rather than the codepoint itself: the renderer's atlas is
         * keyed the same way, so drawing is a direct lookup and no character
         * ever needs decoding on the hot path */
        std::vector<uint16_t> chars;
        std::vector<int> active_list;       // Indices of active droplets
        std::vector<DepthMetrics> metrics;  // sized p.depth_levels
        std::vector<int> speed_base;        // per-depth base speed
        std::vector<float> length_scale;    // per-depth length factor
        int active_count = 0;               // Number of active droplets
        int free_head = -1;                 // Head of free-list (-1 = empty)
        uint32_t rng_state = 1;             // xorshift32 PRNG state

        /* per-tick thresholds derived from p.fps so the per-second look is
         * anchored to RAIN_REF_HZ regardless of the configured frame rate */
        int speed_jitter = 2;               // extra head speed: fast_rand % this
        int spawn_mod = 500;                // spawn when fast_rand % this < RAIN_REF_HZ
        int mutate_threshold = 2;           // mutate when (rnd & 0x1F) < this

        /* size everything for the given parameters and reset the free-list */
        void configure(const RainParams& params, uint32_t seed);

        /* the character stream of droplet i, as charset indices */
        uint16_t* droplet_chars(int i) {
            return &this->chars[(size_t)i * this->p.droplet_length];
        }
        const uint16_t* droplet_chars(int i) const {
            return &this->chars[(size_t)i * this->p.droplet_length];
        }

        /* spawn a single droplet */
        void rain_droplet(int width, int height);

        /* one simulation step: spawn, deactivate, mutate, move */
        void step(int width, int height, bool mutate_chars);
};


#endif /* RAIN_HPP__ */
