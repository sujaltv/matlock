/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#include "../include/rain.hpp"


void Rain::configure(const RainParams& params, uint32_t seed) {
    /**
     * Size the droplet pool and per-depth tables for the given parameters,
     * derive the depth curves, and reset the droplets to a free-list.
     */

    this->p = params;
    this->droplets.assign(this->p.max_droplets, Droplet{});
    this->chars.assign((size_t)this->p.max_droplets * this->p.droplet_length, 0);
    this->active_list.assign(this->p.max_droplets, 0);
    this->metrics.assign(this->p.depth_levels, DepthMetrics{});

    /* The curves are px/tick at RAIN_REF_HZ; rescale so the on-screen
     * velocity (px/s) is preserved at the configured fps: fewer, larger
     * steps at a lower fps, but the same distance covered per second. */
    float rescale = (float)RAIN_REF_HZ / (float)this->p.fps;
    this->speed_base.resize(this->p.depth_levels);
    this->length_scale.resize(this->p.depth_levels);
    for (int d = 0; d < this->p.depth_levels; d++) {
        float sp = sample_curve(DEPTH_SPEED_CURVE, this->p.depth_levels, d) * rescale;
        this->speed_base[d] = sp < 1.0f ? 1 : (int)(sp + 0.5f);
        this->length_scale[d] = sample_curve(DEPTH_LENGTH_CURVE, this->p.depth_levels, d);
    }

    /* head-speed jitter: fast_rand % speed_jitter; at RAIN_REF_HZ this is
     * % 2 (0..1 px/tick), as it always was, and its mean px/s is held */
    this->speed_jitter = (int)(rescale + 0.5f) + 1;
    if (this->speed_jitter < 2) this->speed_jitter = 2;
    /* spawn probability per attempt is RAIN_REF_HZ / spawn_mod == 20/fps, so
     * the expected spawns/s stay spawn_attempts * RAIN_REF_HZ / 5 for fps > 20;
     * below that the per-attempt probability saturates at 1 and density falls
     * off (an accepted edge, well outside the default fps=30) */
    this->spawn_mod = 5 * this->p.fps;
    /* mutation chance per char per tick, held near 6.25/s per char */
    int mt = (int)(2.0f * rescale + 0.5f);
    this->mutate_threshold = mt > 31 ? 31 : (mt < 1 ? 1 : mt);

    this->free_head = 0;
    this->active_count = 0;
    this->rng_state = seed | 1; // must be non-zero
    for (int i = 0; i < this->p.max_droplets; i++) {
        this->droplets[i].active = 0;
        this->droplets[i].x = (i + 1 < this->p.max_droplets) ? i + 1 : -1;
    }
}


void Rain::rain_droplet(int width, int height) {
    /**
     * Create a droplet (a stream of characters) and rain it down the screen.
     */

    if (width <= 0 || height <= 0) return;

    int idx;

    if (this->free_head >= 0) {
        // O(1) allocation from free-list
        idx = this->free_head;
        this->free_head = this->droplets[idx].x; // next-free stored in x
    } else {
        // Fallback: recycle the furthest-down droplet from active list
        if (this->active_count == 0) return;
        int worst_ai = 0;
        int max_y = this->droplets[this->active_list[0]].y;
        for (int ai = 1; ai < this->active_count; ai++) {
            int y = this->droplets[this->active_list[ai]].y;
            if (y > max_y) { max_y = y; worst_ai = ai; }
        }
        idx = this->active_list[worst_ai];
        // Swap-remove recycled entry from active list
        this->active_list[worst_ai] = this->active_list[--this->active_count];
    }

    struct Droplet& drop = this->droplets[idx];
    drop.x = fast_rand(this->rng_state) % width;
    drop.y = 0;
    drop.depth = fast_rand(this->rng_state) % this->p.depth_levels;
    // Closer droplets are faster; all but the farthest level get jitter
    drop.speed = this->speed_base[drop.depth];
    if (drop.depth < this->p.depth_levels - 1)
        drop.speed += fast_rand(this->rng_state) % this->speed_jitter;
    // Closer droplets are longer, farther ones shorter
    int max_len = (int)(this->p.droplet_length * this->length_scale[drop.depth]);
    if (max_len < 1) max_len = 1;
    drop.length = max_len - (fast_rand(this->rng_state) % (max_len / 2 + 1));
    if (drop.length < 1) drop.length = 1;
    drop.active = 1;
    this->active_list[this->active_count++] = idx;

    uint16_t* dc = this->droplet_chars(idx);
    uint32_t ncs = (uint32_t)this->p.charset.size();
    for (int j = 0; j < drop.length; j++) {
        dc[j] = (uint16_t)(fast_rand(this->rng_state) % ncs);
    }
}


void Rain::step(int width, int height, bool mutate_chars) {
    /**
     * Advance the simulation by one tick: spawn new droplets, deactivate
     * off-screen ones, mutate characters and move everything down.
     */

    // Randomly create new droplets
    for (int s = 0; s < this->p.spawn_attempts; s++) {
        if ((int)(fast_rand(this->rng_state) % this->spawn_mod) < RAIN_REF_HZ)
            this->rain_droplet(width, height);
    }

    uint32_t ncs = (uint32_t)this->p.charset.size();

    // Update positions, mutate chars, deactivate off-screen droplets
    for (int ai = 0; ai < this->active_count; ai++) {
        int i = this->active_list[ai];
        struct Droplet& drop = this->droplets[i];
        int dch = this->metrics[drop.depth].char_height;

        // Deactivate if the tail (topmost char) has scrolled past the bottom
        int tail_y = drop.y - ((drop.length - 1) * dch);
        if (tail_y > height) {
            drop.active = 0;
            drop.x = this->free_head;
            this->free_head = i;
            // Swap-remove from active list
            this->active_list[ai] = this->active_list[--this->active_count];
            ai--;
            continue;
        }

        // Mutate characters
        if (mutate_chars) {
            uint16_t* dc = this->droplet_chars(i);
            uint32_t rnd = fast_rand(this->rng_state);
            int bits_left = 6;
            for (int j = 0; j < drop.length; j++) {
                if ((int)(rnd & 0x1F) < this->mutate_threshold) {
                    dc[j] = (uint16_t)(fast_rand(this->rng_state) % ncs);
                }
                rnd >>= 5;
                if (--bits_left <= 0) {
                    rnd = fast_rand(this->rng_state);
                    bits_left = 6;
                }
            }
        }

        // Move the droplet down
        drop.y += drop.speed;
    }
}
