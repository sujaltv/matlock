/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#ifndef RAIN_HPP__
#define RAIN_HPP__

/* default charset for the raining characters */
#define MATRIX_CHARS "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789~`!@#$%^&*()_+-=[]{}\\;:'\",<.>/?"

/* one simulation step per this many microseconds */
#define UPDATE_INTERVAL 10042

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


/* structural simulation parameters (from the configuration); changing any
 * of these requires Rain::configure */
struct RainParams {
    int depth_levels = 4;
    int max_droplets = 1000;
    int droplet_length = 50;            // longest stream, in characters
    int spawn_attempts = 2;             // spawn attempts per step, 1-in-5 each
    std::string charset = MATRIX_CHARS;

    bool operator==(const RainParams&) const = default;
};


struct Rain {
    public:
        RainParams p;
        std::vector<Droplet> droplets;
        std::vector<char> chars;            // p.droplet_length chars per droplet
        std::vector<int> active_list;       // Indices of active droplets
        std::vector<DepthMetrics> metrics;  // sized p.depth_levels
        std::vector<int> speed_base;        // per-depth base speed
        std::vector<float> length_scale;    // per-depth length factor
        int active_count = 0;               // Number of active droplets
        int free_head = -1;                 // Head of free-list (-1 = empty)
        uint32_t rng_state = 1;             // xorshift32 PRNG state

        /* size everything for the given parameters and reset the free-list */
        void configure(const RainParams& params, uint32_t seed);

        /* the character stream of droplet i */
        char* droplet_chars(int i) {
            return &this->chars[(size_t)i * this->p.droplet_length];
        }
        const char* droplet_chars(int i) const {
            return &this->chars[(size_t)i * this->p.droplet_length];
        }

        /* spawn a single droplet */
        void rain_droplet(int width, int height);

        /* one simulation step: spawn, deactivate, mutate, move */
        void step(int width, int height, bool mutate_chars);
};


#endif /* RAIN_HPP__ */
