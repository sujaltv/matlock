/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#ifndef RAIN_HPP__
#define RAIN_HPP__

/* The Matrix-like rain-related constants */
#define MAX_DROPLETS 1000
#define NUM_THREADS_PER_FRAME 2
#define DROPLET_LENGTH 50
#define MATRIX_CHARS "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789~`!@#$%^&*()_+-=[]{}\\;:'\",<.>/?"
#define NUM_MATRIX_CHARS ((int)sizeof(MATRIX_CHARS) - 1)
#define UPDATE_INTERVAL 10042
#define DEPTH_LEVELS 4

#include <cstdint>


struct Droplet {
    int active;                         // Whether this stream is currently active
    int depth;                          // Depth level (0 = closest, DEPTH_LEVELS-1 = farthest)
    int x;                              // x position
    int y;                              // y position
    int speed;                          // How many pixels to move per update
    int length;                         // Actual length of this stream
    char chars[DROPLET_LENGTH];         // Characters in this stream (cold, accessed only during draw)
};


/* per-depth glyph cell metrics, filled in by the backend at init */
struct DepthMetrics {
    int char_width;
    int char_height;
};


/* body alpha per depth; head colours use min(1.0, alpha * 1.3) */
static const float depth_alpha[DEPTH_LEVELS] = {1.0f, 0.65f, 0.4f, 0.2f};


/* xorshift32 PRNG */
static inline uint32_t fast_rand(uint32_t& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s;
}


struct Rain {
    public:
        struct Droplet droplets[MAX_DROPLETS];
        int active_list[MAX_DROPLETS];      // Indices of active droplets
        int active_count;                   // Number of active droplets
        int free_head;                      // Head of free-list (-1 = empty)
        uint32_t rng_state;                 // xorshift32 PRNG state
        struct DepthMetrics metrics[DEPTH_LEVELS];

        /* reset droplets to a free-list and seed the PRNG */
        void init(uint32_t seed);

        /* spawn a single droplet */
        void rain_droplet(int width, int height);

        /* one simulation step: spawn, deactivate, mutate, move */
        void step(int width, int height, bool mutate_chars);
};


#endif /* RAIN_HPP__ */
