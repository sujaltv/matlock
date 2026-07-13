/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#ifndef RENDER_HPP__
#define RENDER_HPP__

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "auth.hpp"
#include "conf.hpp"
#include "rain.hpp"


/* an 8-bit coverage bitmap for one rasterised character (X11's Xrender.h
 * already claims the name Glyph, hence RainGlyph) */
struct RainGlyph {
    int w = 0, h = 0;
    int left = 0, top = 0;              // bearings relative to the baseline
    std::vector<uint8_t> cov;
};

/* the charset rasterised at every depth size, for one buffer scale */
struct Atlas {
    std::vector<DepthMetrics> metrics;               // sized depth_levels
    std::vector<std::vector<RainGlyph>> glyphs;      // [depth][charset index]
};


/* one ink rectangle in a target buffer, in buffer pixels */
struct DirtyRect {
    int x = 0, y = 0, w = 0, h = 0;
};


/* Per-destination-buffer state for incremental drawing: the ink currently
 * in the buffer, and what it was drawn with. The renderer erases exactly
 * these rectangles instead of clearing the whole frame; a generation or
 * size mismatch (reconfiguration, resize, fresh buffer) forces a full
 * clear. Owners must invalidate() whenever the pixel storage is replaced. */
struct RenderTarget {
    std::vector<DirtyRect> content;
    int gen = -1;
    int w = 0, h = 0;

    void invalidate() { this->gen = -1; }
};


/* Shared software renderer for both backends: resolves the font, rasterises
 * the charset into per-scale atlases, precomputes coverage->pixel lookup
 * tables per (state, depth, head/body), and draws a full frame into a
 * caller-owned 0xAARRGGBB pixel buffer. It holds no backend (X11/Wayland)
 * types, so both backends render pixel-identically at the same scale. */
class Renderer {
    public:
        Renderer() = default;
        ~Renderer();

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        /* (Re)compute everything derived from the configuration: fonts, sizes,
         * colours, coverage LUTs and the structural rain parameters. Safe to
         * call again on hot reload; the next drawn frame picks it up. Must be
         * called before the privilege drop, while fontconfig still sees the
         * invoking user's configuration. */
        void configure(const Config& cfg);

        /* whether a usable face has been resolved */
        bool has_face() const { return !this->faces_.empty(); }

        const RainParams& rain_params() const { return this->applied_rain_; }
        uint32_t background() const { return this->background_; }

        /* atlas for an integer buffer scale (built lazily) */
        const Atlas& atlas_for(int scale);

        /* drop cached atlases for buffer scales not present in `keep` */
        void evict_atlases(const std::vector<int>& keep);

        /* Draw the rain for `state` into `px` (`w`x`h` pixels at buffer
         * scale `scale`), erasing only the previous frame's ink recorded in
         * `tgt` (full clear when `tgt` is invalid). `damage` receives the
         * changed rectangles (erased plus drawn; a single full-frame rect on
         * a full clear), for the presentation path to forward. */
        void draw(const Rain& rain, uint32_t* px, int w, int h,
                  int scale, int state, RenderTarget& tgt,
                  std::vector<DirtyRect>& damage);

    private:
        void rebuild_luts(const Config& cfg);

        /* Resolve `pattern` to a primary FreeType face and, for every charset
         * codepoint that face does not cover, ask fontconfig for a fallback
         * face that does; the faces are replaced only if the primary resolves,
         * so a bad hot-reloaded pattern never kills the locker. */
        bool resolve_fonts(const char* pattern,
                           const std::vector<char32_t>& charset);

        FT_Library ft_ = nullptr;
        std::vector<FT_Face> faces_;                // faces_[0] is the primary
        std::vector<int> charset_face_;             // [charset index] -> face,
                                                    // -1 if no face covers it
        std::string applied_pattern_;               // pattern the faces came from
        int applied_size_ = 0;
        RainParams applied_rain_;
        uint32_t background_ = 0xFF000000;
        std::vector<int> font_sizes_;               // per-depth pixel sizes
        std::map<int, Atlas> atlases_;              // keyed by integer scale
        int gen_ = 0;                               // bumped by configure()

        /* per-frame scratch, kept to avoid reallocation: active droplets
         * bucketed by depth, and the ink rect of each active droplet */
        std::vector<int> bucket_start_;
        std::vector<int> bucket_pos_;
        std::vector<int> order_;
        std::vector<DirtyRect> drop_rects_;

        /* coverage-to-pixel tables: [state] holds one 256-entry table per
         * depth, blended over the opaque background colour */
        std::vector<std::array<uint32_t, 256>> body_lut_[States::NUMSTATES];
        std::vector<std::array<uint32_t, 256>> head_lut_[States::NUMSTATES];
};


#endif /* RENDER_HPP__ */
