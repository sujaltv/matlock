/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#ifndef X11_BACKEND_HPP__
#define X11_BACKEND_HPP__

#include <X11/Xlib.h>
#include <memory>
#include <vector>

#include "auth.hpp"
#include "backend.hpp"
#include "conf.hpp"
#include "rain.hpp"


struct XRRBaseCodes {
    int active;
    int event;
    int error;
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
        unsigned long colours[States::NUMSTATES];

        /* rain simulation state */
        struct Rain rain;

        /* X11 rendering resources for the rain */
        XFontStruct* font[DEPTH_LEVELS];
        GC gc[DEPTH_LEVELS];
        Pixmap backbuffer;
        unsigned long body_colour[States::NUMSTATES][DEPTH_LEVELS];
        unsigned long head_colour[States::NUMSTATES][DEPTH_LEVELS];

        /* initialise rain resources for this monitor */
        void init_rain(Display*, const Config&);

        /* re-apply a hot-reloaded configuration */
        void apply_config(Display*, const Config&, int current_state);

        /* draw the current simulation state to the window */
        void draw_rain(Display*, int);

        /* free X11 resources */
        void cleanup(Display*);

    private:
        /* (re)load fonts, GCs, metrics and colour tables from the config */
        void setup_rain_resources(Display*, const Config&);
};


class X11Backend : public Backend {
    public:
        /* pointer to the display server */
        Display* disp;

        /* number of screens */
        int num_screens;

        /* attached monitors */
        std::vector<std::unique_ptr<Monitor>> monitors;

        /* XRandr base codes */
        struct XRRBaseCodes rr;

        explicit X11Backend(Conf& conf);
        ~X11Backend() override;

        bool lock() override;
        int conn_fd() const override;
        void run(Auth& auth) override;

    private:
        Conf& conf_;

        std::unique_ptr<Monitor> lock_screen(int);
};


#endif /* X11_BACKEND_HPP__ */
