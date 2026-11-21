#include "avatars.h"

#include "app.h"
#include "config.h"
#include "draw.h"
#include "stb_image.h"

namespace sanctum {

static SDL_Texture *g_tex = nullptr;
static int g_cols = 8, g_rows = 1;
static std::vector<std::string> g_names, g_cats, g_cat_list;

static const std::vector<AvatarColour> kColours = {
    {"Red", IM_COL32(232, 36, 58, 255)},     {"Coral", IM_COL32(255, 94, 77, 255)},
    {"Orange", IM_COL32(255, 128, 32, 255)}, {"Amber", IM_COL32(240, 172, 24, 255)},
    {"Lime", IM_COL32(128, 192, 36, 255)},   {"Green", IM_COL32(40, 170, 90, 255)},
    {"Teal", IM_COL32(18, 156, 146, 255)},   {"Cyan", IM_COL32(28, 176, 208, 255)},
    {"Sky", IM_COL32(58, 138, 230, 255)},    {"Blue", IM_COL32(40, 88, 208, 255)},
    {"Indigo", IM_COL32(88, 72, 210, 255)},  {"Violet", IM_COL32(140, 70, 220, 255)},
    {"Magenta", IM_COL32(198, 58, 188, 255)}, {"Pink", IM_COL32(238, 80, 150, 255)},
    {"Slate", IM_COL32(88, 104, 128, 255)},  {"Graphite", IM_COL32(52, 54, 64, 255)},
};

void avatars_init(SDL_Renderer *r) {
    std::string base = SDL_GetBasePath();
    size_t n = 0;
    if (char *txt = (char *)SDL_LoadFile((base + "assets\\avatars.txt").c_str(), &n)) {
        std::string all(txt, n);
        SDL_free(txt);
        size_t at = 0;
        while (at < all.size()) {
            size_t end = all.find('\n', at);
            if (end == std::string::npos) end = all.size();
            std::string line = all.substr(at, end - at);
            at = end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;
            g_cats.push_back(line.substr(0, tab));
            g_names.push_back(line.substr(tab + 1));
            if (g_cat_list.empty() || g_cat_list.back() != g_cats.back()) g_cat_list.push_back(g_cats.back());
        }
    }
    int w, h, ch;
    if (unsigned char *px = stbi_load((base + "assets\\avatars.png").c_str(), &w, &h, &ch, 4)) {
        g_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
        if (g_tex) {
            SDL_UpdateTexture(g_tex, nullptr, px, w * 4);
            SDL_SetTextureBlendMode(g_tex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(g_tex, SDL_SCALEMODE_LINEAR);
        }
        g_cols = w / 192, g_rows = h / 192;
        stbi_image_free(px);
    }
    if (!g_tex) g_names.clear(), g_cats.clear(), g_cat_list.clear();
}

int avatar_count() { return (int)g_names.size(); }
const std::string &avatar_name(int i) { return g_names[i]; }
const std::string &avatar_category(int i) { return g_cats[i]; }
const std::vector<std::string> &avatar_categories() { return g_cat_list; }
const std::vector<AvatarColour> &avatar_colours() { return kColours; }

int avatar_find(const std::string &name) {
    for (int i = 0; i < (int)g_names.size(); ++i)
        if (g_names[i] == name) return i;
    return -1;
}

void avatar_draw(int icon, int bg, float cx, float cy, float r, float alpha) {
    int a = (int)(255 * alpha);
    ImU32 col = kColours[((bg % 16) + 16) % 16].col;
    dl()->AddCircleFilled(P(cx, cy), S(r), with_alpha(col, a), 64);
    if (!g_tex || icon < 0 || icon >= (int)g_names.size()) {   // no artwork: the profile's initial
        std::string ini = cfg().profile.empty() ? "?" : cfg().profile.substr(0, 1);
        text(cx, cy - r * 0.45f, r * 0.9f, with_alpha(IM_COL32_WHITE, a), ini.c_str(), 0.5f);
        return;
    }
    float u0 = (float)(icon % g_cols) / g_cols, v0 = (float)(icon / g_cols) / g_rows;
    dl()->AddImage((ImTextureID)(intptr_t)g_tex, P(cx - r, cy - r), P(cx + r, cy + r), ImVec2(u0, v0),
                   ImVec2(u0 + 1.0f / g_cols, v0 + 1.0f / g_rows), IM_COL32(255, 255, 255, a));
}

void avatar_draw_player(float cx, float cy, float r) {
    avatar_draw(avatar_find(cfg().avatar), cfg().avatar_bg, cx, cy, r);
}

}  // namespace sanctum
