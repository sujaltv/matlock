/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#ifndef MATLOCK_HPP__
#define MATLOCK_HPP__

/* The Matrix-like rain-related constants */
#define MAX_DROPLETS 1000
#define NUM_THREADS_PER_FRAME 2
#define DROPLET_LENGTH 50
#define MATRIX_CHARS "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789~`!@#$%^&*()_+-=[]{}\\;:'\",<.>/?"
#define NUM_MATRIX_CHARS 69
#define UPDATE_INTERVAL 10042
#define DEPTH_LEVELS 4

#include <cstdint>
#include <X11/Xlib.h>
#include <vector>
#include <memory>


/* forward declaration */
struct Monitor;


struct Droplet {
    int active;                         // Whether this stream is currently active
    int depth;                          // Depth level (0 = closest, DEPTH_LEVELS-1 = farthest)
    int x;                              // x position
    int y;                              // y position
    int speed;                          // How many pixels to move per update
    int length;                         // Actual length of this stream
    char chars[DROPLET_LENGTH];         // Characters in this stream (cold, accessed only during draw)
};


struct Rain {
    public:
        struct Droplet droplets[MAX_DROPLETS];
        int active_list[MAX_DROPLETS];      // Indices of active droplets
        int active_count;                   // Number of active droplets
        int free_head;                      // Head of free-list (-1 = empty)
        uint32_t rng_state;                 // xorshift32 PRNG state
        XFontStruct* font[DEPTH_LEVELS];
        int char_width[DEPTH_LEVELS];
        int char_height[DEPTH_LEVELS];
        GC gc[DEPTH_LEVELS];
        Pixmap backbuffer;
        unsigned long default_colour[DEPTH_LEVELS];
        unsigned long default_head_colour[DEPTH_LEVELS];
        unsigned long input_colour[DEPTH_LEVELS];
        unsigned long input_head_colour[DEPTH_LEVELS];
        unsigned long failed_colour[DEPTH_LEVELS];
        unsigned long failed_head_colour[DEPTH_LEVELS];

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
        std::vector<std::unique_ptr<Monitor>> monitors;

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
        std::unique_ptr<Monitor> lock_screen(int, const char*);
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

        /* free X11 resources */
        void cleanup(Display*);
};


#endif /* MATLOCK_HPP__ */
