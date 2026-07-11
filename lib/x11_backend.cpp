/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#include <cstdlib>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <algorithm>
#include <unistd.h>
#include <cstring>
#include <sys/time.h>
#include <sys/select.h>
#include <X11/extensions/Xrandr.h>

#include "../include/x11_backend.hpp"
#include "../include/utils.hpp"


X11Backend::X11Backend(Conf& conf) : conf_(conf) {
    /* the display server */
    this->disp = XOpenDisplay(NULL);

	if (!this->disp) {
        Utils::die("%s: cannot open display\n", NAME);
    }

    /* set the number of attached screens */
    this->num_screens = ScreenCount(this->disp);

    this->rr.active = XRRQueryExtension(this->disp, &this->rr.event, &this->rr.error);
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


void X11Backend::run(Auth& auth) {
    /**
     * Read password and handle it accordingly.
     */
    struct timeval last_update, current_time;
    unsigned int oldc = States::INIT;

    // Initialize matrix effect for each screen
    for (auto& mon : this->monitors) {
        mon->init_rain(this->disp, this->conf_.cfg());
    }

    gettimeofday(&last_update, NULL);
	XRRScreenChangeNotifyEvent* rre;
	char buf[32];
	int num;
	unsigned int colour;
	KeySym ksym;
	XEvent ev;
	int xfd = ConnectionNumber(this->disp);
	int wfd = this->conf_.watch_fd();
	fd_set fds;

	while (!auth.unlocked()) {
        // Apply configuration changes live
        if (this->conf_.reload_if_changed()) {
            const Config& cfg = this->conf_.cfg();
            auth.set_failonclear(cfg.failonclear);
            colour = auth.state();
            RainParams np = rain_params(cfg);
            for (auto& mon : this->monitors) {
                // structural changes restart the simulation
                if (!(mon->rain.p == np))
                    mon->rain.configure(np, (uint32_t)rand());
                mon->apply_config(this->disp, cfg, colour);
            }
            oldc = colour;
        }

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
			colour = auth.state();
			if (!auth.unlocked() && oldc != colour) {
				for (auto& mon : this->monitors) {
					XSetWindowBackground(this->disp, mon->win, mon->colours[colour]);
					XClearWindow(this->disp, mon->win);
				}
				oldc = colour;
			}
		} else if (this->rr.active && ev.type == this->rr.event + RRScreenChangeNotify) {
			rre = (XRRScreenChangeNotifyEvent*)&ev;
			for (auto& mon : this->monitors) {
				if (mon->win == rre->window) {
					if (rre->rotation == RR_Rotate_90 ||
					    rre->rotation == RR_Rotate_270)
						XResizeWindow(this->disp, mon->win,
						              rre->height, rre->width);
					else
						XResizeWindow(this->disp, mon->win,
						              rre->width, rre->height);
					XClearWindow(this->disp, mon->win);
					break;
				}
			}
		} else {
			for (auto& mon : this->monitors)
				XRaiseWindow(this->disp, mon->win);
		}
	}

	// Calculate time until next frame
	gettimeofday(&current_time, NULL);
	long elapsed = (current_time.tv_sec - last_update.tv_sec) * 1000000 +
	              (current_time.tv_usec - last_update.tv_usec);

	if (elapsed >= UPDATE_INTERVAL) {
	    for (auto& mon : this->monitors) {
	        int width = DisplayWidth(this->disp, mon->screen_num);
	        int height = DisplayHeight(this->disp, mon->screen_num);
	        mon->rain.step(width, height, this->conf_.cfg().mutate_chars);
	        mon->draw_rain(this->disp, oldc);
	    }
	    last_update = current_time;
	} else {
	    // Wait for X events, config changes, or the next frame via select()
	    long wait_us = UPDATE_INTERVAL - elapsed;
	    struct timeval timeout;
	    timeout.tv_sec = wait_us / 1000000;
	    timeout.tv_usec = wait_us % 1000000;
	    FD_ZERO(&fds);
	    FD_SET(xfd, &fds);
	    if (wfd >= 0)
	        FD_SET(wfd, &fds);
	    select(std::max(xfd, wfd) + 1, &fds, NULL, NULL, &timeout);
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

	for (i = 0; i < States::NUMSTATES; i++) {
		XAllocNamedColor(this->disp, DefaultColormap(this->disp, lock->screen_num),
		                 this->conf_.cfg().background.c_str(), &colour, &dummy);
		lock->colours[i] = colour.pixel;
	}

	/* init */
	wa.override_redirect = 1;
	wa.background_pixel = lock->colours[States::INIT];
	lock->win = XCreateWindow(this->disp, lock->parent, 0, 0,
	                          DisplayWidth(this->disp, lock->screen_num),
	                          DisplayHeight(this->disp, lock->screen_num),
	                          0, DefaultDepth(this->disp, lock->screen_num),
	                          CopyFromParent,
	                          DefaultVisual(this->disp, lock->screen_num),
	                          CWOverrideRedirect | CWBackPixel, &wa);
	lock->pixmap = XCreateBitmapFromData(this->disp, lock->win, curs, 8, 8);
	invisible = XCreatePixmapCursor(this->disp, lock->pixmap, lock->pixmap,
	                                &colour, &colour, 0, 0);
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
		fprintf(stderr, "%s: unable to grab mouse pointer for screen %d\n", NAME, screen_num);
	if (kbgrab != GrabSuccess)
		fprintf(stderr, "%s: unable to grab keyboard for screen %d\n", NAME, screen_num);
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


void Monitor::cleanup(Display* disp) {
    for (size_t d = 0; d < this->font.size(); d++) {
        if (this->font[d]) {
            XFreeFont(disp, this->font[d]);
            this->font[d] = nullptr;
        }
    }
    for (size_t d = 0; d < this->gc.size(); d++) {
        if (this->gc[d]) {
            XFreeGC(disp, this->gc[d]);
            this->gc[d] = 0;
        }
    }
    if (this->backbuffer) {
        XFreePixmap(disp, this->backbuffer);
        this->backbuffer = 0;
    }
    if (this->pixmap) {
        XFreePixmap(disp, this->pixmap);
        this->pixmap = 0;
    }
}


static unsigned long alloc_dimmed_colour(Display* disp, int screen,
        const char* hex, float alpha) {
    /**
     * Parse a "#RRGGBB" hex string, scale RGB by alpha, and allocate an X11
     * colour. On a black background, colour * alpha is equivalent to true
     * alpha blending.
     */
    unsigned int r, g, b;
    sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b);

    XColor xc;
    xc.red   = (unsigned short)(r * alpha) * 257;
    xc.green = (unsigned short)(g * alpha) * 257;
    xc.blue  = (unsigned short)(b * alpha) * 257;
    xc.flags = DoRed | DoGreen | DoBlue;

    XAllocColor(disp, DefaultColormap(disp, screen), &xc);
    return xc.pixel;
}


void Monitor::setup_rain_resources(Display* disp, const Config& cfg) {
    /**
     * (Re)load the per-depth fonts, GCs, metrics and state/depth colour
     * tables from the configuration. Safe to call again on hot reload.
     */

    XGCValues gcv;
    int n = cfg.depth_levels;

    // Font pixel sizes per depth: closer = larger, farther = smaller
    std::vector<int> sizes = depth_font_sizes(cfg.font_size, n);
    char font_name[128];

    // free resources beyond the new depth count before shrinking
    for (size_t d = n; d < this->font.size(); d++) {
        if (this->font[d])
            XFreeFont(disp, this->font[d]);
        if (this->gc[d])
            XFreeGC(disp, this->gc[d]);
    }
    this->font.resize(n, nullptr);
    this->gc.resize(n, 0);

    for (int d = 0; d < n; d++) {
        snprintf(font_name, sizeof(font_name),
            "-misc-fixed-medium-r-semicondensed--%d-*-*-*-c-*-iso8859-1",
            sizes[d]);
        XFontStruct* font = XLoadQueryFont(disp, font_name);
        if (!font)
            font = XLoadQueryFont(disp, "6x13");
        if (!font)
            Utils::die("%s: no usable font found\n", NAME);

        if (this->font[d])
            XFreeFont(disp, this->font[d]);
        this->font[d] = font;

        this->rain.metrics[d].char_width = font->max_bounds.width;
        this->rain.metrics[d].char_height = font->ascent + font->descent;

        gcv.font = font->fid;
        if (this->gc[d])
            XChangeGC(disp, this->gc[d], GCFont, &gcv);
        else
            this->gc[d] = XCreateGC(disp, this->win, GCFont, &gcv);
    }

    /* allocate depth-dimmed colour variants for each state */
    for (int st = 0; st < States::NUMSTATES; st++) {
        this->body_colour[st].resize(n);
        this->head_colour[st].resize(n);
        for (int d = 0; d < n; d++) {
            float a = sample_curve(DEPTH_ALPHA_CURVE, n, d);
            const char* hex = cfg.fontcolour[st].c_str();
            this->body_colour[st][d] = alloc_dimmed_colour(disp, this->screen_num, hex, a);
            this->head_colour[st][d] = alloc_dimmed_colour(disp, this->screen_num, hex,
                                                           std::min(1.0f, a * 1.3f));
        }
    }
}


void Monitor::init_rain(Display* disp, const Config& cfg) {
    /**
     * Initialise the setup to create the effect of characters raining like The
     * Matrix.
     */

    // size the simulation before the metrics are filled in
    this->rain.configure(rain_params(cfg), (uint32_t)rand());

    this->setup_rain_resources(disp, cfg);

    // Create backbuffer for double-buffering
    int width = DisplayWidth(disp, this->screen_num);
    int height = DisplayHeight(disp, this->screen_num);
    int depth = DefaultDepth(disp, this->screen_num);
    this->backbuffer = XCreatePixmap(disp, this->win, width, height, depth);
}


void Monitor::apply_config(Display* disp, const Config& cfg, int current_state) {
    /**
     * Apply a hot-reloaded configuration: window background, fonts, colours.
     */

    XColor colour, dummy;
    for (int i = 0; i < States::NUMSTATES; i++) {
        XAllocNamedColor(disp, DefaultColormap(disp, this->screen_num),
                         cfg.background.c_str(), &colour, &dummy);
        this->colours[i] = colour.pixel;
    }
    XSetWindowBackground(disp, this->win, this->colours[current_state]);
    XClearWindow(disp, this->win);

    this->setup_rain_resources(disp, cfg);
}


void Monitor::draw_rain(Display* disp, int current_state) {
    int width = DisplayWidth(disp, this->screen_num);
    int height = DisplayHeight(disp, this->screen_num);
    Pixmap dst = this->backbuffer;

    // Clear the backbuffer (use gc[0] for non-text operations)
    XSetForeground(disp, this->gc[0], this->colours[States::INIT]);
    XFillRectangle(disp, dst, this->gc[0], 0, 0, width, height);

    const std::vector<unsigned long>& body_colour = this->body_colour[current_state];
    const std::vector<unsigned long>& head_colour = this->head_colour[current_state];

    // Draw grouped by depth -- two XSetForeground calls per depth in total
    for (int d = 0; d < this->rain.p.depth_levels; d++) {
        GC dgc = this->gc[d];
        int dch = this->rain.metrics[d].char_height;

        // Body pass
        XSetForeground(disp, dgc, body_colour[d]);
        for (int ai = 0; ai < this->rain.active_count; ai++) {
            int i = this->rain.active_list[ai];
            struct Droplet& drop = this->rain.droplets[i];
            if (drop.depth != d) continue;

            const char* dc = this->rain.droplet_chars(i);
            for (int j = 1; j < drop.length; j++) {
                int y = drop.y - (j * dch);
                if (y < 0 || y > height) continue;
                XDrawString(disp, dst, dgc, drop.x, y, &dc[j], 1);
            }
        }

        // Head pass
        XSetForeground(disp, dgc, head_colour[d]);
        for (int ai = 0; ai < this->rain.active_count; ai++) {
            int i = this->rain.active_list[ai];
            struct Droplet& drop = this->rain.droplets[i];
            if (drop.depth != d) continue;

            int y = drop.y;
            if (y >= 0 && y <= height) {
                XDrawString(disp, dst, dgc, drop.x, y, this->rain.droplet_chars(i), 1);
            }
        }
    }

    // Flip backbuffer to window
    XCopyArea(disp, dst, this->win, this->gc[0], 0, 0, width, height, 0, 0);
    XFlush(disp);
}
