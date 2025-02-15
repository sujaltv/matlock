#include <stdlib.h>
#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
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

    /* allocate memory for the monitors */
    this->monitors = (struct Monitor**) calloc(this->num_screens, sizeof(struct Monitor*));
    if (!this->monitors) {
        Utils::die("%s: out of memory\n", NAME);
    }

    this->rr.active = XRRQueryExtension(this->disp, &this->rr.event, &this->rr.error);
}


Matlock::~Matlock() {
    /**
     * Cleanup allocated memories and close the display server.
     */

    free(this->monitors);
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
    for (int screen = 0; screen < this->num_screens; screen++) {
        this->monitors[screen]->init_rain(this->disp, fontcolour);
    }

    gettimeofday(&last_update, NULL);
	XRRScreenChangeNotifyEvent* rre;
	char buf[32], passwd[256], *inputhash;
	int num, screen, running, failure;
	unsigned int len, colour;
	KeySym ksym;
	XEvent ev;

	len = 0;
	running = 1;
	failure = 0;
	oldc = Matlock::States::INIT;

	while (running) {
        // Check for X events
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
				else
					running = !!strcmp(inputhash, hash);
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
				for (screen = 0; screen < this->num_screens; screen++) {
					XSetWindowBackground(this->disp, this->monitors[screen]->win, this->monitors[screen]->colours[colour]);
					XClearWindow(this->disp, this->monitors[screen]->win);
				}
				oldc = colour;
			}
		} else if (this->rr.active && ev.type == this->rr.event + RRScreenChangeNotify) {
			rre = (XRRScreenChangeNotifyEvent*)&ev;
			for (screen = 0; screen < this->num_screens; screen++) {
				if (this->monitors[screen]->win == rre->window) {
					if (rre->rotation == RR_Rotate_90 ||
					    rre->rotation == RR_Rotate_270)
						XResizeWindow(this->disp, this->monitors[screen]->win,
						              rre->height, rre->width);
					else
						XResizeWindow(this->disp, this->monitors[screen]->win,
						              rre->width, rre->height);
					XClearWindow(this->disp, this->monitors[screen]->win);
					break;
				}
			}
		} else {
			for (screen = 0; screen < this->num_screens; screen++)
				XRaiseWindow(this->disp, this->monitors[screen]->win);
		}
	}
	
	// Update matrix effect
	gettimeofday(&current_time, NULL);
	long elapsed = (current_time.tv_sec - last_update.tv_sec) * 1000000 +
	              (current_time.tv_usec - last_update.tv_usec);

	if (elapsed >= UPDATE_INTERVAL) {
	    for (int screen = 0; screen < this->num_screens; screen++) {
	        this->monitors[screen]->keep_raining(this->disp, oldc, mutate_chars);
	    }
	    last_update = current_time;
	} else {
	    usleep(1000); // Small sleep to prevent CPU hogging
	}
	}
}


struct Monitor* Matlock::lock_screen(int screen_num, const char* background_colour) {
	char curs[] = {0, 0, 0, 0, 0, 0, 0, 0};
	int i, ptgrab, kbgrab;
	struct Monitor* lock;
	XColor colour, dummy;
	XSetWindowAttributes wa;
	Cursor invisible;

	if (this->disp == NULL || screen_num < 0 || !(lock = (struct Monitor*) malloc(sizeof(struct Monitor))))
		return NULL;

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
	return NULL;
}


int Matlock::lock_screens(const char* background_colour) {
    int n_locks, s;

	for (n_locks = 0, s = 0; s < this->num_screens; s++) {
		if ((this->monitors[s] = this->lock_screen(s, background_colour)) != NULL)
			n_locks++;
		else
			break;
	}

	XSync(this->disp, 0);

    return n_locks;
}


void Monitor::init_rain(Display* disp, const char* font_colour[]) {
    /**
     * Initialise the setup to create the effect of characters raining like The
     * Matrix.
     */

    XGCValues gcv;
    XColor colour, buffer;

    this->rain.font = XLoadQueryFont(disp, "9x13");
    if (!this->rain.font) {
        this->rain.font = XLoadQueryFont(disp, "6x13");
    }

    this->rain.char_width = this->rain.font->max_bounds.width;
    this->rain.char_height = this->rain.font->ascent + this->rain.font->descent;

    gcv.font = this->rain.font->fid;
    this->rain.gc = XCreateGC(disp, this->win, GCFont, &gcv);

    auto assign_colour = [&](unsigned long* dest, const char* fc) {
        XAllocNamedColor(disp, DefaultColormap(disp, this->screen_num), fc,
                &colour, &buffer);
        *dest = colour.pixel;
    };

    /* character and the head character colours */
    assign_colour(&this->rain.default_colour, font_colour[Matlock::States::INIT]);
    assign_colour(&this->rain.default_head_colour, font_colour[Matlock::States::INIT]);
    assign_colour(&this->rain.input_colour, font_colour[Matlock::States::INPUT]);
    assign_colour(&this->rain.input_head_colour, font_colour[Matlock::States::INPUT]);
    assign_colour(&this->rain.failed_colour, font_colour[Matlock::States::FAILED]);
    assign_colour(&this->rain.failed_head_colour, font_colour[Matlock::States::FAILED]);

    // initialise droplets
    for (int i = 0; i < MAX_DROPLETS; i++) {
        this->rain.droplets[i].active = 0;
    }
}


void Monitor::keep_raining(Display* disp, int current_state, bool mutate_chars) {
    int width = DisplayWidth(disp, this->screen_num);
    int height = DisplayHeight(disp, this->screen_num);

    // Clear the window
    XSetForeground(disp, this->rain.gc, this->colours[Matlock::States::INIT]);
    XFillRectangle(disp, this->win, this->rain.gc, 0, 0, width, height);

    // Randomly create new droplets
    if (rand() % 7 == 0) {
        this->rain.rain_droplet(width, height);
    }

    // Update and draw each droplet
    for (int i = 0; i < MAX_DROPLETS; i++) {
        if (this->rain.droplets[i].active) {
            // Draw the droplet
            for (int j = 0; j < this->rain.droplets[i].length; j++) {
                int y = this->rain.droplets[i].y - (j * this->rain.char_height);

                if (y < 0) continue;
                if (y > height) continue;

                // Only deactivate droplet when the top character (last in the droplet) passes bottom
                if (j == this->rain.droplets[i].length - 1 && y > height) {
                    this->rain.droplets[i].active = 0;
                    break;
                }

                // Choose colours based on current state
                if (current_state == Matlock::States::INPUT) {
                    if (j == 0) {
                        XSetForeground(disp, this->rain.gc, this->rain.input_head_colour);
                    } else {
                        XSetForeground(disp, this->rain.gc, this->rain.input_colour);
                    }
                } else if (current_state == Matlock::States::FAILED) {
                    if (j == 0) {
                        XSetForeground(disp, this->rain.gc, this->rain.failed_head_colour);
                    } else {
                        XSetForeground(disp, this->rain.gc, this->rain.failed_colour);
                    }
                } else {
                    if (j == 0) {
                        XSetForeground(disp, this->rain.gc, this->rain.default_head_colour);
                    } else {
                        XSetForeground(disp, this->rain.gc, this->rain.default_colour);
                    }
                }

                char str[2] = {this->rain.droplets[i].chars[j], '\0'};
                XDrawString(disp, this->win, this->rain.gc, this->rain.droplets[i].x, y, str, 1);

                if (mutate_chars) {
                    // Randomly change characters inside a droplet
                    if (rand() % 20 == 0) {
                        this->rain.droplets[i].chars[j] = MATRIX_CHARS[rand() % NUM_MATRIX_CHARS];
                    }
                }
            }

            // Move the droplet down
            this->rain.droplets[i].y += this->rain.droplets[i].speed;
        }
    }

    XFlush(disp);
}


void Rain::rain_droplet(int width, int height) {
    /**
     * Create a droplet (a stream of characters) and rain it down the screen.
     */

    // First try to find an inactive droplet
    for (int i = 0; i < MAX_DROPLETS; i++) {
        if (!this->droplets[i].active) {
            this->droplets[i].x = rand() % width;
            this->droplets[i].y = 0;
            this->droplets[i].speed = 1 + (rand() % 3);
            this->droplets[i].length = DROPLET_LENGTH - (rand() % (DROPLET_LENGTH/2));
            this->droplets[i].active = 1;

            // Fill with random characters
            for (int j = 0; j < this->droplets[i].length; j++) {
                this->droplets[i].chars[j] = MATRIX_CHARS[rand() % NUM_MATRIX_CHARS];
            }
            return;
        }
    }

    // If no inactive droplet found, find the one that's furthest down and recycle it
    int furthest_droplet = 0;
    int max_y = this->droplets[0].y;

    for (int i = 1; i < MAX_DROPLETS; i++) {
        if (this->droplets[i].y > max_y) {
            max_y = this->droplets[i].y;
            furthest_droplet = i;
        }
    }

    // Recycle the furthest droplet
    this->droplets[furthest_droplet].x = rand() % width;
    this->droplets[furthest_droplet].y = 0;
    this->droplets[furthest_droplet].speed = 1 + (rand() % 3);
    this->droplets[furthest_droplet].length = DROPLET_LENGTH - (rand() % (DROPLET_LENGTH/2));
    this->droplets[furthest_droplet].active = 1;

    // Fill with new random characters
    for (int j = 0; j < this->droplets[furthest_droplet].length; j++) {
        this->droplets[furthest_droplet].chars[j] = MATRIX_CHARS[rand() % NUM_MATRIX_CHARS];
    }
}
