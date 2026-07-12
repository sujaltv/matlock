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
    std::vector<DepthMetrics> metrics;              // sized depth_levels
    std::vector<std::array<RainGlyph, 128>> glyphs; // [depth][ASCII char]
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

        /* Resolve a fontconfig pattern to a FreeType face, replacing the
         * current one; keeps the old face and returns false on failure so a
         * bad hot-reloaded pattern never kills the locker. Must be called
         * (via configure) before the privilege drop, while fontconfig still
         * sees the invoking user's configuration. */
        bool init_fonts(const char* pattern);

        /* (Re)compute everything derived from the configuration: font, sizes,
         * colours, coverage LUTs and the structural rain parameters. Safe to
         * call again on hot reload; the next drawn frame picks it up. */
        void configure(const Config& cfg);

        /* whether a usable face has been resolved */
        bool has_face() const { return this->face_ != nullptr; }

        const RainParams& rain_params() const { return this->applied_rain_; }
        uint32_t background() const { return this->background_; }

        /* atlas for an integer buffer scale (built lazily) */
        const Atlas& atlas_for(int scale);

        /* drop cached atlases for buffer scales not present in `keep` */
        void evict_atlases(const std::vector<int>& keep);

        /* clear `px` to the background and draw the rain for `state`; the
         * buffer is `w`x`h` pixels rendered at buffer scale `scale` */
        void draw(const Rain& rain, uint32_t* px, int w, int h,
                  int scale, int state);

    private:
        void rebuild_luts(const Config& cfg);

        FT_Library ft_ = nullptr;
        FT_Face face_ = nullptr;
        std::string applied_pattern_;               // pattern the face came from
        int applied_size_ = 0;
        RainParams applied_rain_;
        uint32_t background_ = 0xFF000000;
        std::vector<int> font_sizes_;               // per-depth pixel sizes
        std::map<int, Atlas> atlases_;              // keyed by integer scale

        /* coverage-to-pixel tables: [state] holds one 256-entry table per
         * depth, blended over the opaque background colour */
        std::vector<std::array<uint32_t, 256>> body_lut_[States::NUMSTATES];
        std::vector<std::array<uint32_t, 256>> head_lut_[States::NUMSTATES];
};


#endif /* RENDER_HPP__ */
