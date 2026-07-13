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
#include <memory>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/timerfd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "ext-session-lock-v1.h"

#include "../include/wl_backend.hpp"
#include "../include/rain.hpp"
#include "../include/render.hpp"
#include "../include/utils.hpp"


namespace {

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
    Renderer renderer;
    bool render_ready = false;          // renderer configured, run() started
    bool mutate_chars = false;
    std::vector<DirtyRect> damage;      // per-frame scratch

    /* pacing: a dedicated timerfd steps and redraws at the configured fps,
     * decoupled from the monitor refresh; the animation freezes after
     * idle_timeout seconds without input */
    int anim_timerfd = -1;
    int fps = 30;
    int idle_timeout = 120;
    bool hidpi = true;
    struct timespec last_input = {};
    bool paused = false;

    Auth* auth = nullptr;

    void create_lock_surface(WlOutput* out);
    void render_and_commit(WlOutput* out, int steps = 0);
    void apply_config();
    void handle_key_press(uint32_t key, bool from_repeat);
    void note_input();
    void arm_anim();
    void disarm_anim();
    void evict_unused_atlases();
    int output_scale(WlOutput* out) const;
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
    int last_scale = -1;                // scale the rain metrics came from

    /* incremental drawing: per-buffer ink state, and the ink of the last
     * committed frame (what the compositor currently shows), which is what
     * commit damage must be computed against under double buffering */
    RenderTarget targets[2];
    std::vector<DirtyRect> last_commit;

    void destroy_buffers() {
        for (int i = 0; i < 2; i++) {
            if (this->buffers[i]) {
                wl_buffer_destroy(this->buffers[i]);
                this->buffers[i] = nullptr;
            }
            this->busy[i] = false;
            this->targets[i].invalidate();
        }
        this->last_commit.clear();
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
        /* Fallback for kernels without memfd. The name must not be
         * predictable: with O_EXCL, any local process that squats a
         * predictable name keeps the locker from starting. */
        for (int attempt = 0; attempt < 16 && fd < 0; attempt++) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            char name[64];
            snprintf(name, sizeof(name), "/matlock-%d-%ld-%d", getpid(),
                     ts.tv_nsec, attempt);
            fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
            if (fd >= 0)
                shm_unlink(name);
            else if (errno != EEXIST)
                break;
        }
        if (fd < 0)
            Utils::die("%s: shm_open: %s\n", NAME, strerror(errno));
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

int WlBackend::Impl::output_scale(WlOutput* out) const {
    /* buffer scale actually used: the output's scale on compositor v3+,
     * unless hidpi rendering is disabled, in which case render at scale 1
     * and let the compositor upscale */
    return (this->compositor_version >= 3 && this->hidpi) ? out->scale : 1;
}


void WlBackend::Impl::render_and_commit(WlOutput* out, int steps) {
    /**
     * Advance the simulation `steps` ticks, draw into a free buffer, and
     * commit. Pacing and occlusion are driven purely by the shm buffer
     * release protocol, not by frame callbacks: a surface the compositor is
     * displaying releases its previous buffer promptly, so a free buffer is
     * available every tick; an output that is off (DPMS/blanked) stops
     * releasing, both buffers stay busy, and the output is left frozen with
     * no simulation step until it resumes. This cannot deadlock across a
     * display sleep, unlike a frame-callback gate whose callback the
     * compositor may never deliver.
     */

    if (!out->configured)
        return;

    int sc = this->output_scale(out);
    int bw = (int)out->width * sc;
    int bh = (int)out->height * sc;

    if (!ensure_buffers(this, out, bw, bh))
        return;

    int bi = !out->busy[0] ? 0 : (!out->busy[1] ? 1 : -1);
    if (bi < 0)
        return;                 // both buffers in flight: output off/occluded

    uint32_t* px = (uint32_t*)((char*)out->shm_data +
                               (size_t)bi * out->buf_w * 4 * out->buf_h);

    RenderTarget& tgt = out->targets[bi];
    if (this->render_ready) {
        if (!out->rain_ready) {
            out->rain.configure(this->renderer.rain_params(), (uint32_t)rand());
            out->rain_ready = true;
        }
        /* keep the simulation's metrics in step with the atlas at this scale
         * (droplet deactivation uses char_height); apply_config resets
         * last_scale so a hot reload refreshes them too */
        if (out->last_scale != sc) {
            out->rain.metrics = this->renderer.atlas_for(sc).metrics;
            out->last_scale = sc;
        }
        for (int s = 0; s < steps; s++)
            out->rain.step(out->buf_w, out->buf_h, this->mutate_chars);
        int state = this->auth ? this->auth->state() : States::INIT;
        this->renderer.draw(out->rain, px, out->buf_w, out->buf_h, sc, state,
                            tgt, this->damage);
    } else {
        std::fill_n(px, (size_t)out->buf_w * out->buf_h,
                    this->renderer.background());
        tgt.invalidate();
        tgt.content.clear();
        this->damage.assign(1, {0, 0, out->buf_w, out->buf_h});
    }

    if (this->compositor_version >= 3)
        wl_surface_set_buffer_scale(out->surface, sc);
    wl_surface_attach(out->surface, out->buffers[bi], 0, 0);

    /* The compositor shows the last committed frame (usually the other
     * buffer), so the commit damage is that frame's ink plus this one's,
     * not the erased-in-this-buffer set the renderer reports. A full clear
     * (or an excessive rect count) damages the whole buffer instead. */
    bool full = !this->damage.empty() && this->damage[0].x == 0 &&
                this->damage[0].y == 0 && this->damage[0].w == out->buf_w &&
                this->damage[0].h == out->buf_h;
    size_t nrects = out->last_commit.size() + tgt.content.size();
    if (full || nrects > 256) {
        wl_surface_damage_buffer(out->surface, 0, 0, out->buf_w, out->buf_h);
    } else {
        for (const DirtyRect& r : out->last_commit)
            wl_surface_damage_buffer(out->surface, r.x, r.y, r.w, r.h);
        for (const DirtyRect& r : tgt.content)
            wl_surface_damage_buffer(out->surface, r.x, r.y, r.w, r.h);
    }
    out->last_commit = tgt.content;

    out->busy[bi] = true;
    wl_surface_commit(out->surface);
}


void WlBackend::Impl::evict_unused_atlases() {
    std::vector<int> keep;
    for (auto& out : this->outputs)
        keep.push_back(this->output_scale(out.get()));
    this->renderer.evict_atlases(keep);
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

    bool fps_changed = (c.fps != this->fps);
    this->fps = c.fps;
    this->idle_timeout = c.idle_timeout;
    this->hidpi = c.hidpi;

    RainParams np = rain_params(c);
    bool structural = !(np == this->renderer.rain_params());

    this->renderer.configure(c);

    if (structural) {
        /* Restart the simulations with the new structural parameters, as the
         * renderer applied them: a droplet's characters are indices into the
         * charset the renderer resolved fonts for, so if it kept the old
         * charset (a font lookup that failed), the simulations must too. */
        for (auto& out : this->outputs)
            if (out->rain_ready)
                out->rain.configure(this->renderer.rain_params(),
                                    (uint32_t)rand());
    }

    /* the atlas may have been rebuilt at the same scale (font or size
     * change): have every output refresh its rain metrics next frame */
    for (auto& out : this->outputs)
        out->last_scale = -1;

    if (fps_changed && this->render_ready && !this->paused)
        this->arm_anim();
    this->evict_unused_atlases();
}


/* ------------------------------------------------------------------ */
/* pacing and idle                                                     */

void WlBackend::Impl::arm_anim() {
    if (this->anim_timerfd < 0 || this->fps <= 0)
        return;
    struct itimerspec its = {};
    long interval_ns = 1000000000L / this->fps;
    its.it_interval.tv_sec = interval_ns / 1000000000L;
    its.it_interval.tv_nsec = interval_ns % 1000000000L;
    its.it_value = its.it_interval;
    timerfd_settime(this->anim_timerfd, 0, &its, NULL);
}


void WlBackend::Impl::disarm_anim() {
    if (this->anim_timerfd < 0)
        return;
    struct itimerspec its = {};
    timerfd_settime(this->anim_timerfd, 0, &its, NULL);
}


void WlBackend::Impl::note_input() {
    /* record activity for the idle clock; wake and repaint if the animation
     * had been frozen (a key press also changes the state colour, which the
     * repaint reflects immediately) */
    clock_gettime(CLOCK_MONOTONIC, &this->last_input);
    if (this->paused) {
        this->paused = false;
        this->arm_anim();
        for (auto& out : this->outputs)
            this->render_and_commit(out.get());
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
    /* get_utf8 returns the untruncated length; clamp to what buf holds */
    if (len >= (int)sizeof(buf))
        len = (int)sizeof(buf) - 1;

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
        impl->note_input();
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
    out->impl->render_and_commit(out);
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
            /* a scale no other output uses can now be freed */
            if (this->render_ready)
                this->evict_unused_atlases();
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

    impl->anim_timerfd = timerfd_create(CLOCK_MONOTONIC,
                                        TFD_NONBLOCK | TFD_CLOEXEC);
    if (impl->anim_timerfd < 0)
        Utils::die("%s: timerfd_create: %s\n", NAME, strerror(errno));

    impl->registry = wl_display_get_registry(impl->display);
    wl_registry_add_listener(impl->registry, &registry_listener, impl);

    /* first roundtrip collects the globals, second the output events */
    wl_display_roundtrip(impl->display);
    if (!impl->compositor || !impl->shm)
        Utils::die("%s: compositor lacks wl_compositor or wl_shm\n", NAME);
    wl_display_roundtrip(impl->display);

    /* resolve the rain font and build the colour tables now, while fontconfig
     * still sees the invoking user's configuration (privileges are dropped
     * before run()) */
    const Config& c = conf.cfg();
    impl->fps = c.fps;
    impl->idle_timeout = c.idle_timeout;
    impl->hidpi = c.hidpi;
    impl->renderer.configure(c);
    if (!impl->renderer.has_face())
        Utils::die("%s: no usable font for pattern '%s'\n", NAME,
                   c.font_pattern.c_str());
    impl->mutate_chars = c.mutate_chars;
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
    if (impl->anim_timerfd >= 0)
        close(impl->anim_timerfd);
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

    /* start the animation clock and paint the first frame on every output */
    clock_gettime(CLOCK_MONOTONIC, &impl->last_input);
    impl->arm_anim();
    for (auto& out : impl->outputs)
        impl->render_and_commit(out.get());

    struct pollfd fds[4] = {
        { wl_display_get_fd(impl->display), POLLIN, 0 },
        { impl->repeat_timerfd,             POLLIN, 0 },
        { impl->conf->watch_fd(),           POLLIN, 0 },   // -1 is ignored
        { impl->anim_timerfd,               POLLIN, 0 },
    };

    while (!auth.unlocked()) {
        if (impl->finished)
            Utils::die("%s: session lock finished by the compositor\n", NAME);

        while (wl_display_prepare_read(impl->display) != 0) {
            if (wl_display_dispatch_pending(impl->display) < 0)
                Utils::die("%s: wayland connection error\n", NAME);
        }
        wl_display_flush(impl->display);

        if (poll(fds, 4, -1) < 0) {
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
                while (expirations-- && !auth.unlocked()) {
                    impl->note_input();
                    impl->handle_key_press(impl->repeat_key, true);
                }
            }
        }

        if (fds[2].revents & POLLIN) {
            if (impl->conf->reload_if_changed())
                impl->apply_config();
        }

        /* animation tick: step and redraw. render_and_commit skips an output
         * whose buffers are both in flight (off/occluded) and steps only when
         * it draws, so a blanked output freezes without drifting and resumes
         * cleanly when its buffers are released again */
        if (fds[3].revents & POLLIN) {
            uint64_t ticks = 0;
            if (read(impl->anim_timerfd, &ticks, sizeof(ticks)) ==
                    sizeof(ticks) && ticks > 0) {
                if (ticks > 5)
                    ticks = 5;          // drop the backlog after a stall
                for (auto& out : impl->outputs)
                    impl->render_and_commit(out.get(), (int)ticks);
            }
        }

        /* freeze the animation after idle_timeout seconds without input */
        if (impl->idle_timeout > 0 && !impl->paused) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (elapsed_us(impl->last_input, now) >=
                    (long)impl->idle_timeout * 1000000L) {
                impl->paused = true;
                impl->disarm_anim();
            }
        }
    }

    /* mandatory: unlock and let the compositor process it before exiting */
    ext_session_lock_v1_unlock_and_destroy(impl->session_lock);
    impl->session_lock = nullptr;
    wl_display_roundtrip(impl->display);
}
