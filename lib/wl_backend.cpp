/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <array>
#include <map>
#include <memory>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/timerfd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "ext-session-lock-v1.h"

#include "../include/wl_backend.hpp"
#include "../include/rain.hpp"
#include "../include/utils.hpp"


namespace {

/* an 8-bit coverage bitmap for one rasterised character */
struct Glyph {
    int w = 0, h = 0;
    int left = 0, top = 0;              // bearings relative to the baseline
    std::vector<uint8_t> cov;
};

/* the charset rasterised at every depth size, for one buffer scale */
struct Atlas {
    std::vector<DepthMetrics> metrics;              // sized depth_levels
    std::vector<std::array<Glyph, 128>> glyphs;     // [depth][ASCII char]
};


uint32_t parse_hex(const char* hex) {
    /* parse a "#RRGGBB" string into 0x00RRGGBB */
    unsigned int r = 0, g = 0, b = 0;
    sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b);
    return (r << 16) | (g << 8) | b;
}

uint32_t dim(uint32_t rgb, float alpha) {
    /* scale each channel; on an opaque background this equals alpha blending */
    uint32_t r = (uint32_t)(((rgb >> 16) & 0xFF) * alpha);
    uint32_t g = (uint32_t)(((rgb >> 8) & 0xFF) * alpha);
    uint32_t b = (uint32_t)((rgb & 0xFF) * alpha);
    return (r << 16) | (g << 8) | b;
}

long elapsed_us(const struct timespec& from, const struct timespec& to) {
    return (to.tv_sec - from.tv_sec) * 1000000L +
           (to.tv_nsec - from.tv_nsec) / 1000L;
}

bool filtered_sym(xkb_keysym_t s) {
    /* mirrors the X11 IsFunctionKey/IsKeypadKey/IsMiscFunctionKey/IsPFKey/
     * IsPrivateKeypadKey filter (keysym values are identical in xkbcommon) */
    return (s >= XKB_KEY_F1 && s <= XKB_KEY_F35) ||             // function
           (s >= XKB_KEY_KP_Space && s <= XKB_KEY_KP_Equal) ||  // keypad
           (s >= XKB_KEY_Select && s <= XKB_KEY_Break) ||       // misc function
           (s >= 0x11000000 && s <= 0x1100FFFF);                // private keypad
}

} // namespace


struct WlOutput;

struct WlBackend::Impl {
    Conf* conf = nullptr;

    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    uint32_t compositor_version = 0;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    wl_keyboard* keyboard = nullptr;
    ext_session_lock_manager_v1* lock_manager = nullptr;
    ext_session_lock_v1* session_lock = nullptr;

    bool locked = false;
    bool finished = false;

    std::vector<std::unique_ptr<WlOutput>> outputs;

    /* xkbcommon */
    xkb_context* xkb_ctx = nullptr;
    xkb_keymap* keymap = nullptr;
    xkb_state* xkb = nullptr;

    /* key repeat */
    int repeat_timerfd = -1;
    int32_t repeat_rate = 25;           // chars per second
    int32_t repeat_delay = 600;         // ms
    uint32_t repeat_key = 0;
    bool repeat_armed = false;

    /* rendering */
    uint32_t background = 0xFF000000;
    bool render_ready = false;          // atlases and colour tables built
    std::map<int, Atlas> atlases;       // keyed by integer buffer scale
    FT_Library ft = nullptr;
    FT_Face face = nullptr;
    std::vector<uint32_t> body_colour[States::NUMSTATES];
    std::vector<uint32_t> head_colour[States::NUMSTATES];
    std::vector<int> font_sizes;
    std::string applied_pattern;        // pattern the current face came from
    int applied_size = 0;
    RainParams applied_rain;            // structural params the rains use

    Auth* auth = nullptr;
    bool mutate_chars = false;

    void create_lock_surface(WlOutput* out);
    void render_and_commit(WlOutput* out, bool want_frame);
    void on_frame(WlOutput* out);
    const Atlas& atlas_for(int scale);
    bool init_fonts(const char* pattern);
    void apply_config();
    void handle_key_press(uint32_t key, bool from_repeat);
    void arm_repeat(uint32_t key);
    void disarm_repeat();
    void remove_output(uint32_t global_name);
};


struct WlOutput {
    struct BufSlot { WlOutput* out; int idx; };

    WlBackend::Impl* impl = nullptr;
    wl_output* output = nullptr;
    uint32_t global_name = 0;
    uint32_t version = 0;
    int32_t scale = 1;
    int32_t pending_scale = 1;
    bool done = false;                  // wl_output.done received

    wl_surface* surface = nullptr;
    ext_session_lock_surface_v1* lock_surface = nullptr;
    bool configured = false;
    uint32_t width = 0, height = 0;     // surface-local coordinates

    /* double-buffered wl_shm pool */
    int buf_w = 0, buf_h = 0;           // buffer pixels
    void* shm_data = nullptr;
    size_t shm_size = 0;
    wl_shm_pool* pool = nullptr;
    wl_buffer* buffers[2] = {nullptr, nullptr};
    bool busy[2] = {false, false};
    BufSlot slots[2] = {{this, 0}, {this, 1}};

    /* animation */
    Rain rain;
    bool rain_ready = false;
    wl_callback* frame_cb = nullptr;
    struct timespec last_step = {};
    int last_drawn_state = -1;

    void destroy_buffers() {
        for (int i = 0; i < 2; i++) {
            if (this->buffers[i]) {
                wl_buffer_destroy(this->buffers[i]);
                this->buffers[i] = nullptr;
            }
            this->busy[i] = false;
        }
        if (this->pool) {
            wl_shm_pool_destroy(this->pool);
            this->pool = nullptr;
        }
        if (this->shm_data) {
            munmap(this->shm_data, this->shm_size);
            this->shm_data = nullptr;
            this->shm_size = 0;
        }
        this->buf_w = this->buf_h = 0;
    }

    ~WlOutput() {
        if (this->frame_cb)
            wl_callback_destroy(this->frame_cb);
        if (this->lock_surface)
            ext_session_lock_surface_v1_destroy(this->lock_surface);
        if (this->surface)
            wl_surface_destroy(this->surface);
        this->destroy_buffers();
        if (this->output) {
            if (this->version >= 3)
                wl_output_release(this->output);
            else
                wl_output_destroy(this->output);
        }
    }
};


/* ------------------------------------------------------------------ */
/* wl_buffer                                                           */

static void buffer_release(void* data, wl_buffer*) {
    auto* slot = static_cast<WlOutput::BufSlot*>(data);
    slot->out->busy[slot->idx] = false;
}

static const wl_buffer_listener buffer_listener = { buffer_release };


static bool ensure_buffers(WlBackend::Impl* impl, WlOutput* out, int bw, int bh) {
    /**
     * (Re)create the two ARGB8888 buffers whenever the required pixel size
     * changes; buffers are picked via the wl_buffer.release busy flags.
     */

    if (bw <= 0 || bh <= 0)
        return false;
    if (out->buffers[0] && out->buf_w == bw && out->buf_h == bh)
        return true;

    out->destroy_buffers();

    int stride = bw * 4;
    size_t size = (size_t)stride * bh * 2;

    int fd = memfd_create("matlock-shm", MFD_CLOEXEC);
    if (fd < 0) {
        /* fallback for kernels without memfd */
        char name[64];
        snprintf(name, sizeof(name), "/matlock-%d", getpid());
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0)
            Utils::die("%s: shm_open: %s\n", NAME, strerror(errno));
        shm_unlink(name);
    }
    if (ftruncate(fd, size) < 0)
        Utils::die("%s: ftruncate: %s\n", NAME, strerror(errno));

    out->shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (out->shm_data == MAP_FAILED)
        Utils::die("%s: mmap: %s\n", NAME, strerror(errno));
    out->shm_size = size;

    out->pool = wl_shm_create_pool(impl->shm, fd, size);
    close(fd);

    for (int i = 0; i < 2; i++) {
        out->buffers[i] = wl_shm_pool_create_buffer(out->pool, i * stride * bh,
                                                    bw, bh, stride,
                                                    WL_SHM_FORMAT_ARGB8888);
        wl_buffer_add_listener(out->buffers[i], &buffer_listener, &out->slots[i]);
        out->busy[i] = false;
    }
    out->buf_w = bw;
    out->buf_h = bh;
    return true;
}


/* ------------------------------------------------------------------ */
/* rendering                                                           */

static void blit_glyph(uint32_t* px, int bw, int bh, const Glyph& g,
                       int ox, int oy, uint32_t col) {
    /* (ox, oy) is the baseline origin, as with XDrawString. The channel
     * maths must be signed: a dimmer glyph over a brighter pixel makes
     * (r - dr) negative. */
    int x0 = ox + g.left;
    int y0 = oy - g.top;
    int r = (col >> 16) & 0xFF, gr = (col >> 8) & 0xFF, b = col & 0xFF;

    for (int row = 0; row < g.h; row++) {
        int y = y0 + row;
        if (y < 0 || y >= bh) continue;
        const uint8_t* src = &g.cov[(size_t)row * g.w];
        uint32_t* dst = px + (size_t)y * bw;
        for (int cx = 0; cx < g.w; cx++) {
            int x = x0 + cx;
            if (x < 0 || x >= bw) continue;
            int cov = src[cx];
            if (!cov) continue;
            uint32_t d = dst[x];
            int dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            dst[x] = 0xFF000000
                   | ((uint32_t)(dr + (r - dr) * cov / 255) << 16)
                   | ((uint32_t)(dg + (gr - dg) * cov / 255) << 8)
                   |  (uint32_t)(db + (b - db) * cov / 255);
        }
    }
}


static void draw_rain(WlBackend::Impl* impl, WlOutput* out, uint32_t* px,
                      const Atlas& atlas, int state) {
    /* body pass then head pass per depth, exactly as the X11 backend */
    for (int d = 0; d < out->rain.p.depth_levels; d++) {
        int dch = atlas.metrics[d].char_height;

        uint32_t body = impl->body_colour[state][d];
        for (int ai = 0; ai < out->rain.active_count; ai++) {
            int i = out->rain.active_list[ai];
            const Droplet& drop = out->rain.droplets[i];
            if (drop.depth != d) continue;
            const char* dc = out->rain.droplet_chars(i);
            for (int j = 1; j < drop.length; j++) {
                int y = drop.y - (j * dch);
                if (y < 0 || y > out->buf_h) continue;
                blit_glyph(px, out->buf_w, out->buf_h,
                           atlas.glyphs[d][(unsigned char)dc[j] & 0x7F],
                           drop.x, y, body);
            }
        }

        uint32_t head = impl->head_colour[state][d];
        for (int ai = 0; ai < out->rain.active_count; ai++) {
            int i = out->rain.active_list[ai];
            const Droplet& drop = out->rain.droplets[i];
            if (drop.depth != d) continue;
            if (drop.y >= 0 && drop.y <= out->buf_h) {
                blit_glyph(px, out->buf_w, out->buf_h,
                           atlas.glyphs[d][(unsigned char)out->rain.droplet_chars(i)[0] & 0x7F],
                           drop.x, drop.y, head);
            }
        }
    }
}


/* wl_callback (frame) */
static void frame_done(void* data, wl_callback* cb, uint32_t);
static const wl_callback_listener frame_listener = { frame_done };

static void frame_done(void* data, wl_callback* cb, uint32_t) {
    auto* out = static_cast<WlOutput*>(data);
    wl_callback_destroy(cb);
    out->frame_cb = nullptr;
    out->impl->on_frame(out);
}


void WlBackend::Impl::render_and_commit(WlOutput* out, bool want_frame) {
    /**
     * Draw the current content (background, plus rain once run() initialised
     * rendering) into a free buffer and commit it. Called only after the
     * first configure was acked. If both buffers are busy the commit still
     * happens so a requested frame callback keeps the animation alive.
     */

    if (!out->configured)
        return;

    int sc = (this->compositor_version >= 3) ? out->scale : 1;
    int bw = (int)out->width * sc;
    int bh = (int)out->height * sc;

    if (want_frame && !out->frame_cb) {
        out->frame_cb = wl_surface_frame(out->surface);
        wl_callback_add_listener(out->frame_cb, &frame_listener, out);
    }

    if (ensure_buffers(this, out, bw, bh)) {
        int bi = !out->busy[0] ? 0 : (!out->busy[1] ? 1 : -1);
        if (bi >= 0) {
            uint32_t* px = (uint32_t*)((char*)out->shm_data +
                                       (size_t)bi * out->buf_w * 4 * out->buf_h);
            std::fill_n(px, (size_t)out->buf_w * out->buf_h, this->background);

            if (this->render_ready) {
                const Atlas& atlas = this->atlas_for(sc);
                if (!out->rain_ready) {
                    out->rain.configure(this->applied_rain, (uint32_t)rand());
                    clock_gettime(CLOCK_MONOTONIC, &out->last_step);
                    out->rain_ready = true;
                }
                out->rain.metrics = atlas.metrics;
                int state = this->auth ? this->auth->state() : States::INIT;
                draw_rain(this, out, px, atlas, state);
                out->last_drawn_state = state;
            }

            if (this->compositor_version >= 3)
                wl_surface_set_buffer_scale(out->surface, sc);
            wl_surface_attach(out->surface, out->buffers[bi], 0, 0);
            wl_surface_damage_buffer(out->surface, 0, 0, INT32_MAX, INT32_MAX);
            out->busy[bi] = true;
        }
    }

    wl_surface_commit(out->surface);
}


void WlBackend::Impl::on_frame(WlOutput* out) {
    /**
     * Frame-callback driven animation: run the simulation for however many
     * UPDATE_INTERVALs elapsed (capped to absorb stalls), then redraw.
     */

    if (!this->render_ready || !out->configured || !out->rain_ready) {
        this->render_and_commit(out, true);
        return;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed = elapsed_us(out->last_step, now);
    long steps = elapsed / UPDATE_INTERVAL;

    if (steps > 5) {
        /* stalled (e.g. output off): drop the backlog */
        steps = 5;
        out->last_step = now;
    } else {
        long consumed = steps * UPDATE_INTERVAL;
        out->last_step.tv_nsec += (consumed % 1000000) * 1000;
        out->last_step.tv_sec += consumed / 1000000 +
                                 out->last_step.tv_nsec / 1000000000;
        out->last_step.tv_nsec %= 1000000000;
    }

    for (long s = 0; s < steps; s++)
        out->rain.step(out->buf_w, out->buf_h, this->mutate_chars);

    int state = this->auth ? this->auth->state() : States::INIT;
    if (steps == 0 && state == out->last_drawn_state) {
        /* nothing changed: keep the callback chain alive without drawing */
        if (!out->frame_cb) {
            out->frame_cb = wl_surface_frame(out->surface);
            wl_callback_add_listener(out->frame_cb, &frame_listener, out);
        }
        wl_surface_commit(out->surface);
        return;
    }

    this->render_and_commit(out, true);
}


const Atlas& WlBackend::Impl::atlas_for(int scale) {
    auto it = this->atlases.find(scale);
    if (it != this->atlases.end())
        return it->second;

    int n = this->applied_rain.depth_levels;
    Atlas& atlas = this->atlases[scale];
    atlas.metrics.resize(n);
    atlas.glyphs.resize(n);
    for (int d = 0; d < n; d++) {
        if (FT_Set_Pixel_Sizes(this->face, 0, this->font_sizes[d] * scale))
            Utils::die("%s: FT_Set_Pixel_Sizes failed\n", NAME);

        /* the face is monospaced: use a reference glyph for the advance */
        if (FT_Load_Char(this->face, 'M', FT_LOAD_RENDER))
            Utils::die("%s: FT_Load_Char failed\n", NAME);
        atlas.metrics[d].char_width = (int)(this->face->glyph->advance.x >> 6);
        atlas.metrics[d].char_height =
            (int)((this->face->size->metrics.ascender -
                   this->face->size->metrics.descender) >> 6);

        for (unsigned char ch : this->applied_rain.charset) {
            if (FT_Load_Char(this->face, ch, FT_LOAD_RENDER))
                continue;
            const FT_Bitmap& bm = this->face->glyph->bitmap;
            Glyph& g = atlas.glyphs[d][ch & 0x7F];
            g.w = (int)bm.width;
            g.h = (int)bm.rows;
            g.left = this->face->glyph->bitmap_left;
            g.top = this->face->glyph->bitmap_top;
            g.cov.resize((size_t)g.w * g.h);
            for (int row = 0; row < g.h; row++)
                memcpy(&g.cov[(size_t)row * g.w],
                       bm.buffer + (size_t)row * bm.pitch, g.w);
        }
    }
    return atlas;
}


bool WlBackend::Impl::init_fonts(const char* pattern) {
    /**
     * Resolve a fontconfig pattern to a font file and load its face,
     * replacing the current one. Returns false (keeping the current face)
     * on failure, so a bad hot-reloaded pattern never kills the locker.
     */

    FcPattern* pat = FcNameParse((const FcChar8*)pattern);
    if (!pat) {
        fprintf(stderr, "%s: cannot parse font pattern '%s'\n", NAME, pattern);
        return false;
    }
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcPattern* match = FcFontMatch(NULL, pat, &res);
    FcPatternDestroy(pat);
    if (!match) {
        fprintf(stderr, "%s: no font matches '%s'\n", NAME, pattern);
        return false;
    }

    FcChar8* file = NULL;
    if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch) {
        fprintf(stderr, "%s: matched font has no file\n", NAME);
        FcPatternDestroy(match);
        return false;
    }

    if (!this->ft && FT_Init_FreeType(&this->ft)) {
        fprintf(stderr, "%s: FT_Init_FreeType failed\n", NAME);
        FcPatternDestroy(match);
        return false;
    }
    FT_Face nface = nullptr;
    if (FT_New_Face(this->ft, (const char*)file, 0, &nface)) {
        fprintf(stderr, "%s: FT_New_Face %s failed\n", NAME, (const char*)file);
        FcPatternDestroy(match);
        return false;
    }
    FcPatternDestroy(match);

    if (this->face)
        FT_Done_Face(this->face);
    this->face = nface;
    return true;
}


void WlBackend::Impl::apply_config() {
    /**
     * (Re)compute everything derived from the configuration. Called at
     * startup and on hot reload; the next frame picks the changes up.
     */

    const Config& c = this->conf->cfg();

    this->mutate_chars = c.mutate_chars;
    if (this->auth)
        this->auth->set_failonclear(c.failonclear);

    bool rebuild = false;

    RainParams np = rain_params(c);
    if (!(np == this->applied_rain)) {
        this->applied_rain = np;
        /* structural change: restart the simulations */
        for (auto& out : this->outputs) {
            if (out->rain_ready)
                out->rain.configure(np, (uint32_t)rand());
        }
        rebuild = true;
    }

    int n = this->applied_rain.depth_levels;
    this->background = 0xFF000000 | parse_hex(c.background.c_str());
    for (int st = 0; st < States::NUMSTATES; st++) {
        uint32_t rgb = parse_hex(c.fontcolour[st].c_str());
        this->body_colour[st].resize(n);
        this->head_colour[st].resize(n);
        for (int d = 0; d < n; d++) {
            float a = sample_curve(DEPTH_ALPHA_CURVE, n, d);
            this->body_colour[st][d] = dim(rgb, a);
            this->head_colour[st][d] = dim(rgb, std::min(1.0f, a * 1.3f));
        }
    }

    if (c.font_pattern != this->applied_pattern) {
        if (this->init_fonts(c.font_pattern.c_str())) {
            this->applied_pattern = c.font_pattern;
            rebuild = true;
        } else {
            fprintf(stderr, "%s: keeping font '%s'\n", NAME,
                    this->applied_pattern.c_str());
        }
    }
    if (c.font_size != this->applied_size) {
        this->applied_size = c.font_size;
        rebuild = true;
    }
    if (rebuild) {
        this->font_sizes = depth_font_sizes(this->applied_size, n);
        this->atlases.clear();
    }
}


/* ------------------------------------------------------------------ */
/* keyboard                                                            */

void WlBackend::Impl::arm_repeat(uint32_t key) {
    if (this->repeat_timerfd < 0 || this->repeat_rate <= 0)
        return;
    struct itimerspec its = {};
    long interval_ns = 1000000000L / this->repeat_rate;
    its.it_interval.tv_sec = interval_ns / 1000000000L;
    its.it_interval.tv_nsec = interval_ns % 1000000000L;
    its.it_value.tv_sec = this->repeat_delay / 1000;
    its.it_value.tv_nsec = (long)(this->repeat_delay % 1000) * 1000000L;
    if (its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0)
        its.it_value = its.it_interval;
    timerfd_settime(this->repeat_timerfd, 0, &its, NULL);
    this->repeat_key = key;
    this->repeat_armed = true;
}


void WlBackend::Impl::disarm_repeat() {
    if (!this->repeat_armed)
        return;
    struct itimerspec its = {};
    timerfd_settime(this->repeat_timerfd, 0, &its, NULL);
    this->repeat_armed = false;
}


void WlBackend::Impl::handle_key_press(uint32_t key, bool from_repeat) {
    if (!this->xkb || !this->auth)
        return;

    xkb_keycode_t kc = key + 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(this->xkb, kc);
    char buf[32];
    explicit_bzero(&buf, sizeof(buf));
    int len = xkb_state_key_get_utf8(this->xkb, kc, buf, sizeof(buf));

    /* keypad remapping, as the X11 backend does */
    if (sym == XKB_KEY_KP_Enter) {
        sym = XKB_KEY_Return;
    } else if (sym >= XKB_KEY_KP_0 && sym <= XKB_KEY_KP_9) {
        buf[0] = (char)('0' + (sym - XKB_KEY_KP_0));
        buf[1] = '\0';
        len = 1;
        sym = XKB_KEY_0 + (sym - XKB_KEY_KP_0);
    }
    if (filtered_sym(sym)) {
        explicit_bzero(&buf, sizeof(buf));
        return;
    }

    Key k;
    switch (sym) {
    case XKB_KEY_Return:
        k = Key::Enter;
        break;
    case XKB_KEY_Escape:
        k = Key::Escape;
        break;
    case XKB_KEY_BackSpace:
        k = Key::Backspace;
        break;
    default:
        k = Key::Char;
        break;
    }

    this->auth->feed(k, buf, len);
    explicit_bzero(&buf, sizeof(buf));

    /* only character keys and Backspace repeat */
    if (!from_repeat) {
        if ((k == Key::Char || k == Key::Backspace) &&
            xkb_keymap_key_repeats(this->keymap, kc))
            this->arm_repeat(key);
        else
            this->disarm_repeat();
    }
}


static void keyboard_keymap(void* data, wl_keyboard*, uint32_t format,
                            int32_t fd, uint32_t size) {
    auto* impl = static_cast<WlBackend::Impl*>(data);

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    char* map_str = (char*)mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map_str == MAP_FAILED)
        return;

    xkb_keymap* keymap = xkb_keymap_new_from_string(impl->xkb_ctx, map_str,
                             XKB_KEYMAP_FORMAT_TEXT_V1,
                             XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map_str, size);
    if (!keymap)
        return;

    xkb_state* state = xkb_state_new(keymap);
    if (!state) {
        xkb_keymap_unref(keymap);
        return;
    }

    if (impl->xkb)
        xkb_state_unref(impl->xkb);
    if (impl->keymap)
        xkb_keymap_unref(impl->keymap);
    impl->keymap = keymap;
    impl->xkb = state;
}

static void keyboard_enter(void*, wl_keyboard*, uint32_t, wl_surface*,
                           wl_array*) {}

static void keyboard_leave(void* data, wl_keyboard*, uint32_t, wl_surface*) {
    static_cast<WlBackend::Impl*>(data)->disarm_repeat();
}

static void keyboard_key(void* data, wl_keyboard*, uint32_t, uint32_t,
                         uint32_t key, uint32_t state) {
    auto* impl = static_cast<WlBackend::Impl*>(data);
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        impl->handle_key_press(key, false);
    } else if (impl->repeat_armed && key == impl->repeat_key) {
        impl->disarm_repeat();
    }
}

static void keyboard_modifiers(void* data, wl_keyboard*, uint32_t,
                               uint32_t depressed, uint32_t latched,
                               uint32_t locked, uint32_t group) {
    auto* impl = static_cast<WlBackend::Impl*>(data);
    if (impl->xkb)
        xkb_state_update_mask(impl->xkb, depressed, latched, locked, 0, 0, group);
}

static void keyboard_repeat_info(void* data, wl_keyboard*, int32_t rate,
                                 int32_t delay) {
    auto* impl = static_cast<WlBackend::Impl*>(data);
    impl->repeat_rate = rate;
    impl->repeat_delay = delay;
}

static const wl_keyboard_listener keyboard_listener = {
    keyboard_keymap,
    keyboard_enter,
    keyboard_leave,
    keyboard_key,
    keyboard_modifiers,
    keyboard_repeat_info,
};


static void seat_capabilities(void* data, wl_seat* seat, uint32_t caps) {
    auto* impl = static_cast<WlBackend::Impl*>(data);
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !impl->keyboard) {
        impl->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(impl->keyboard, &keyboard_listener, impl);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && impl->keyboard) {
        wl_keyboard_release(impl->keyboard);
        impl->keyboard = nullptr;
    }
}

static void seat_name(void*, wl_seat*, const char*) {}

static const wl_seat_listener seat_listener = { seat_capabilities, seat_name };


/* ------------------------------------------------------------------ */
/* outputs                                                             */

static void lock_surface_configure(void* data, ext_session_lock_surface_v1* ls,
                                   uint32_t serial, uint32_t width,
                                   uint32_t height) {
    auto* out = static_cast<WlOutput*>(data);
    out->width = width;
    out->height = height;
    ext_session_lock_surface_v1_ack_configure(ls, serial);
    out->configured = true;
    out->impl->render_and_commit(out, out->impl->render_ready);
}

static const ext_session_lock_surface_v1_listener lock_surface_listener = {
    lock_surface_configure,
};


void WlBackend::Impl::create_lock_surface(WlOutput* out) {
    out->surface = wl_compositor_create_surface(this->compositor);
    out->lock_surface = ext_session_lock_v1_get_lock_surface(
        this->session_lock, out->surface, out->output);
    ext_session_lock_surface_v1_add_listener(out->lock_surface,
                                             &lock_surface_listener, out);
}


static void output_geometry(void*, wl_output*, int32_t, int32_t, int32_t,
                            int32_t, int32_t, const char*, const char*,
                            int32_t) {}

static void output_mode(void*, wl_output*, uint32_t, int32_t, int32_t,
                        int32_t) {}

static void output_done(void* data, wl_output*) {
    auto* out = static_cast<WlOutput*>(data);
    out->scale = out->pending_scale;
    out->done = true;
    /* hotplug: an output that appeared while locking or locked gets its
     * surface as soon as its state is complete */
    if (out->impl->session_lock && !out->impl->finished && !out->lock_surface)
        out->impl->create_lock_surface(out);
}

static void output_scale(void* data, wl_output*, int32_t factor) {
    static_cast<WlOutput*>(data)->pending_scale = factor;
}

static void output_name(void*, wl_output*, const char*) {}

static void output_description(void*, wl_output*, const char*) {}

static const wl_output_listener output_listener = {
    output_geometry,
    output_mode,
    output_done,
    output_scale,
    output_name,
    output_description,
};


void WlBackend::Impl::remove_output(uint32_t global_name) {
    for (auto it = this->outputs.begin(); it != this->outputs.end(); ++it) {
        if ((*it)->global_name == global_name) {
            this->outputs.erase(it);
            return;
        }
    }
}


/* ------------------------------------------------------------------ */
/* session lock                                                        */

static void lock_locked(void* data, ext_session_lock_v1*) {
    static_cast<WlBackend::Impl*>(data)->locked = true;
}

static void lock_finished(void* data, ext_session_lock_v1*) {
    static_cast<WlBackend::Impl*>(data)->finished = true;
}

static const ext_session_lock_v1_listener lock_listener = {
    lock_locked,
    lock_finished,
};


/* ------------------------------------------------------------------ */
/* registry                                                            */

static void shm_format(void*, wl_shm*, uint32_t) {}
static const wl_shm_listener shm_listener = { shm_format };

static void registry_global(void* data, wl_registry* registry, uint32_t name,
                            const char* interface, uint32_t version) {
    auto* impl = static_cast<WlBackend::Impl*>(data);

    if (!strcmp(interface, wl_compositor_interface.name)) {
        impl->compositor_version = std::min(version, 4u);
        impl->compositor = (wl_compositor*)wl_registry_bind(
            registry, name, &wl_compositor_interface, impl->compositor_version);
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        impl->shm = (wl_shm*)wl_registry_bind(registry, name,
                                              &wl_shm_interface, 1);
        wl_shm_add_listener(impl->shm, &shm_listener, impl);
    } else if (!strcmp(interface, wl_seat_interface.name)) {
        if (!impl->seat) {
            impl->seat = (wl_seat*)wl_registry_bind(
                registry, name, &wl_seat_interface, std::min(version, 4u));
            wl_seat_add_listener(impl->seat, &seat_listener, impl);
        }
    } else if (!strcmp(interface, wl_output_interface.name)) {
        auto out = std::make_unique<WlOutput>();
        out->impl = impl;
        out->global_name = name;
        out->version = std::min(version, 4u);
        out->output = (wl_output*)wl_registry_bind(
            registry, name, &wl_output_interface, out->version);
        wl_output_add_listener(out->output, &output_listener, out.get());
        if (out->version < 2)
            out->done = true;   // no done event before v2
        impl->outputs.push_back(std::move(out));
    } else if (!strcmp(interface, ext_session_lock_manager_v1_interface.name)) {
        impl->lock_manager = (ext_session_lock_manager_v1*)wl_registry_bind(
            registry, name, &ext_session_lock_manager_v1_interface, 1);
    }
}

static void registry_global_remove(void* data, wl_registry*, uint32_t name) {
    static_cast<WlBackend::Impl*>(data)->remove_output(name);
}

static const wl_registry_listener registry_listener = {
    registry_global,
    registry_global_remove,
};


/* ------------------------------------------------------------------ */
/* WlBackend                                                           */

WlBackend::WlBackend(Conf& conf)
    : impl_(std::make_unique<Impl>()) {
    Impl* impl = this->impl_.get();
    impl->conf = &conf;

    impl->display = wl_display_connect(NULL);
    if (!impl->display)
        Utils::die("%s: cannot connect to the wayland display\n", NAME);

    impl->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!impl->xkb_ctx)
        Utils::die("%s: cannot create xkb context\n", NAME);

    impl->repeat_timerfd = timerfd_create(CLOCK_MONOTONIC,
                                          TFD_NONBLOCK | TFD_CLOEXEC);
    if (impl->repeat_timerfd < 0)
        Utils::die("%s: timerfd_create: %s\n", NAME, strerror(errno));

    impl->registry = wl_display_get_registry(impl->display);
    wl_registry_add_listener(impl->registry, &registry_listener, impl);

    /* first roundtrip collects the globals, second the output events */
    wl_display_roundtrip(impl->display);
    if (!impl->compositor || !impl->shm)
        Utils::die("%s: compositor lacks wl_compositor or wl_shm\n", NAME);
    wl_display_roundtrip(impl->display);

    /* resolve the rain font now, while fontconfig still sees the invoking
     * user's configuration (privileges are dropped before run()) */
    const Config& c = conf.cfg();
    if (!impl->init_fonts(c.font_pattern.c_str()))
        Utils::die("%s: no usable font for pattern '%s'\n", NAME,
                   c.font_pattern.c_str());
    impl->applied_pattern = c.font_pattern;
    impl->applied_size = c.font_size;
    impl->applied_rain = rain_params(c);
    impl->font_sizes = depth_font_sizes(impl->applied_size,
                                        impl->applied_rain.depth_levels);
    impl->apply_config();
}


WlBackend::~WlBackend() {
    Impl* impl = this->impl_.get();

    impl->outputs.clear();
    if (impl->session_lock)
        ext_session_lock_v1_destroy(impl->session_lock);
    if (impl->lock_manager)
        ext_session_lock_manager_v1_destroy(impl->lock_manager);
    if (impl->keyboard)
        wl_keyboard_release(impl->keyboard);
    if (impl->seat)
        wl_seat_destroy(impl->seat);
    if (impl->shm)
        wl_shm_destroy(impl->shm);
    if (impl->compositor)
        wl_compositor_destroy(impl->compositor);
    if (impl->registry)
        wl_registry_destroy(impl->registry);
    if (impl->display)
        wl_display_disconnect(impl->display);

    if (impl->xkb)
        xkb_state_unref(impl->xkb);
    if (impl->keymap)
        xkb_keymap_unref(impl->keymap);
    if (impl->xkb_ctx)
        xkb_context_unref(impl->xkb_ctx);
    if (impl->repeat_timerfd >= 0)
        close(impl->repeat_timerfd);

    if (impl->face)
        FT_Done_Face(impl->face);
    if (impl->ft)
        FT_Done_FreeType(impl->ft);
}


int WlBackend::conn_fd() const {
    return wl_display_get_fd(this->impl_->display);
}


bool WlBackend::lock() {
    Impl* impl = this->impl_.get();

    if (!impl->lock_manager)
        Utils::die("%s: compositor does not support ext-session-lock-v1\n",
                   NAME);

    impl->session_lock = ext_session_lock_manager_v1_lock(impl->lock_manager);
    ext_session_lock_v1_add_listener(impl->session_lock, &lock_listener, impl);

    /* create the lock surfaces right away, before waiting for the locked
     * event: compositors hold that event back until every output has a
     * committed lock surface so they can switch without showing a blank
     * frame (niri falls back only after a ~1 s deadline). Late outputs are
     * handled by output_done. */
    for (auto& out : impl->outputs) {
        if (out->done && !out->lock_surface)
            impl->create_lock_surface(out.get());
    }

    /* configure events arrive and are painted while we wait */
    while (!impl->locked && !impl->finished) {
        if (wl_display_dispatch(impl->display) < 0)
            Utils::die("%s: wayland connection error\n", NAME);
    }
    if (impl->finished) {
        fprintf(stderr, "%s: the compositor denied the lock "
                "(is another locker running?)\n", NAME);
        return false;
    }

    /* deliver any remaining configure events */
    wl_display_roundtrip(impl->display);

    return true;
}


void WlBackend::run(Auth& auth) {
    Impl* impl = this->impl_.get();

    impl->auth = &auth;
    impl->apply_config();
    impl->render_ready = true;

    /* first rain frame on every output; keeps animating via frame callbacks */
    for (auto& out : impl->outputs)
        impl->render_and_commit(out.get(), true);

    struct pollfd fds[3] = {
        { wl_display_get_fd(impl->display), POLLIN, 0 },
        { impl->repeat_timerfd,             POLLIN, 0 },
        { impl->conf->watch_fd(),           POLLIN, 0 },   // -1 is ignored
    };

    while (!auth.unlocked()) {
        if (impl->finished)
            Utils::die("%s: session lock finished by the compositor\n", NAME);

        while (wl_display_prepare_read(impl->display) != 0) {
            if (wl_display_dispatch_pending(impl->display) < 0)
                Utils::die("%s: wayland connection error\n", NAME);
        }
        wl_display_flush(impl->display);

        if (poll(fds, 3, -1) < 0) {
            wl_display_cancel_read(impl->display);
            if (errno == EINTR)
                continue;
            Utils::die("%s: poll: %s\n", NAME, strerror(errno));
        }

        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(impl->display) < 0)
                Utils::die("%s: wayland connection error\n", NAME);
        } else {
            wl_display_cancel_read(impl->display);
        }
        if (wl_display_dispatch_pending(impl->display) < 0)
            Utils::die("%s: wayland connection error\n", NAME);

        if (fds[1].revents & POLLIN) {
            uint64_t expirations = 0;
            if (read(impl->repeat_timerfd, &expirations,
                     sizeof(expirations)) == sizeof(expirations) &&
                impl->repeat_armed) {
                if (expirations > 32)
                    expirations = 32;   // absorb stalls
                while (expirations-- && !auth.unlocked())
                    impl->handle_key_press(impl->repeat_key, true);
            }
        }

        if (fds[2].revents & POLLIN) {
            if (impl->conf->reload_if_changed())
                impl->apply_config();
        }
    }

    /* mandatory: unlock and let the compositor process it before exiting */
    ext_session_lock_v1_unlock_and_destroy(impl->session_lock);
    impl->session_lock = nullptr;
    wl_display_roundtrip(impl->display);
}
