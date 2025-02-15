/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#ifndef MATLOCK_HPP__
#define MATLOCK_HPP__

/* The Matrix-like rain-related constants */
#define MAX_DROPLETS 1000
#define DROPLET_LENGTH 50
#define MATRIX_CHARS "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789~`!@#$%^&*()_+-=[]{}\\;:'\",<.>/?"
#define NUM_MATRIX_CHARS 69
#define UPDATE_INTERVAL 10042

#include <X11/Xlib.h>


/* forward declaration */
struct Monitor;


struct Droplet {
    int x;                              // x position
    int y;                              // y position
    int speed;                          // How many pixels to move per update
    char chars[DROPLET_LENGTH];         // Characters in this stream
    int length;                         // Actual length of this stream
    int active;                         // Whether this stream is currently active
};


struct Rain {
    public:
        struct Droplet droplets[MAX_DROPLETS];
        XFontStruct* font;
        int char_width;
        int char_height;
        GC gc;
        unsigned long default_colour;       // Main green color
        unsigned long default_head_colour;  // Bright head color
        unsigned long input_colour;         // Blue color for typing
        unsigned long input_head_colour;    // Bright blue for typing head
        unsigned long failed_colour;        // Red color for failed
        unsigned long failed_head_colour;   // Bright red for failed head

        void rain_droplet(int, int);
};


struct XRRBaseCodes {
	int active;
	int event;
	int error;
};


class Matlock {
    public:
        /* possible states (plus number of states) in which matlock can be */
        struct States {
            enum {
                INIT,
                INPUT,
                FAILED,
                NUMSTATES
            };
        };

        /* pointer to the display server */
        Display* disp;

        /* number of screens */
        int num_screens;

        /* attached monitors */
        struct Monitor** monitors;

        /* XRandr base codes */
        struct XRRBaseCodes rr;

        /* initialiser */
        Matlock();

        /* destructor */
        ~Matlock();

        /* print command usage */
        static void usage();

        /* read password */
        void read_pwd(const char*, bool, int, const char*[]);

        int lock_screens(const char*);

    private:
        struct Monitor* lock_screen(int, const char*);
};


struct Monitor {
    public:
        /* serial number of this monitor */
        int screen_num;

        /* parent window */
        Window parent;

        /* window */
        Window win;

        /* pixmap */
        Pixmap pixmap;

        /* possible colours for the monitor */
        unsigned long colours[Matlock::States::NUMSTATES];

        /* rain */
        struct Rain rain;

        /* initialise rain for this monitor */
        void init_rain(Display*, const char*[]);

        /* keep raining */
        void keep_raining(Display*, int, bool);
};


#endif /* MATLOCK_HPP__ */
