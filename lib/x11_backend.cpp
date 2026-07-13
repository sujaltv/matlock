/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <unistd.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/shm.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/XShm.h>

#include "../include/x11_backend.hpp"
#include "../include/utils.hpp"


namespace {

bool host_little_endian() {
    uint32_t x = 1;
    return *(const char*)&x == 1;
}

int mask_shift(unsigned long mask) {
    int s = 0;
    if (!mask) return 0;
    while (!(mask & 1)) { mask >>= 1; s++; }
    return s;
}

long elapsed_us(const struct timeval& from, const struct timeval& to) {
    return (to.tv_sec - from.tv_sec) * 1000000L + (to.tv_usec - from.tv_usec);
}

/* MIT-SHM attach failures surface asynchronously as X errors; a scoped
 * handler flips this flag so the probe/create paths can fall back cleanly. */
bool shm_attach_failed = false;
int shm_error_handler(Display*, XErrorEvent*) {
    shm_attach_failed = true;
    return 0;
}

bool probe_shm(Display* disp) {
    /* the extension must be present and a real shared segment must attach
     * (it will not on a remote or restricted server) */
    if (!XShmQueryExtension(disp))
        return false;

    XShmSegmentInfo si = {};
    si.shmid = shmget(IPC_PRIVATE, 4, IPC_CREAT | 0600);
    if (si.shmid < 0)
        return false;
    si.shmaddr = (char*)shmat(si.shmid, NULL, 0);
    if (si.shmaddr == (char*)-1) {
        shmctl(si.shmid, IPC_RMID, NULL);
        return false;
    }
    si.readOnly = False;

    shm_attach_failed = false;
    XErrorHandler old = XSetErrorHandler(shm_error_handler);
    Bool attached = XShmAttach(disp, &si);
    XSync(disp, False);
    XSetErrorHandler(old);

    bool ok = attached && !shm_attach_failed;
    if (ok)
        XShmDetach(disp, &si);
    XSync(disp, False);
    shmdt(si.shmaddr);
    shmctl(si.shmid, IPC_RMID, NULL);
    return ok;
}

} // namespace


X11Backend::X11Backend(Conf& conf) : conf_(conf) {
    /* the display server */
    this->disp = XOpenDisplay(NULL);
    if (!this->disp)
        Utils::die("%s: cannot open display\n", NAME);

    /* set the number of attached screens */
    this->num_screens = ScreenCount(this->disp);

    this->rr.active = XRRQueryExtension(this->disp, &this->rr.event,
                                        &this->rr.error);

    this->shm_available = probe_shm(this->disp);
    if (this->shm_available)
        this->shm_event_base = XShmGetEventBase(this->disp);

    /* resolve the rain font and build the colour tables now, while
     * fontconfig still sees the invoking user's configuration (privileges
     * are dropped before run()) */
    const Config& c = conf.cfg();
    this->fps_ = c.fps;
    this->idle_timeout_ = c.idle_timeout;
    this->renderer.configure(c);
    if (!this->renderer.has_face())
        Utils::die("%s: no usable font for pattern '%s'\n", NAME,
                   c.font_pattern.c_str());
}


X11Backend::~X11Backend() {
    /**
     * Cleanup allocated memories and close the display server.
     */

    for (auto& mon : this->monitors)
        mon->cleanup(this->disp);
    this->monitors.clear();
    XCloseDisplay(this->disp);
}


int X11Backend::conn_fd() const {
    return ConnectionNumber(this->disp);
}


/* ------------------------------------------------------------------ */
/* Monitor presentation                                                */

void Monitor::destroy_image(Display* disp) {
    if (!this->image)
        return;
    if (this->shm_active) {
        XShmDetach(disp, &this->shminfo);
        XSync(disp, False);
        shmdt(this->shminfo.shmaddr);
        this->image->data = nullptr;    // shm memory, not Xlib's to free
        XDestroyImage(this->image);
    } else {
        XDestroyImage(this->image);     // frees the heap data too
    }
    this->image = nullptr;
    this->shm_active = false;
    this->busy = false;
    this->img_w = this->img_h = 0;
    memset(&this->shminfo, 0, sizeof(this->shminfo));
}


void Monitor::create_image(Display* disp, X11Backend* be, int w, int h) {
    /**
     * (Re)create the presentation image at w x h: a shared MIT-SHM image
     * when available, otherwise a heap XImage presented with XPutImage.
     */

    this->destroy_image(disp);

    Visual* vis = DefaultVisual(disp, this->screen_num);
    int depth = DefaultDepth(disp, this->screen_num);
    if (vis->c_class != TrueColor && vis->c_class != DirectColor)
        Utils::die("%s: unsupported X visual (not TrueColor)\n", NAME);

    if (be->shm_available) {
        this->image = XShmCreateImage(disp, vis, depth, ZPixmap, NULL,
                                      &this->shminfo, w, h);
        if (this->image && this->image->bits_per_pixel != 32) {
            int bpp = this->image->bits_per_pixel;
            XDestroyImage(this->image);
            this->image = nullptr;
            Utils::die("%s: unsupported pixel format (%d bpp, need 32)\n",
                       NAME, bpp);
        }
        if (this->image) {
            size_t size = (size_t)this->image->bytes_per_line *
                          this->image->height;
            this->shminfo.shmid = shmget(IPC_PRIVATE, size, IPC_CREAT | 0600);
            this->shminfo.shmaddr = this->shminfo.shmid >= 0
                ? (char*)shmat(this->shminfo.shmid, NULL, 0) : (char*)-1;
            if (this->shminfo.shmid < 0 || this->shminfo.shmaddr == (char*)-1) {
                if (this->shminfo.shmid >= 0)
                    shmctl(this->shminfo.shmid, IPC_RMID, NULL);
                XDestroyImage(this->image);
                this->image = nullptr;
            } else {
                this->image->data = this->shminfo.shmaddr;
                this->shminfo.readOnly = False;
                shm_attach_failed = false;
                XErrorHandler old = XSetErrorHandler(shm_error_handler);
                Bool attached = XShmAttach(disp, &this->shminfo);
                XSync(disp, False);
                XSetErrorHandler(old);
                if (attached && !shm_attach_failed) {
                    shmctl(this->shminfo.shmid, IPC_RMID, NULL);
                    this->shm_active = true;
                } else {
                    shmdt(this->shminfo.shmaddr);
                    shmctl(this->shminfo.shmid, IPC_RMID, NULL);
                    this->image->data = nullptr;
                    XDestroyImage(this->image);
                    this->image = nullptr;
                }
            }
        }
    }

    if (!this->image) {
        /* heap XImage fallback (also the path when MIT-SHM is unavailable) */
        char* data = (char*)malloc((size_t)w * 4 * h);
        if (!data)
            Utils::die("%s: out of memory for the frame buffer\n", NAME);
        this->image = XCreateImage(disp, vis, depth, ZPixmap, 0, data,
                                   w, h, 32, w * 4);
        if (!this->image)
            Utils::die("%s: XCreateImage failed\n", NAME);
        if (this->image->bits_per_pixel != 32)
            Utils::die("%s: unsupported pixel format (%d bpp, need 32)\n",
                       NAME, this->image->bits_per_pixel);
        this->shm_active = false;
    }

    /* we write native uint32 pixels, so the image byte order must match the
     * host; Xlib byte-swaps on transfer if the server differs */
    this->image->byte_order = host_little_endian() ? LSBFirst : MSBFirst;

    this->red_mask = this->image->red_mask;
    this->green_mask = this->image->green_mask;
    this->blue_mask = this->image->blue_mask;
    this->red_shift = mask_shift(this->red_mask);
    this->green_shift = mask_shift(this->green_mask);
    this->blue_shift = mask_shift(this->blue_mask);
    this->needs_remap = !(this->red_mask == 0xFF0000 &&
                          this->green_mask == 0xFF00 &&
                          this->blue_mask == 0xFF);

    this->img_w = w;
    this->img_h = h;
    this->busy = false;
    this->target.invalidate();      // fresh pixel storage: force a full clear

    if (!this->gc)
        this->gc = XCreateGC(disp, this->win, 0, NULL);
}


void Monitor::init(Display* disp, X11Backend* be) {
    /**
     * Size the simulation and create the presentation image for this monitor.
     * The simulation takes its parameters from the renderer rather than from
     * the configuration directly: a droplet's characters are indices into the
     * charset the renderer resolved fonts for, so the two must never disagree.
     */

    this->rain.configure(be->renderer.rain_params(), (uint32_t)rand());
    int width = DisplayWidth(disp, this->screen_num);
    int height = DisplayHeight(disp, this->screen_num);
    this->create_image(disp, be, width, height);
    this->rain.metrics = be->renderer.atlas_for(1).metrics;
}


void Monitor::draw(Display* disp, Renderer& renderer, int state) {
    /**
     * Render the current simulation state into the image and present it. A
     * shared image still being read by the server (busy) is left untouched;
     * the next tick redraws once the completion event clears the flag. As a
     * safety net, if a completion event is lost the busy flag is force-cleared
     * after 200 ms so a monitor can never freeze permanently.
     */

    if (!this->image)
        return;
    if (this->busy) {
        struct timeval now;
        gettimeofday(&now, NULL);
        long us = (now.tv_sec - this->busy_since.tv_sec) * 1000000L +
                  (now.tv_usec - this->busy_since.tv_usec);
        if (us < 200000)
            return;
        this->busy = false;         // completion apparently lost; recover
    }

    uint32_t* px = (uint32_t*)this->image->data;

    /* An off-layout visual takes the full-frame path every time: the remap
     * is not idempotent, so it cannot run over overlapping damage rects, and
     * such visuals are rare enough that incremental drawing is not worth a
     * remap-aware variant. */
    if (this->needs_remap)
        this->target.invalidate();

    renderer.draw(this->rain, px, this->img_w, this->img_h, 1, state,
                  this->target, this->damage);
    if (this->damage.empty())
        return;                     // blank frame after a blank frame

    if (this->needs_remap) {
        size_t n = (size_t)this->img_w * this->img_h;
        for (size_t i = 0; i < n; i++) {
            uint32_t p = px[i];
            uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
            px[i] = (uint32_t)(((unsigned long)r << this->red_shift) & this->red_mask) |
                    (uint32_t)(((unsigned long)g << this->green_shift) & this->green_mask) |
                    (uint32_t)(((unsigned long)b << this->blue_shift) & this->blue_mask);
        }
    }

    /* present only the bounding box of what changed */
    int x1 = this->img_w, y1 = this->img_h, x2 = 0, y2 = 0;
    for (const DirtyRect& r : this->damage) {
        x1 = std::min(x1, r.x);
        y1 = std::min(y1, r.y);
        x2 = std::max(x2, r.x + r.w);
        y2 = std::max(y2, r.y + r.h);
    }

    if (this->shm_active) {
        XShmPutImage(disp, this->win, this->gc, this->image, x1, y1, x1, y1,
                     x2 - x1, y2 - y1, True);
        this->busy = true;
        gettimeofday(&this->busy_since, NULL);
    } else {
        XPutImage(disp, this->win, this->gc, this->image, x1, y1, x1, y1,
                  x2 - x1, y2 - y1);
    }
    XFlush(disp);
}


void Monitor::cleanup(Display* disp) {
    this->destroy_image(disp);
    if (this->gc) {
        XFreeGC(disp, this->gc);
        this->gc = 0;
    }
    if (this->cursor_pixmap) {
        XFreePixmap(disp, this->cursor_pixmap);
        this->cursor_pixmap = 0;
    }
}


/* ------------------------------------------------------------------ */
/* input and pacing                                                    */

void X11Backend::note_input(int state) {
    gettimeofday(&this->last_input_, NULL);
    if (this->paused_) {
        this->paused_ = false;
        for (auto& mon : this->monitors)
            mon->draw(this->disp, this->renderer, state);
    }
}


void X11Backend::run(Auth& auth) {
    /**
     * Read the password and drive the animation: step and redraw at the
     * configured fps, hot-reload the configuration, and freeze after
     * idle_timeout seconds of inactivity until a key resumes it.
     */

    struct timeval last_update, current_time;

    for (auto& mon : this->monitors)
        mon->init(this->disp, this);

    int tick_us = 1000000 / this->fps_;

    gettimeofday(&last_update, NULL);
    this->last_input_ = last_update;
    this->paused_ = false;

    /* first frame */
    for (auto& mon : this->monitors)
        mon->draw(this->disp, this->renderer, auth.state());

    XRRScreenChangeNotifyEvent* rre;
    char buf[32];
    int num;
    KeySym ksym;
    XEvent ev;
    int xfd = ConnectionNumber(this->disp);
    int wfd = this->conf_.watch_fd();
    fd_set fds;
    bool conf_ready = true;         // check once at startup, then on wakeups

    while (!auth.unlocked()) {
        // Apply configuration changes live
        if (conf_ready && this->conf_.reload_if_changed()) {
            const Config& cfg = this->conf_.cfg();
            auth.set_failonclear(cfg.failonclear);
            this->fps_ = cfg.fps;
            this->idle_timeout_ = cfg.idle_timeout;
            tick_us = 1000000 / this->fps_;

            RainParams np = rain_params(cfg);
            bool structural = !(np == this->renderer.rain_params());
            this->renderer.configure(cfg);
            for (auto& mon : this->monitors) {
                if (structural)
                    mon->rain.configure(this->renderer.rain_params(),
                                        (uint32_t)rand());
                mon->rain.metrics = this->renderer.atlas_for(1).metrics;
            }
            if (!this->paused_) {
                for (auto& mon : this->monitors)
                    mon->draw(this->disp, this->renderer, auth.state());
            }
        }
        conf_ready = false;

        // Process all pending X events
        while (XPending(this->disp)) {
            XNextEvent(this->disp, &ev);
            if (ev.type == KeyPress) {
                explicit_bzero(&buf, sizeof(buf));
                num = XLookupString(&ev.xkey, buf, sizeof(buf), &ksym, 0);
                if (IsKeypadKey(ksym)) {
                    if (ksym == XK_KP_Enter)
                        ksym = XK_Return;
                    else if (ksym >= XK_KP_0 && ksym <= XK_KP_9)
                        ksym = (ksym - XK_KP_0) + XK_0;
                }
                if (IsFunctionKey(ksym) ||
                    IsKeypadKey(ksym) ||
                    IsMiscFunctionKey(ksym) ||
                    IsPFKey(ksym) ||
                    IsPrivateKeypadKey(ksym))
                    continue;
                Key k;
                switch (ksym) {
                case XK_Return:
                    k = Key::Enter;
                    break;
                case XK_Escape:
                    k = Key::Escape;
                    break;
                case XK_BackSpace:
                    k = Key::Backspace;
                    break;
                default:
                    k = Key::Char;
                    break;
                }
                auth.feed(k, buf, num);
                explicit_bzero(&buf, sizeof(buf));
                if (k == Key::Enter && !auth.unlocked())
                    XBell(this->disp, 100);
                this->note_input(auth.state());
            } else if (this->rr.active &&
                       ev.type == this->rr.event + RRScreenChangeNotify) {
                rre = (XRRScreenChangeNotifyEvent*)&ev;
                for (auto& mon : this->monitors) {
                    if (mon->win == rre->window) {
                        int nw, nh;
                        if (rre->rotation == RR_Rotate_90 ||
                            rre->rotation == RR_Rotate_270) {
                            nw = rre->height; nh = rre->width;
                        } else {
                            nw = rre->width; nh = rre->height;
                        }
                        XResizeWindow(this->disp, mon->win, nw, nh);
                        mon->create_image(this->disp, this, nw, nh);
                        mon->draw(this->disp, this->renderer, auth.state());
                        break;
                    }
                }
            } else if (this->shm_available &&
                       ev.type == this->shm_event_base + ShmCompletion) {
                /* match on the segment, not the window: a completion for a
                 * segment that was recreated (RandR resize) carries the old
                 * shmseg and must not clear the busy flag of its replacement */
                auto* ce = (XShmCompletionEvent*)&ev;
                for (auto& mon : this->monitors)
                    if (mon->shm_active && mon->shminfo.shmseg == ce->shmseg)
                        mon->busy = false;
            } else if (ev.type == MotionNotify || ev.type == ButtonPress ||
                       ev.type == ButtonRelease) {
                /* pointer activity also counts as input, so the idle pause
                 * (when enabled) resumes on a mouse move, not just a key */
                this->note_input(auth.state());
            } else {
                for (auto& mon : this->monitors)
                    XRaiseWindow(this->disp, mon->win);
            }
        }

        // Advance and redraw at the configured rate, unless idle-paused
        gettimeofday(&current_time, NULL);
        long elapsed = elapsed_us(last_update, current_time);

        if (!this->paused_ && elapsed >= tick_us) {
            bool mutate = this->conf_.cfg().mutate_chars;
            int state = auth.state();
            for (auto& mon : this->monitors) {
                int width = DisplayWidth(this->disp, mon->screen_num);
                int height = DisplayHeight(this->disp, mon->screen_num);
                mon->rain.step(width, height, mutate);
                mon->draw(this->disp, this->renderer, state);
            }
            last_update = current_time;
        } else {
            // Wait for X events, config changes, or the next frame
            struct timeval timeout;
            struct timeval* to = nullptr;
            if (!this->paused_) {
                long wait_us = tick_us - elapsed;
                if (wait_us < 0) wait_us = 0;
                timeout.tv_sec = wait_us / 1000000;
                timeout.tv_usec = wait_us % 1000000;
                to = &timeout;
            }
            FD_ZERO(&fds);
            FD_SET(xfd, &fds);
            if (wfd >= 0)
                FD_SET(wfd, &fds);
            if (select(std::max(xfd, wfd) + 1, &fds, NULL, NULL, to) > 0)
                conf_ready = wfd >= 0 && FD_ISSET(wfd, &fds);
        }

        // Freeze the animation after idle_timeout seconds without input
        if (this->idle_timeout_ > 0 && !this->paused_) {
            gettimeofday(&current_time, NULL);
            if (elapsed_us(this->last_input_, current_time) >=
                    (long)this->idle_timeout_ * 1000000L)
                this->paused_ = true;
        }
    }
}


std::unique_ptr<Monitor> X11Backend::lock_screen(int screen_num) {
    char curs[] = {0, 0, 0, 0, 0, 0, 0, 0};
    int i, ptgrab, kbgrab;
    XColor colour, dummy;
    XSetWindowAttributes wa;
    Cursor invisible;

    if (this->disp == NULL || screen_num < 0)
        return nullptr;

    auto lock = std::make_unique<Monitor>();

    lock->screen_num = screen_num;
    lock->parent = RootWindow(this->disp, lock->screen_num);

    XAllocNamedColor(this->disp, DefaultColormap(this->disp, lock->screen_num),
                     this->conf_.cfg().background.c_str(), &colour, &dummy);
    lock->bg_pixel = colour.pixel;

    /* init */
    wa.override_redirect = 1;
    wa.background_pixel = lock->bg_pixel;
    lock->win = XCreateWindow(this->disp, lock->parent, 0, 0,
                              DisplayWidth(this->disp, lock->screen_num),
                              DisplayHeight(this->disp, lock->screen_num),
                              0, DefaultDepth(this->disp, lock->screen_num),
                              CopyFromParent,
                              DefaultVisual(this->disp, lock->screen_num),
                              CWOverrideRedirect | CWBackPixel, &wa);
    lock->cursor_pixmap = XCreateBitmapFromData(this->disp, lock->win, curs, 8, 8);
    invisible = XCreatePixmapCursor(this->disp, lock->cursor_pixmap,
                                    lock->cursor_pixmap, &colour, &colour, 0, 0);
    XDefineCursor(this->disp, lock->win, invisible);

    /* Try to grab mouse pointer *and* keyboard for 600ms, else fail the monitor */
    for (i = 0, ptgrab = kbgrab = -1; i < 6; i++) {
        if (ptgrab != GrabSuccess) {
            ptgrab = XGrabPointer(this->disp, lock->parent, False,
                                  ButtonPressMask | ButtonReleaseMask |
                                  PointerMotionMask, GrabModeAsync,
                                  GrabModeAsync, None, invisible, CurrentTime);
        }
        if (kbgrab != GrabSuccess) {
            kbgrab = XGrabKeyboard(this->disp, lock->parent, True,
                                   GrabModeAsync, GrabModeAsync, CurrentTime);
        }

        /* input is grabbed: we can lock the screen */
        if (ptgrab == GrabSuccess && kbgrab == GrabSuccess) {
            XMapRaised(this->disp, lock->win);
            if (this->rr.active)
                XRRSelectInput(this->disp, lock->win, RRScreenChangeNotifyMask);

            XSelectInput(this->disp, lock->parent, SubstructureNotifyMask);
            return lock;
        }

        /* retry on AlreadyGrabbed but fail on other errors */
        if ((ptgrab != AlreadyGrabbed && ptgrab != GrabSuccess) ||
            (kbgrab != AlreadyGrabbed && kbgrab != GrabSuccess))
            break;

        usleep(100000);
    }

    /* we couldn't grab all input: fail out */
    if (ptgrab != GrabSuccess)
        fprintf(stderr, "%s: unable to grab mouse pointer for screen %d\n",
                NAME, screen_num);
    if (kbgrab != GrabSuccess)
        fprintf(stderr, "%s: unable to grab keyboard for screen %d\n",
                NAME, screen_num);
    return nullptr;
}


bool X11Backend::lock() {
    for (int s = 0; s < this->num_screens; s++) {
        auto mon = this->lock_screen(s);
        if (mon)
            this->monitors.push_back(std::move(mon));
        else
            break;
    }

    XSync(this->disp, 0);

    return static_cast<int>(this->monitors.size()) == this->num_screens;
}
