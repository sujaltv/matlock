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

void rect_union(DirtyRect& a, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0)
        return;
    if (a.w <= 0) {
        a = {x, y, w, h};
        return;
    }
    int x1 = std::min(a.x, x), y1 = std::min(a.y, y);
    int x2 = std::max(a.x + a.w, x + w), y2 = std::max(a.y + a.h, y + h);
    a = {x1, y1, x2 - x1, y2 - y1};
}

void blit_glyph(uint32_t* px, int bw, int bh, const RainGlyph& g,
                int ox, int oy, const uint32_t* lut, DirtyRect& ink) {
    /* (ox, oy) is the baseline origin, as with XDrawString. Clipping is
     * hoisted out of the pixel loop: the visible glyph rectangle is computed
     * once, then the inner loops run unclipped with a single well-predicted
     * coverage branch, no arithmetic and no divides. The clipped rectangle
     * is accumulated into `ink` for the dirty-region tracking. */
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

    rect_union(ink, x0 + cs, y0 + rs, ce - cs, re - rs);

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

/* how many faces (the primary plus fallbacks) one charset may draw from */
constexpr size_t MAX_FACES = 8;

} // namespace


Renderer::~Renderer() {
    for (FT_Face f : this->faces_)
        FT_Done_Face(f);
    if (this->ft_)
        FT_Done_FreeType(this->ft_);
}


bool Renderer::resolve_fonts(const char* pattern,
                             const std::vector<char32_t>& charset) {
    /**
     * Match `pattern` to a primary face, then cover whatever it is missing.
     * A pattern like "monospace" resolves to a face that, in practice, holds
     * Latin and little else, so any charset outside that range needs help: the
     * codepoints the primary face lacks are handed back to fontconfig as a
     * charset requirement, and the sorted matches are walked until every
     * character has a face that can draw it. Characters no font on the system
     * can draw are reported and left blank.
     *
     * fontconfig is initialised for the lookup and torn down again afterwards
     * (FreeType reads the files itself and needs no fontconfig), keeping no
     * fontconfig caches resident.
     */

    if (!FcInit()) {
        fprintf(stderr, "%s: fontconfig init failed\n", NAME);
        return false;
    }
    if (!this->ft_ && FT_Init_FreeType(&this->ft_)) {
        fprintf(stderr, "%s: FT_Init_FreeType failed\n", NAME);
        FcFini();
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
    FT_Face primary = nullptr;
    if (FT_New_Face(this->ft_, (const char*)file, 0, &primary)) {
        fprintf(stderr, "%s: FT_New_Face %s failed\n", NAME, (const char*)file);
        FcPatternDestroy(match);
        FcFini();
        return false;
    }
    FcPatternDestroy(match);

    /* from here on nothing can fail: the primary face is loaded, so the new
     * face set will replace the old one whatever the fallback search finds */
    std::vector<FT_Face> faces{primary};
    std::vector<int> map(charset.size(), -1);
    size_t missing = 0;
    for (size_t i = 0; i < charset.size(); i++) {
        if (FT_Get_Char_Index(primary, (FT_ULong)charset[i]))
            map[i] = 0;
        else
            missing++;
    }

    if (missing) {
        FcCharSet* want = FcCharSetCreate();
        for (size_t i = 0; i < charset.size(); i++)
            if (map[i] < 0)
                FcCharSetAddChar(want, (FcChar32)charset[i]);

        FcPattern* fpat = FcNameParse((const FcChar8*)pattern);
        if (fpat) {
            /* keep the user's pattern (weight, spacing, style) and add the
             * uncovered characters as a requirement, so the sort prefers a
             * font that both matches the look and can draw them */
            FcPatternAddCharSet(fpat, FC_CHARSET, want);
            FcConfigSubstitute(NULL, fpat, FcMatchPattern);
            FcDefaultSubstitute(fpat);
            FcResult fres;
            FcFontSet* fs = FcFontSort(NULL, fpat, FcTrue, NULL, &fres);
            FcPatternDestroy(fpat);

            if (fs) {
                for (int f = 0; f < fs->nfont && missing &&
                                faces.size() < MAX_FACES; f++) {
                    FcChar8* ffile = NULL;
                    if (FcPatternGetString(fs->fonts[f], FC_FILE, 0, &ffile)
                        != FcResultMatch)
                        continue;

                    /* fontconfig's cached charset answers coverage without
                     * touching the font file; only load faces that help */
                    FcCharSet* have = NULL;
                    bool cached = FcPatternGetCharSet(fs->fonts[f], FC_CHARSET,
                                                      0, &have) == FcResultMatch;
                    if (cached) {
                        bool useful = false;
                        for (size_t i = 0; i < charset.size() && !useful; i++)
                            if (map[i] < 0 &&
                                FcCharSetHasChar(have, (FcChar32)charset[i]))
                                useful = true;
                        if (!useful)
                            continue;
                    }

                    FT_Face fb = nullptr;
                    if (FT_New_Face(this->ft_, (const char*)ffile, 0, &fb))
                        continue;

                    int fi = (int)faces.size();
                    bool used = false;
                    for (size_t i = 0; i < charset.size(); i++) {
                        if (map[i] >= 0)
                            continue;
                        if (!FT_Get_Char_Index(fb, (FT_ULong)charset[i]))
                            continue;
                        map[i] = fi;
                        missing--;
                        used = true;
                    }
                    if (used)
                        faces.push_back(fb);
                    else
                        FT_Done_Face(fb);
                }
                FcFontSetDestroy(fs);
            }
        }
        FcCharSetDestroy(want);
    }

    FcFini();

    if (missing)
        fprintf(stderr, "%s: no font on this system draws %zu of the %zu "
                "charset characters; they will fall blank\n", NAME, missing,
                charset.size());

    for (FT_Face f : this->faces_)
        FT_Done_Face(f);
    this->faces_ = faces;
    this->charset_face_ = map;
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

    /* The faces are resolved for a specific charset, so a new charset needs a
     * new lookup just as a new pattern does. If the lookup fails the old faces
     * stay, and so must the charset they cover: the two are one unit, and the
     * atlas is indexed by charset position. */
    if (cfg.font_pattern != this->applied_pattern_ || this->faces_.empty() ||
        np.charset != this->applied_rain_.charset) {
        if (this->resolve_fonts(cfg.font_pattern.c_str(), np.charset)) {
            this->applied_pattern_ = cfg.font_pattern;
            rebuild = true;
        } else {
            fprintf(stderr, "%s: keeping font '%s' and its charset\n", NAME,
                    this->applied_pattern_.c_str());
            np.charset = this->applied_rain_.charset;
        }
    }

    if (!(np == this->applied_rain_)) {
        this->applied_rain_ = np;
        rebuild = true;
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

    /* colours or worse may have changed: every target needs a full repaint */
    this->gen_++;
}


const Atlas& Renderer::atlas_for(int scale) {
    auto it = this->atlases_.find(scale);
    if (it != this->atlases_.end())
        return it->second;

    int n = this->applied_rain_.depth_levels;
    const std::vector<char32_t>& cs = this->applied_rain_.charset;

    Atlas& atlas = this->atlases_[scale];
    atlas.metrics.resize(n);
    atlas.glyphs.assign(n, std::vector<RainGlyph>(cs.size()));

    for (int d = 0; d < n; d++) {
        for (FT_Face f : this->faces_)
            if (FT_Set_Pixel_Sizes(f, 0, this->font_sizes_[d] * scale))
                Utils::die("%s: FT_Set_Pixel_Sizes failed\n", NAME);

        /* The cell is the tallest line box and the widest advance over the
         * faces and characters actually in use, rather than the advance of one
         * reference glyph: a charset can span several faces now, and need not
         * contain any given Latin letter. For a monospaced face and an ASCII
         * charset this is the advance of every glyph in it, as before. */
        int cw = 0, ch = 0;
        for (FT_Face f : this->faces_) {
            int lh = (int)((f->size->metrics.ascender -
                            f->size->metrics.descender) >> 6);
            if (lh > ch)
                ch = lh;
        }

        for (size_t i = 0; i < cs.size(); i++) {
            int fi = this->charset_face_[i];
            if (fi < 0)                         // no face draws this character
                continue;
            FT_Face f = this->faces_[fi];
            if (FT_Load_Char(f, (FT_ULong)cs[i], FT_LOAD_RENDER))
                continue;

            int adv = (int)(f->glyph->advance.x >> 6);
            if (adv > cw)
                cw = adv;

            const FT_Bitmap& bm = f->glyph->bitmap;
            RainGlyph& g = atlas.glyphs[d][i];
            g.w = (int)bm.width;
            g.h = (int)bm.rows;
            g.left = f->glyph->bitmap_left;
            g.top = f->glyph->bitmap_top;
            g.cov.resize((size_t)g.w * g.h);
            for (int row = 0; row < g.h; row++)
                memcpy(&g.cov[(size_t)row * g.w],
                       bm.buffer + (size_t)row * bm.pitch, g.w);
        }

        /* a droplet's spacing divides by these, so never leave them at zero */
        atlas.metrics[d].char_width = cw > 0 ? cw : 1;
        atlas.metrics[d].char_height = ch > 0 ? ch : 1;
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
                    int scale, int state, RenderTarget& tgt,
                    std::vector<DirtyRect>& damage) {
    /**
     * Erase the previous frame's ink (or clear fully when the target is
     * invalid), then draw a body pass and a head pass per depth (near depths
     * last so they overlay far ones), matching the layer ordering both
     * backends used before unification. Each active droplet's clipped ink is
     * recorded in the target, so the next frame erases and the presentation
     * path transfers only what actually changed.
     */

    const Atlas& atlas = this->atlas_for(scale);
    damage.clear();

    if (tgt.gen != this->gen_ || tgt.w != w || tgt.h != h) {
        std::fill_n(px, (size_t)w * h, this->background_);
        tgt.gen = this->gen_;
        tgt.w = w;
        tgt.h = h;
        damage.push_back({0, 0, w, h});
    } else {
        /* The previous ink must become background again, but the damage
         * reported stays the ink rects either way: a full clear rewrites the
         * other pixels with the value they already have. Erasing rect by
         * rect writes narrow column strips, which is several times slower
         * per pixel than one streaming clear, so it only pays off when the
         * ink covers little of the frame. */
        size_t area = 0;
        for (const DirtyRect& r : tgt.content)
            area += (size_t)r.w * r.h;
        if (area * 8 > (size_t)w * h) {
            std::fill_n(px, (size_t)w * h, this->background_);
        } else {
            for (const DirtyRect& r : tgt.content)
                for (int row = 0; row < r.h; row++)
                    std::fill_n(px + (size_t)(r.y + row) * w + r.x,
                                (size_t)r.w, this->background_);
        }
        for (const DirtyRect& r : tgt.content)
            damage.push_back(r);
    }
    tgt.content.clear();

    /* bucket the active droplets by depth (counting sort), so each depth
     * pass walks only its own droplets */
    int nd = rain.p.depth_levels;
    this->bucket_start_.assign(nd + 1, 0);
    for (int ai = 0; ai < rain.active_count; ai++)
        this->bucket_start_[rain.droplets[rain.active_list[ai]].depth + 1]++;
    for (int d = 0; d < nd; d++)
        this->bucket_start_[d + 1] += this->bucket_start_[d];
    this->order_.resize(rain.active_count);
    this->bucket_pos_.assign(this->bucket_start_.begin(),
                             this->bucket_start_.end() - 1);
    for (int ai = 0; ai < rain.active_count; ai++)
        this->order_[this->bucket_pos_[rain.droplets[rain.active_list[ai]]
                                           .depth]++] = ai;

    this->drop_rects_.assign(rain.active_count, DirtyRect{});

    for (int d = 0; d < nd; d++) {
        int dch = atlas.metrics[d].char_height;

        const uint32_t* body = this->body_lut_[state][d].data();
        for (int k = this->bucket_start_[d]; k < this->bucket_start_[d + 1]; k++) {
            int ai = this->order_[k];
            int i = rain.active_list[ai];
            const Droplet& drop = rain.droplets[i];
            const uint16_t* dc = rain.droplet_chars(i);
            /* only the characters whose baseline can put ink on screen:
             * y = drop.y - j*dch within (-dch, h + dch); the blit clips the
             * exact extents */
            int j0 = 1, j1 = drop.length - 1;
            int over = drop.y - h - dch;
            if (over > 0)
                j0 = (over + dch - 1) / dch;
            if (j0 < 1)
                j0 = 1;
            int jv = (drop.y + dch) / dch;
            if (jv < j1)
                j1 = jv;
            DirtyRect& ink = this->drop_rects_[ai];
            for (int j = j0; j <= j1; j++)
                blit_glyph(px, w, h, atlas.glyphs[d][dc[j]], drop.x,
                           drop.y - j * dch, body, ink);
        }

        const uint32_t* head = this->head_lut_[state][d].data();
        for (int k = this->bucket_start_[d]; k < this->bucket_start_[d + 1]; k++) {
            int ai = this->order_[k];
            int i = rain.active_list[ai];
            const Droplet& drop = rain.droplets[i];
            if (drop.y > -dch && drop.y < h + dch)
                blit_glyph(px, w, h,
                           atlas.glyphs[d][rain.droplet_chars(i)[0]],
                           drop.x, drop.y, head, this->drop_rects_[ai]);
        }
    }

    for (const DirtyRect& r : this->drop_rects_) {
        if (r.w > 0) {
            tgt.content.push_back(r);
            damage.push_back(r);
        }
    }
}
