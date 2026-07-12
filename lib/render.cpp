/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <fontconfig/fontconfig.h>

#include "../include/render.hpp"
#include "../include/utils.hpp"


namespace {

uint32_t parse_hex(const char* hex) {
    /* parse a "#RRGGBB" string into 0x00RRGGBB */
    unsigned int r = 0, g = 0, b = 0;
    sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b);
    return (r << 16) | (g << 8) | b;
}

uint32_t dim(uint32_t rgb, float alpha) {
    /* scale each channel; on an opaque background this equals alpha blending */
    uint32_t r = (uint32_t)(((rgb >> 16) & 0xFF) * alpha);
    uint32_t g = (uint32_t)(((rgb >> 8) & 0xFF) * alpha);
    uint32_t b = (uint32_t)((rgb & 0xFF) * alpha);
    return (r << 16) | (g << 8) | b;
}

void build_lut(std::array<uint32_t, 256>& lut, uint32_t bg, uint32_t fg) {
    /* coverage -> opaque pixel, foreground blended over the solid background.
     * This is the exact integer blend the old per-pixel path used, but the
     * destination is always the known background colour, so it precomputes
     * to a table: dst = lut[coverage]. */
    int br = (bg >> 16) & 0xFF, bgc = (bg >> 8) & 0xFF, bb = bg & 0xFF;
    int fr = (fg >> 16) & 0xFF, fgc = (fg >> 8) & 0xFF, fb = fg & 0xFF;
    for (int cov = 0; cov < 256; cov++) {
        int r = br + (fr - br) * cov / 255;
        int g = bgc + (fgc - bgc) * cov / 255;
        int b = bb + (fb - bb) * cov / 255;
        lut[cov] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) |
                   (uint32_t)b;
    }
}

void blit_glyph(uint32_t* px, int bw, int bh, const RainGlyph& g,
                int ox, int oy, const uint32_t* lut) {
    /* (ox, oy) is the baseline origin, as with XDrawString. Clipping is
     * hoisted out of the pixel loop: the visible glyph rectangle is computed
     * once, then the inner loops run unclipped with a single well-predicted
     * coverage branch, no arithmetic and no divides. */
    int x0 = ox + g.left;
    int y0 = oy - g.top;

    int rs = 0, re = g.h;
    if (y0 < 0) rs = -y0;
    if (y0 + g.h > bh) re = bh - y0;
    int cs = 0, ce = g.w;
    if (x0 < 0) cs = -x0;
    if (x0 + g.w > bw) ce = bw - x0;
    if (rs >= re || cs >= ce)
        return;

    for (int row = rs; row < re; row++) {
        const uint8_t* src = &g.cov[(size_t)row * g.w];
        uint32_t* dst = px + (size_t)(y0 + row) * bw + x0;
        for (int cx = cs; cx < ce; cx++) {
            uint8_t cov = src[cx];
            if (cov)
                dst[cx] = lut[cov];
        }
    }
}

} // namespace


Renderer::~Renderer() {
    if (this->face_)
        FT_Done_Face(this->face_);
    if (this->ft_)
        FT_Done_FreeType(this->ft_);
}


bool Renderer::init_fonts(const char* pattern) {
    /**
     * Resolve a fontconfig pattern to a font file and load its face,
     * replacing the current one. fontconfig is initialised for the lookup
     * and torn down again afterwards (FreeType reads the file itself and
     * needs no fontconfig), keeping no fontconfig caches resident.
     */

    if (!FcInit()) {
        fprintf(stderr, "%s: fontconfig init failed\n", NAME);
        return false;
    }

    FcPattern* pat = FcNameParse((const FcChar8*)pattern);
    if (!pat) {
        fprintf(stderr, "%s: cannot parse font pattern '%s'\n", NAME, pattern);
        FcFini();
        return false;
    }
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcPattern* match = FcFontMatch(NULL, pat, &res);
    FcPatternDestroy(pat);
    if (!match) {
        fprintf(stderr, "%s: no font matches '%s'\n", NAME, pattern);
        FcFini();
        return false;
    }

    FcChar8* file = NULL;
    if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch) {
        fprintf(stderr, "%s: matched font has no file\n", NAME);
        FcPatternDestroy(match);
        FcFini();
        return false;
    }

    if (!this->ft_ && FT_Init_FreeType(&this->ft_)) {
        fprintf(stderr, "%s: FT_Init_FreeType failed\n", NAME);
        FcPatternDestroy(match);
        FcFini();
        return false;
    }
    FT_Face nface = nullptr;
    if (FT_New_Face(this->ft_, (const char*)file, 0, &nface)) {
        fprintf(stderr, "%s: FT_New_Face %s failed\n", NAME, (const char*)file);
        FcPatternDestroy(match);
        FcFini();
        return false;
    }
    FcPatternDestroy(match);
    FcFini();

    if (this->face_)
        FT_Done_Face(this->face_);
    this->face_ = nface;
    return true;
}


void Renderer::rebuild_luts(const Config& cfg) {
    int n = this->applied_rain_.depth_levels;
    for (int st = 0; st < States::NUMSTATES; st++) {
        this->body_lut_[st].assign(n, {});
        this->head_lut_[st].assign(n, {});
        uint32_t rgb = parse_hex(cfg.fontcolour[st].c_str());
        for (int d = 0; d < n; d++) {
            float a = sample_curve(DEPTH_ALPHA_CURVE, n, d);
            build_lut(this->body_lut_[st][d], this->background_, dim(rgb, a));
            build_lut(this->head_lut_[st][d], this->background_,
                      dim(rgb, std::min(1.0f, a * 1.3f)));
        }
    }
}


void Renderer::configure(const Config& cfg) {
    bool rebuild = false;

    RainParams np = ::rain_params(cfg);
    if (!(np == this->applied_rain_)) {
        this->applied_rain_ = np;
        rebuild = true;
    }

    if (cfg.font_pattern != this->applied_pattern_ || !this->face_) {
        if (this->init_fonts(cfg.font_pattern.c_str())) {
            this->applied_pattern_ = cfg.font_pattern;
            rebuild = true;
        } else if (this->face_) {
            fprintf(stderr, "%s: keeping font '%s'\n", NAME,
                    this->applied_pattern_.c_str());
        }
    }
    if (cfg.font_size != this->applied_size_) {
        this->applied_size_ = cfg.font_size;
        rebuild = true;
    }

    this->background_ = 0xFF000000u | parse_hex(cfg.background.c_str());
    this->rebuild_luts(cfg);

    if (rebuild) {
        this->font_sizes_ = depth_font_sizes(this->applied_size_,
                                             this->applied_rain_.depth_levels);
        this->atlases_.clear();
    }
}


const Atlas& Renderer::atlas_for(int scale) {
    auto it = this->atlases_.find(scale);
    if (it != this->atlases_.end())
        return it->second;

    int n = this->applied_rain_.depth_levels;
    Atlas& atlas = this->atlases_[scale];
    atlas.metrics.resize(n);
    atlas.glyphs.resize(n);
    for (int d = 0; d < n; d++) {
        if (FT_Set_Pixel_Sizes(this->face_, 0, this->font_sizes_[d] * scale))
            Utils::die("%s: FT_Set_Pixel_Sizes failed\n", NAME);

        /* the face is monospaced: use a reference glyph for the advance */
        if (FT_Load_Char(this->face_, 'M', FT_LOAD_RENDER))
            Utils::die("%s: FT_Load_Char failed\n", NAME);
        atlas.metrics[d].char_width = (int)(this->face_->glyph->advance.x >> 6);
        atlas.metrics[d].char_height =
            (int)((this->face_->size->metrics.ascender -
                   this->face_->size->metrics.descender) >> 6);

        for (unsigned char ch : this->applied_rain_.charset) {
            if (FT_Load_Char(this->face_, ch, FT_LOAD_RENDER))
                continue;
            const FT_Bitmap& bm = this->face_->glyph->bitmap;
            RainGlyph& g = atlas.glyphs[d][ch & 0x7F];
            g.w = (int)bm.width;
            g.h = (int)bm.rows;
            g.left = this->face_->glyph->bitmap_left;
            g.top = this->face_->glyph->bitmap_top;
            g.cov.resize((size_t)g.w * g.h);
            for (int row = 0; row < g.h; row++)
                memcpy(&g.cov[(size_t)row * g.w],
                       bm.buffer + (size_t)row * bm.pitch, g.w);
        }
    }
    return atlas;
}


void Renderer::evict_atlases(const std::vector<int>& keep) {
    for (auto it = this->atlases_.begin(); it != this->atlases_.end();) {
        if (std::find(keep.begin(), keep.end(), it->first) == keep.end())
            it = this->atlases_.erase(it);
        else
            ++it;
    }
}


void Renderer::draw(const Rain& rain, uint32_t* px, int w, int h,
                    int scale, int state) {
    /**
     * Clear to the background, then draw a body pass and a head pass per
     * depth (near depths last so they overlay far ones), matching the layer
     * ordering both backends used before unification.
     */

    const Atlas& atlas = this->atlas_for(scale);
    std::fill_n(px, (size_t)w * h, this->background_);

    for (int d = 0; d < rain.p.depth_levels; d++) {
        int dch = atlas.metrics[d].char_height;

        const uint32_t* body = this->body_lut_[state][d].data();
        for (int ai = 0; ai < rain.active_count; ai++) {
            int i = rain.active_list[ai];
            const Droplet& drop = rain.droplets[i];
            if (drop.depth != d) continue;
            const char* dc = rain.droplet_chars(i);
            for (int j = 1; j < drop.length; j++) {
                int y = drop.y - (j * dch);
                if (y < 0 || y > h) continue;
                blit_glyph(px, w, h,
                           atlas.glyphs[d][(unsigned char)dc[j] & 0x7F],
                           drop.x, y, body);
            }
        }

        const uint32_t* head = this->head_lut_[state][d].data();
        for (int ai = 0; ai < rain.active_count; ai++) {
            int i = rain.active_list[ai];
            const Droplet& drop = rain.droplets[i];
            if (drop.depth != d) continue;
            if (drop.y >= 0 && drop.y <= h) {
                blit_glyph(px, w, h,
                           atlas.glyphs[d][(unsigned char)rain.droplet_chars(i)[0] & 0x7F],
                           drop.x, drop.y, head);
            }
        }
    }
}
