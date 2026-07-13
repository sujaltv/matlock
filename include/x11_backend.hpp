/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#ifndef X11_BACKEND_HPP__
#define X11_BACKEND_HPP__

#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
#include <memory>
#include <sys/time.h>
#include <vector>

#include "auth.hpp"
#include "backend.hpp"
#include "conf.hpp"
#include "rain.hpp"
#include "render.hpp"


struct XRRBaseCodes {
    int active;
    int event;
    int error;
};


class X11Backend;


struct Monitor {
    public:
        /* serial number of this monitor */
        int screen_num = 0;

        /* parent (root) window and the lock window */
        Window parent = 0;
        Window win = 0;

        /* invisible cursor bitmap */
        Pixmap cursor_pixmap = 0;

        /* window background pixel (shown before the first frame) */
        unsigned long bg_pixel = 0;

        /* rain simulation state */
        struct Rain rain;

        /* presentation: a client-side image (MIT-SHM when available, else a
         * heap XImage) blitted straight to the window, plus a plain GC */
        GC gc = 0;
        XImage* image = nullptr;
        XShmSegmentInfo shminfo = {};
        bool shm_active = false;            // this image is a shared segment
        bool busy = false;                  // awaiting ShmCompletion
        struct timeval busy_since = {};     // when the in-flight put started
        int img_w = 0, img_h = 0;

        /* channel packing for the window's visual; a remap pass runs only
         * when it differs from the usual 0xFF0000/0xFF00/0xFF layout */
        bool needs_remap = false;
        int red_shift = 16, green_shift = 8, blue_shift = 0;
        unsigned long red_mask = 0xFF0000, green_mask = 0xFF00, blue_mask = 0xFF;

        /* incremental drawing state for the image, plus damage scratch */
        RenderTarget target;
        std::vector<DirtyRect> damage;

        /* configure the rain and create the presentation image */
        void init(Display*, X11Backend*);

        /* (re)create the image at the given pixel size */
        void create_image(Display*, X11Backend*, int w, int h);

        /* render the current simulation state and present it */
        void draw(Display*, Renderer&, int state);

        /* free the presentation image */
        void destroy_image(Display*);

        /* free all X11 resources */
        void cleanup(Display*);
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

        /* shared software renderer (font, colours, atlases) */
        Renderer renderer;

        /* MIT-SHM availability and its completion-event base */
        bool shm_available = false;
        int shm_event_base = 0;

        explicit X11Backend(Conf& conf);
        ~X11Backend() override;

        bool lock() override;
        int conn_fd() const override;
        void run(Auth& auth) override;

    private:
        Conf& conf_;

        /* pacing and idle state */
        int fps_ = 30;
        int idle_timeout_ = 120;
        bool paused_ = false;
        struct timeval last_input_ = {};

        std::unique_ptr<Monitor> lock_screen(int);
        void note_input(int state);
};


#endif /* X11_BACKEND_HPP__ */
