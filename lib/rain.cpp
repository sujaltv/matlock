/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#include "../include/rain.hpp"


void Rain::init(uint32_t seed) {
    /**
     * Initialise the droplets as a free-list and seed the PRNG.
     */

    this->free_head = 0;
    this->active_count = 0;
    this->rng_state = seed | 1; // must be non-zero
    for (int i = 0; i < MAX_DROPLETS; i++) {
        this->droplets[i].active = 0;
        this->droplets[i].x = (i + 1 < MAX_DROPLETS) ? i + 1 : -1;
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

    this->droplets[idx].x = fast_rand(this->rng_state) % width;
    this->droplets[idx].y = 0;
    this->droplets[idx].depth = fast_rand(this->rng_state) % DEPTH_LEVELS;
    switch (this->droplets[idx].depth) {
        case 0: this->droplets[idx].speed = 5 + (fast_rand(this->rng_state) % 2); break;
        case 1: this->droplets[idx].speed = 3 + (fast_rand(this->rng_state) % 2); break;
        case 2: this->droplets[idx].speed = 2 + (fast_rand(this->rng_state) % 2); break;
        case 3: this->droplets[idx].speed = 1;                                     break;
    }
    // Closer droplets are longer, farther ones shorter
    static const float depth_length_scale[DEPTH_LEVELS] = {1.0f, 0.7f, 0.45f, 0.25f};
    int max_len = (int)(DROPLET_LENGTH * depth_length_scale[this->droplets[idx].depth]);
    this->droplets[idx].length = max_len - (fast_rand(this->rng_state) % (max_len / 2 + 1));
    this->droplets[idx].active = 1;
    this->active_list[this->active_count++] = idx;

    for (int j = 0; j < this->droplets[idx].length; j++) {
        this->droplets[idx].chars[j] = MATRIX_CHARS[fast_rand(this->rng_state) % NUM_MATRIX_CHARS];
    }
}


void Rain::step(int width, int height, bool mutate_chars) {
    /**
     * Advance the simulation by one tick: spawn new droplets, deactivate
     * off-screen ones, mutate characters and move everything down.
     */

    // Randomly create new droplets (spawn up to 2 per frame)
    for (int s = 0; s < NUM_THREADS_PER_FRAME; s++) {
        if (fast_rand(this->rng_state) % 5 == 0)
            this->rain_droplet(width, height);
    }

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
            uint32_t rnd = fast_rand(this->rng_state);
            int bits_left = 6;
            for (int j = 0; j < drop.length; j++) {
                if ((rnd & 0x1F) < 2) {
                    drop.chars[j] = MATRIX_CHARS[fast_rand(this->rng_state) % NUM_MATRIX_CHARS];
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
