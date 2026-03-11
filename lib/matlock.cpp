#include <cstdlib>
#include <cstdint>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <unistd.h>
#include <cstring>
#include <sys/time.h>
#include <sys/select.h>
#include <X11/extensions/Xrandr.h>

#include "../include/matlock.hpp"
#include "../include/utils.hpp"


Matlock::Matlock() {
    /* the display server */
    this->disp = XOpenDisplay(NULL);

	if (!this->disp) {
        Utils::die("%s: cannot open display\n", NAME);
    }

    /* set the number of attached screens */
    this->num_screens = ScreenCount(this->disp);

    this->rr.active = XRRQueryExtension(this->disp, &this->rr.event, &this->rr.error);
}


Matlock::~Matlock() {
    /**
     * Cleanup allocated memories and close the display server.
     */

    for (auto& mon : this->monitors)
        mon->cleanup(this->disp);
    this->monitors.clear();
    XCloseDisplay(this->disp);
}


void Matlock::usage(void) {
    /**
     * Print usage.
     */

	Utils::die("usage: %s [-v] [cmd [arg ...]]\n", NAME);
}


void Matlock::read_pwd(const char* hash, bool mutate_chars, int failonclear, const char* fontcolour[]) {
    /**
     * Read password and handle it accordingly.
     */
    struct timeval last_update, current_time;
    unsigned int oldc = Matlock::States::INIT;  // Make oldc accessible to update_matrix_effect

    // Initialize matrix effect for each screen
    for (auto& mon : this->monitors) {
        mon->init_rain(this->disp, fontcolour);
    }

    gettimeofday(&last_update, NULL);
	XRRScreenChangeNotifyEvent* rre;
	char buf[32], passwd[256], *inputhash;
	int num, running, failure;
	unsigned int len, colour;
	KeySym ksym;
	XEvent ev;
	int xfd = ConnectionNumber(this->disp);
	fd_set fds;

	len = 0;
	running = 1;
	failure = 0;
	oldc = Matlock::States::INIT;

	while (running) {
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
			switch (ksym) {
			case XK_Return:
				passwd[len] = '\0';
				errno = 0;
				if (!(inputhash = crypt(passwd, hash)))
					fprintf(stderr, "%s: crypt: %s\n", NAME, strerror(errno));
				else {
					size_t hlen = strlen(hash);
					running = strlen(inputhash) != hlen ||
					          Utils::timingsafe_bcmp(inputhash, hash, hlen);
				}
				if (running) {
					XBell(this->disp, 100);
					failure = 1;
				}
				explicit_bzero(&passwd, sizeof(passwd));
				len = 0;
				break;
			case XK_Escape:
				explicit_bzero(&passwd, sizeof(passwd));
				len = 0;
                failure = 0;
				break;
			case XK_BackSpace:
				if (len)
					passwd[--len] = '\0';
				break;
			default:
				if (num && !iscntrl((int)buf[0]) &&
				    (len + num < sizeof(passwd))) {
					memcpy(passwd + len, buf, num);
					len += num;
				}
				break;
			}
			colour = len ? Matlock::States::INPUT : ((failure || failonclear) ? Matlock::States::FAILED : Matlock::States::INIT);
			if (running && oldc != colour) {
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
	        mon->keep_raining(this->disp, oldc, mutate_chars);
	    }
	    last_update = current_time;
	} else {
	    // Wait for X events or next frame timeout via select()
	    long wait_us = UPDATE_INTERVAL - elapsed;
	    struct timeval timeout;
	    timeout.tv_sec = wait_us / 1000000;
	    timeout.tv_usec = wait_us % 1000000;
	    FD_ZERO(&fds);
	    FD_SET(xfd, &fds);
	    select(xfd + 1, &fds, NULL, NULL, &timeout);
	}
	}
}


std::unique_ptr<Monitor> Matlock::lock_screen(int screen_num, const char* background_colour) {
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

	for (i = 0; i < Matlock::States::NUMSTATES; i++) {
		XAllocNamedColor(this->disp, DefaultColormap(this->disp, lock->screen_num), background_colour, &colour, &dummy);
		lock->colours[i] = colour.pixel;
	}

	/* init */
	wa.override_redirect = 1;
	wa.background_pixel = lock->colours[Matlock::States::INIT];
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


int Matlock::lock_screens(const char* background_colour) {
	for (int s = 0; s < this->num_screens; s++) {
		auto mon = this->lock_screen(s, background_colour);
		if (mon)
			this->monitors.push_back(std::move(mon));
		else
			break;
	}

	XSync(this->disp, 0);

    return static_cast<int>(this->monitors.size());
}


void Monitor::cleanup(Display* disp) {
    for (int d = 0; d < DEPTH_LEVELS; d++) {
        if (this->rain.font[d]) {
            XFreeFont(disp, this->rain.font[d]);
            this->rain.font[d] = nullptr;
        }
        if (this->rain.gc[d]) {
            XFreeGC(disp, this->rain.gc[d]);
            this->rain.gc[d] = 0;
        }
    }
    if (this->rain.backbuffer) {
        XFreePixmap(disp, this->rain.backbuffer);
        this->rain.backbuffer = 0;
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


static const float depth_alpha[DEPTH_LEVELS] = {1.0f, 0.65f, 0.4f, 0.2f};

static inline uint32_t fast_rand(uint32_t& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s;
}


void Monitor::init_rain(Display* disp, const char* font_colour[]) {
    /**
     * Initialise the setup to create the effect of characters raining like The
     * Matrix.
     */

    XGCValues gcv;

    // Font pixel sizes per depth: closer = larger, farther = smaller
    static const int depth_font_size[DEPTH_LEVELS] = {20, 16, 13, 10};
    char font_name[128];

    for (int d = 0; d < DEPTH_LEVELS; d++) {
        snprintf(font_name, sizeof(font_name),
            "-misc-fixed-medium-r-semicondensed--%d-*-*-*-c-*-iso8859-1",
            depth_font_size[d]);
        this->rain.font[d] = XLoadQueryFont(disp, font_name);
        if (!this->rain.font[d])
            this->rain.font[d] = XLoadQueryFont(disp, "6x13");
        if (!this->rain.font[d])
            Utils::die("matlock: no usable font found\n");

        this->rain.char_width[d] = this->rain.font[d]->max_bounds.width;
        this->rain.char_height[d] = this->rain.font[d]->ascent + this->rain.font[d]->descent;

        gcv.font = this->rain.font[d]->fid;
        this->rain.gc[d] = XCreateGC(disp, this->win, GCFont, &gcv);
    }

    // Create backbuffer for double-buffering
    int width = DisplayWidth(disp, this->screen_num);
    int height = DisplayHeight(disp, this->screen_num);
    int depth = DefaultDepth(disp, this->screen_num);
    this->rain.backbuffer = XCreatePixmap(disp, this->win, width, height, depth);

    /* allocate depth-dimmed colour variants for each state */
    for (int d = 0; d < DEPTH_LEVELS; d++) {
        float a = depth_alpha[d];
        float ha = std::min(1.0f, a * 1.3f);

        this->rain.default_colour[d]      = alloc_dimmed_colour(disp, this->screen_num, font_colour[Matlock::States::INIT], a);
        this->rain.default_head_colour[d]  = alloc_dimmed_colour(disp, this->screen_num, font_colour[Matlock::States::INIT], ha);
        this->rain.input_colour[d]         = alloc_dimmed_colour(disp, this->screen_num, font_colour[Matlock::States::INPUT], a);
        this->rain.input_head_colour[d]    = alloc_dimmed_colour(disp, this->screen_num, font_colour[Matlock::States::INPUT], ha);
        this->rain.failed_colour[d]        = alloc_dimmed_colour(disp, this->screen_num, font_colour[Matlock::States::FAILED], a);
        this->rain.failed_head_colour[d]   = alloc_dimmed_colour(disp, this->screen_num, font_colour[Matlock::States::FAILED], ha);
    }

    // initialise droplets as a free-list
    this->rain.free_head = 0;
    this->rain.active_count = 0;
    this->rain.rng_state = (uint32_t)rand() | 1; // must be non-zero
    for (int i = 0; i < MAX_DROPLETS; i++) {
        this->rain.droplets[i].active = 0;
        this->rain.droplets[i].x = (i + 1 < MAX_DROPLETS) ? i + 1 : -1;
    }
}


void Monitor::keep_raining(Display* disp, int current_state, bool mutate_chars) {
    int width = DisplayWidth(disp, this->screen_num);
    int height = DisplayHeight(disp, this->screen_num);
    Pixmap dst = this->rain.backbuffer;

    // Clear the backbuffer (use gc[0] for non-text operations)
    XSetForeground(disp, this->rain.gc[0], this->colours[Matlock::States::INIT]);
    XFillRectangle(disp, dst, this->rain.gc[0], 0, 0, width, height);

    // Randomly create new droplets (spawn up to 2 per frame)
    for (int s = 0; s < NUM_THREADS_PER_FRAME; s++) {
        if (fast_rand(this->rain.rng_state) % 5 == 0)
            this->rain.rain_droplet(width, height);
    }

    // Select colour arrays based on state
    unsigned long* body_colour;
    unsigned long* head_colour;
    if (current_state == Matlock::States::INPUT) {
        body_colour = this->rain.input_colour;
        head_colour = this->rain.input_head_colour;
    } else if (current_state == Matlock::States::FAILED) {
        body_colour = this->rain.failed_colour;
        head_colour = this->rain.failed_head_colour;
    } else {
        body_colour = this->rain.default_colour;
        head_colour = this->rain.default_head_colour;
    }

    // Pass 1: Update positions, mutate chars, deactivate off-screen droplets
    for (int ai = 0; ai < this->rain.active_count; ai++) {
        int i = this->rain.active_list[ai];
        struct Droplet& drop = this->rain.droplets[i];
        int dch = this->rain.char_height[drop.depth];

        // Deactivate if the tail (topmost char) has scrolled past the bottom
        int tail_y = drop.y - ((drop.length - 1) * dch);
        if (tail_y > height) {
            drop.active = 0;
            drop.x = this->rain.free_head;
            this->rain.free_head = i;
            // Swap-remove from active list
            this->rain.active_list[ai] = this->rain.active_list[--this->rain.active_count];
            ai--;
            continue;
        }

        // Mutate characters
        if (mutate_chars) {
            uint32_t rnd = fast_rand(this->rain.rng_state);
            int bits_left = 6;
            for (int j = 0; j < drop.length; j++) {
                if ((rnd & 0x1F) < 2) {
                    drop.chars[j] = MATRIX_CHARS[fast_rand(this->rain.rng_state) % NUM_MATRIX_CHARS];
                }
                rnd >>= 5;
                if (--bits_left <= 0) {
                    rnd = fast_rand(this->rain.rng_state);
                    bits_left = 6;
                }
            }
        }

        // Move the droplet down
        drop.y += drop.speed;
    }

    // Pass 2: Draw grouped by depth — only 8 XSetForeground calls total
    for (int d = 0; d < DEPTH_LEVELS; d++) {
        GC dgc = this->rain.gc[d];
        int dch = this->rain.char_height[d];

        // Body pass
        XSetForeground(disp, dgc, body_colour[d]);
        for (int ai = 0; ai < this->rain.active_count; ai++) {
            int i = this->rain.active_list[ai];
            struct Droplet& drop = this->rain.droplets[i];
            if (drop.depth != d) continue;

            for (int j = 1; j < drop.length; j++) {
                int y = drop.y - (j * dch);
                if (y < 0 || y > height) continue;
                XDrawString(disp, dst, dgc, drop.x, y, &drop.chars[j], 1);
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
                XDrawString(disp, dst, dgc, drop.x, y, &drop.chars[0], 1);
            }
        }
    }

    // Flip backbuffer to window
    XCopyArea(disp, dst, this->win, this->rain.gc[0], 0, 0, width, height, 0, 0);
    XFlush(disp);
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
