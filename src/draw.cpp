#include "draw.h"

#include <cmath>
#include <ctime>

#include "config.h"
#include "covers.h"
#include "install.h"

namespace sanctum {

ImDrawList *dl() { return ImGui::GetBackgroundDrawList(); }

float text_w(float size, const char *s) { return app().font->CalcTextSizeA(size, FLT_MAX, 0, s).x; }

float text(float x, float y, float size, ImU32 col, const char *s, float align) {
    float w = text_w(size, s);
    dl()->AddText(app().font, S(size), P(x - w * align, y), col, s);
    return w;
}

float text_wrap(float x, float y, float size, ImU32 col, const char *s, float w, bool draw) {
    ImVec2 sz = app().font->CalcTextSizeA(size, FLT_MAX, w, s);
    if (draw) dl()->AddText(app().font, S(size), P(x, y), col, s, nullptr, S(w));
    return sz.y;
}

void progress_bar(float x, float y, float w, float h, float frac, ImU32 col) {
    rect(x, y, w, h, pal().panel2, h / 2);
    if (frac < 0) {   // no figure: a block sweeps across
        float t = (float)std::fmod(app().now * 0.8, 1.0), bw = w * 0.25f, bx = x + (w + bw) * t - bw;
        dl()->PushClipRect(P(x, y), P(x + w, y + h), true);
        rect(bx, y, bw, h, col, h / 2);
        dl()->PopClipRect();
        return;
    }
    frac = std::min(1.0f, std::max(0.0f, frac));
    if (frac > 0) rect(x, y, std::max(h, w * frac), h, col, h / 2);
}

std::string fit(float size, const std::string &s, float max_w) {
    if (text_w(size, s.c_str()) <= max_w) return s;
    std::string t = s;
    while (!t.empty() && text_w(size, (t + "...").c_str()) > max_w) {
        t.pop_back();
        while (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80) t.pop_back();   // whole UTF-8 characters
    }
    return t + "...";
}

void rect(float x, float y, float w, float h, ImU32 col, float r) { dl()->AddRectFilled(P(x, y), P(x + w, y + h), col, S(r)); }
void frame(float x, float y, float w, float h, ImU32 col, float r, float t) {
    dl()->AddRect(P(x, y), P(x + w, y + h), col, S(r), 0, S(t));
}
void circle(float x, float y, float rad, ImU32 col) { dl()->AddCircleFilled(P(x, y), S(rad), col, 48); }

void focus_ring(float x, float y, float w, float h, float r, float pad) {
    float k = 0.5f + 0.5f * std::sin((float)app().now * 4.2f);
    ImU32 c = pal().focus;
    frame(x - pad - 3, y - pad - 3, w + 2 * pad + 6, h + 2 * pad + 6, with_alpha(c, (int)(40 + 50 * k)), r + pad + 3, 6);
    frame(x - pad, y - pad, w + 2 * pad, h + 2 * pad, with_alpha(c, (int)(165 + 90 * k)), r + pad, 4.5f);
}

// ---- button glyphs
// the label of a glyph drawn as a pill (keys, shoulders, menu buttons); nullptr for a round face button
static const char *pill_label(Nav n, PadStyle st) {
    if (st == PAD_KEYBOARD) {
        switch (n) {
        case NAV_A: return "Enter";
        case NAV_B: return "Esc";
        case NAV_X: return "X";
        case NAV_Y: return "Tab";
        case NAV_L: return "Q";
        case NAV_R: return "E";
        case NAV_PLUS: return "F5";
        case NAV_MINUS: return "F1";
        default: return "?";
        }
    }
    if (n == NAV_L) return st == PAD_PLAYSTATION ? "L1" : st == PAD_XBOX ? "LB" : "L";
    if (n == NAV_R) return st == PAD_PLAYSTATION ? "R1" : st == PAD_XBOX ? "RB" : "R";
    if (n == NAV_PLUS && st != PAD_NINTENDO) return st == PAD_PLAYSTATION ? "Options" : "Menu";
    if (n == NAV_MINUS && st != PAD_NINTENDO) return st == PAD_PLAYSTATION ? "Create" : "View";
    return nullptr;
}

float glyph_w(Nav n, float size) {
    const char *l = pill_label(n, app().pad);
    return l ? std::max(size, text_w(size * 0.6f, l) + size * 0.6f) : size;
}

float glyph(Nav n, float x, float y, float size) {
    const Palette &p = pal();
    PadStyle st = app().pad;
    float r = size / 2, cx = x + r, cy = y + r;
    auto letter_disc = [&](const char *l, ImU32 bg, ImU32 fg) {
        circle(cx, cy, r, bg);
        text(cx, y + size * 0.13f, size * 0.72f, fg, l, 0.5f);
        return size;
    };
    if (const char *l = pill_label(n, st)) {
        float w = glyph_w(n, size);
        rect(x, y, w, size, p.text, r);
        text(x + w / 2, y + size * 0.18f, size * 0.6f, p.bg, l, 0.5f);
        return w;
    }
    if (n == NAV_PLUS) return letter_disc("+", p.text, p.bg);
    if (n == NAV_MINUS) return letter_disc("-", p.text, p.bg);
    if (st == PAD_PLAYSTATION) {
        circle(cx, cy, r, p.text);
        ImU32 c = p.bg;
        float s = r * 0.45f;
        switch (n) {
        case NAV_A:   // cross
            dl()->AddLine(P(cx - s, cy - s), P(cx + s, cy + s), c, S(2.4f));
            dl()->AddLine(P(cx - s, cy + s), P(cx + s, cy - s), c, S(2.4f));
            break;
        case NAV_B: dl()->AddCircle(P(cx, cy), S(s * 1.05f), c, 24, S(2.4f)); break;
        case NAV_X: dl()->AddRect(P(cx - s, cy - s), P(cx + s, cy + s), c, 0, 0, S(2.4f)); break;
        case NAV_Y: dl()->AddTriangle(P(cx, cy - s * 1.1f), P(cx + s * 1.1f, cy + s * 0.8f), P(cx - s * 1.1f, cy + s * 0.8f), c, S(2.4f)); break;
        default: break;
        }
        return size;
    }
    const char *l = n == NAV_A ? "A" : n == NAV_B ? "B" : n == NAV_X ? "X" : "Y";
    if (st == PAD_XBOX) {
        ImU32 c = n == NAV_A ? IM_COL32(92, 190, 74, 255) : n == NAV_B ? IM_COL32(222, 64, 58, 255)
                : n == NAV_X ? IM_COL32(58, 128, 228, 255) : IM_COL32(240, 190, 40, 255);
        return letter_disc(l, c, IM_COL32_WHITE);
    }
    return letter_disc(l, p.text, p.bg);
}

void hint_bar(std::initializer_list<std::pair<Nav, const char *>> hints) {
    const Palette &p = pal();
    rect(0, 656, 1280, 64, p.bg);
    rect(44, 656, 1192, 1, p.line);
    // left: the controller in use
    icon(IC_PAD, 66, 688, 26, p.soft);
    std::string who = app().pad == PAD_KEYBOARD ? "Keyboard and mouse" : app().pad_name;
    text(88, 677, 17, p.soft, fit(17, who, 360).c_str());
    // right: the hints, laid out right to left
    float x = 1236;
    std::pair<Nav, const char *> list[8];
    int n = 0;
    for (auto &h : hints) if (n < 8) list[n++] = h;
    for (int i = n - 1; i >= 0; --i) {
        float lw = text_w(17, list[i].second);
        x -= lw;
        text(x, 677, 17, p.text, list[i].second);
        float gw = glyph_w(list[i].first, 26);
        x -= 8 + gw;
        glyph(list[i].first, x, 675, 26);
        x -= 30;
    }
}

void status_bar(bool show_profile) {
    const Palette &p = pal();
    if (show_profile) {
        circle(67, 46, 23, p.accent);
        std::string ini = cfg().profile.empty() ? "?" : cfg().profile.substr(0, 1);
        if (cfg().profile.rfind("Player ", 0) == 0) ini = "P" + cfg().profile.substr(7, 1);
        text(67, 33, 21, IM_COL32_WHITE, ini.c_str(), 0.5f);
    }
    // right: search status, battery, clock
    time_t t = time(nullptr);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char clock[16];
    strftime(clock, sizeof clock, "%H:%M", &lt);
    float x = 1236 - text(1236, 32, 24, p.text, clock, 1);
    int pct = -1;
    SDL_PowerState ps = SDL_GetPowerInfo(nullptr, &pct);
    if (ps == SDL_POWERSTATE_ON_BATTERY || ps == SDL_POWERSTATE_CHARGING || ps == SDL_POWERSTATE_CHARGED) {
        x -= 52;
        frame(x, 36, 34, 18, p.soft, 4, 2);
        rect(x + 34, 41, 3, 8, p.soft, 1);
        float f = pct >= 0 ? pct / 100.0f : 1;
        rect(x + 4, 40, 26 * f, 10, ps == SDL_POWERSTATE_ON_BATTERY && pct < 20 ? p.warn : p.soft, 2);
    }
    // a download running: its port and percent, so it is visible from every screen
    for (auto &j : install_jobs()) {
        if (j.phase != PH_DOWNLOAD && j.phase != PH_VERIFY && j.phase != PH_INSTALL) continue;
        char s[96];
        int pc = j.total ? (int)(100.0 * j.done / j.total) : 0;
        SDL_snprintf(s, sizeof s, j.phase == PH_DOWNLOAD ? "%s  %d%%" : "%s  installing", ports()[j.port].name, pc);
        x -= 24;
        float w = text_w(16, s);
        text(x - w, 35, 16, p.soft, s);
        icon(IC_DOWNLOAD, x - w - 18, 45, 18, p.accent);
        x -= w + 30;
        break;
    }
    if (scan_running()) {
        char s[64];
        SDL_snprintf(s, sizeof s, "Searching this PC for ports  %d", scan_folders());
        x -= 24;
        float w = text_w(16, s);
        text(x - w, 35, 16, p.dim, s);
        float a = (float)app().now * 6;
        for (int i = 0; i < 8; ++i)
            circle(x - w - 22 + std::cos(a + i * 0.785f) * 8, 45 + std::sin(a + i * 0.785f) * 8, 1.8f,
                   with_alpha(p.dim, 40 + i * 26));
    }
}

// ---- icons: simple strokes, centred on (cx, cy)
void icon(Icon i, float cx, float cy, float s, ImU32 c) {
    ImDrawList *d = dl();
    float h = s / 2, t = S(std::max(1.6f, s / 13));
    auto L = [&](float x0, float y0, float x1, float y1) { d->AddLine(P(cx + x0 * h, cy + y0 * h), P(cx + x1 * h, cy + y1 * h), c, t); };
    auto R = [&](float x0, float y0, float x1, float y1, float r) {
        d->AddRect(P(cx + x0 * h, cy + y0 * h), P(cx + x1 * h, cy + y1 * h), c, S(r * h), 0, t);
    };
    auto C = [&](float x, float y, float r) { d->AddCircle(P(cx + x * h, cy + y * h), S(r * h), c, 32, t); };
    switch (i) {
    case IC_NEWS: R(-0.8f, -0.7f, 0.8f, 0.7f, 0.12f); L(-0.5f, -0.3f, 0.5f, -0.3f); L(-0.5f, 0, 0.5f, 0); L(-0.5f, 0.3f, 0.15f, 0.3f); break;
    case IC_DOWNLOAD: L(0, -0.8f, 0, 0.35f); L(-0.45f, -0.05f, 0, 0.38f); L(0.45f, -0.05f, 0, 0.38f); L(-0.75f, 0.75f, 0.75f, 0.75f); break;
    case IC_MODS:
        L(-0.7f, 0.7f, 0.25f, -0.25f); R(0.15f, -0.85f, 0.85f, -0.15f, 0.2f); L(-0.85f, 0.55f, -0.55f, 0.85f); break;
    case IC_PROFILE: C(0, -0.35f, 0.38f); d->PathArcTo(P(cx, cy + 0.85f * h), S(0.72f * h), 3.14159f, 6.28318f, 24); d->PathStroke(c, 0, t); break;
    case IC_PAD:
        R(-0.9f, -0.45f, 0.9f, 0.5f, 0.42f); L(-0.55f, 0, -0.15f, 0); L(-0.35f, -0.2f, -0.35f, 0.2f);
        d->AddCircleFilled(P(cx + 0.38f * h, cy - 0.08f * h), S(0.09f * h), c); d->AddCircleFilled(P(cx + 0.58f * h, cy + 0.12f * h), S(0.09f * h), c); break;
    case IC_GEAR:
        C(0, 0, 0.28f);
        for (int k = 0; k < 8; ++k) {
            float a = k * 0.785f;
            L(std::cos(a) * 0.5f, std::sin(a) * 0.5f, std::cos(a) * 0.85f, std::sin(a) * 0.85f);
        }
        C(0, 0, 0.55f); break;
    case IC_POWER:
        L(0, -0.85f, 0, -0.1f);
        d->PathArcTo(P(cx, cy + 0.05f * h), S(0.72f * h), -1.0f, 4.14f, 32); d->PathStroke(c, 0, t); break;
    case IC_FOLDER: R(-0.85f, -0.45f, 0.85f, 0.7f, 0.1f); L(-0.85f, -0.45f, -0.6f, -0.7f); L(-0.6f, -0.7f, -0.1f, -0.7f); L(-0.1f, -0.7f, 0.1f, -0.45f); break;
    case IC_PALETTE:
        C(0, 0, 0.8f); d->AddCircleFilled(P(cx - 0.35f * h, cy - 0.25f * h), S(0.13f * h), c);
        d->AddCircleFilled(P(cx + 0.1f * h, cy - 0.42f * h), S(0.13f * h), c); d->AddCircleFilled(P(cx + 0.42f * h, cy - 0.05f * h), S(0.13f * h), c); break;
    case IC_LIBRARY: R(-0.8f, -0.75f, -0.35f, 0.75f, 0.08f); R(-0.2f, -0.75f, 0.25f, 0.75f, 0.08f); L(0.45f, -0.65f, 0.85f, 0.7f); break;
    case IC_INFO: C(0, 0, 0.8f); L(0, -0.1f, 0, 0.42f); d->AddCircleFilled(P(cx, cy - 0.38f * h), S(0.09f * h), c); break;
    case IC_PLAY:
        d->AddTriangleFilled(P(cx - 0.45f * h, cy - 0.7f * h), P(cx + 0.7f * h, cy), P(cx - 0.45f * h, cy + 0.7f * h), c); break;
    case IC_SEARCH: C(-0.15f, -0.15f, 0.55f); L(0.25f, 0.25f, 0.8f, 0.8f); break;
    case IC_DRIVE: R(-0.85f, -0.4f, 0.85f, 0.45f, 0.15f); d->AddCircleFilled(P(cx + 0.55f * h, cy), S(0.09f * h), c); break;
    case IC_UP: L(0, 0.7f, 0, -0.6f); L(-0.45f, -0.15f, 0, -0.62f); L(0.45f, -0.15f, 0, -0.62f); break;
    case IC_CHECK: L(-0.6f, 0, -0.15f, 0.45f); L(-0.15f, 0.45f, 0.65f, -0.45f); break;
    case IC_STORE:   // a bag
        R(-0.7f, -0.35f, 0.7f, 0.8f, 0.12f);
        d->PathArcTo(P(cx, cy - 0.35f * h), S(0.32f * h), 3.14159f, 6.28318f, 16); d->PathStroke(c, 0, t); break;
    case IC_DISPLAY: R(-0.85f, -0.65f, 0.85f, 0.4f, 0.1f); L(0, 0.4f, 0, 0.7f); L(-0.4f, 0.75f, 0.4f, 0.75f); break;
    case IC_PAUSE: L(-0.3f, -0.6f, -0.3f, 0.6f); L(0.3f, -0.6f, 0.3f, 0.6f); break;
    case IC_CLOSE: L(-0.55f, -0.55f, 0.55f, 0.55f); L(-0.55f, 0.55f, 0.55f, -0.55f); break;
    }
}

void tile(const Port &p, float x, float y, float sz, float alpha, bool badges, float lit) {
    const Palette &pl = pal();
    int a = (int)(255 * alpha);
    float r = sz * 0.065f;
    PortState st = p.state();
    SDL_Texture *tex = cover(p.id);
    // a soft shadow
    rect(x + 2, y + 8, sz - 4, sz, with_alpha(IM_COL32_BLACK, (int)((pl.light ? 30 : 90) * alpha)), r + 4);
    if (tex) {
        // not on this PC: dimmed, brightening as it takes focus (lit 0..1)
        int v = st == PORT_ABSENT ? (int)(150 + 105 * lit) : 255;
        ImU32 tint = IM_COL32(v, v, v, a);
        dl()->AddImageRounded((ImTextureID)(intptr_t)tex, P(x, y), P(x + sz, y + sz), ImVec2(0, 0), ImVec2(1, 1), tint, S(r));
    } else {
        ImU32 c1 = p.colours.accent, c2 = p.colours.deep;
        if (st == PORT_ABSENT) c1 = mix(c1, IM_COL32(70, 70, 76, 255), 0.6f), c2 = mix(c2, IM_COL32(30, 30, 34, 255), 0.6f);
        rect(x, y, sz, sz, with_alpha(c2, a), r);
        rect(x, y, sz, sz * 0.62f, with_alpha(c1, a), r);
        rect(x, y + sz * 0.5f, sz, sz * 0.12f, with_alpha(mix(c1, c2, 0.5f), a));
        text(x + sz / 2, y + sz * 0.3f, sz * 0.21f, with_alpha(IM_COL32_WHITE, a), p.mono, 0.5f);
        text(x + sz / 2, y + sz * 0.58f, sz * 0.065f, with_alpha(IM_COL32(255, 255, 255, 210), a), p.name, 0.5f);
    }
    if (!badges) return;
    // downloading or installing: a bar across the bottom of the tile
    int idx = (int)(&p - &ports()[0]);
    for (auto &j : install_jobs()) {
        if (j.port != idx || j.phase >= PH_DONE) continue;
        float bh = sz * 0.16f, by = y + sz - bh;
        dl()->AddRectFilled(P(x, by), P(x + sz, y + sz), IM_COL32(0, 0, 0, (int)(190 * alpha)), S(r), ImDrawFlags_RoundCornersBottom);
        float f = j.phase == PH_DOWNLOAD && j.total ? (float)j.done / j.total : -1;
        progress_bar(x + sz * 0.07f, by + bh * 0.62f, sz * 0.86f, bh * 0.16f, f, p.colours.accent);
        const char *w = j.phase == PH_QUEUED ? "Waiting" : j.phase == PH_DOWNLOAD ? "Downloading" : j.phase == PH_VERIFY ? "Checking" : "Installing";
        text(x + sz * 0.07f, by + bh * 0.12f, sz * 0.06f, with_alpha(IM_COL32_WHITE, a), w);
        return;
    }
    if (st == PORT_ABSENT) {   // download badge with the size
        std::string size = p.download_size();
        float bw = text_w(sz * 0.068f, size.c_str()) + sz * 0.2f, bh = sz * 0.14f;
        float bx = x + sz - bw - sz * 0.045f, by = y + sz - bh - sz * 0.045f;
        rect(bx, by, bw, bh, IM_COL32(0, 0, 0, (int)(185 * alpha)), bh / 2);
        icon(IC_DOWNLOAD, bx + bh * 0.55f, by + bh / 2, bh * 0.55f, with_alpha(IM_COL32_WHITE, a));
        text(bx + bh, by + bh * 0.2f, sz * 0.068f, with_alpha(IM_COL32_WHITE, a), size.c_str());
    } else if (st == PORT_UPDATE) {   // the red dot of a Switch notice, inside the corner
        float d = sz * 0.05f, cx = x + sz - d - sz * 0.05f, cy = y + d + sz * 0.05f;
        circle(cx, cy, d + 3, with_alpha(IM_COL32_WHITE, a));
        circle(cx, cy, d, with_alpha(pl.accent, a));
    }
}

void toast_draw() {
    App &a = app();
    if (a.now >= a.toast.until || a.toast.text.empty()) return;
    const Palette &p = pal();
    // rises in over a quarter second, fades out over the last quarter
    float k = std::min(1.0f, (float)(a.toast.until - a.now) * 4) * std::min(1.0f, (float)(a.now - a.toast.start) * 4);
    k = std::max(0.0f, k);
    float w = text_w(18, a.toast.text.c_str()) + 56;
    float y = 590 + (1 - k) * 16;
    rect(640 - w / 2, y, w, 46, with_alpha(p.text, (int)(235 * k)), 23);
    text(640, y + 12, 18, with_alpha(p.bg, (int)(255 * k)), a.toast.text.c_str(), 0.5f);
}

void background(ImU32 tint) {
    const Palette &p = pal();
    ImVec2 ws = ImGui::GetIO().DisplaySize;
    dl()->AddRectFilled(ImVec2(0, 0), ws, p.bg);
    if (!tint) return;
    // the focused port's colour washes in from the top left, fading to the theme's backdrop
    ImU32 t0 = mix(p.bg, tint, p.light ? 0.13f : 0.2f), t1 = mix(p.bg, tint, p.light ? 0.04f : 0.07f);
    dl()->AddRectFilledMultiColor(ImVec2(0, 0), ws, t0, t1, p.bg, t1);
}

void image_fade(SDL_Texture *t, float x0, float y0, float x1, float y1, int a) {
    // corner alphas: clear on the left and along the bottom, strongest at the top right
    ImDrawList *d = dl();
    d->PushTextureID((ImTextureID)(intptr_t)t);
    d->PrimReserve(6, 4);
    ImDrawIdx i = (ImDrawIdx)d->_VtxCurrentIdx;
    d->PrimWriteIdx(i), d->PrimWriteIdx(i + 1), d->PrimWriteIdx(i + 2);
    d->PrimWriteIdx(i), d->PrimWriteIdx(i + 2), d->PrimWriteIdx(i + 3);
    d->PrimWriteVtx(P(x0, y0), ImVec2(0, 0), IM_COL32(255, 255, 255, 0));
    d->PrimWriteVtx(P(x1, y0), ImVec2(1, 0), IM_COL32(255, 255, 255, a));
    d->PrimWriteVtx(P(x1, y1), ImVec2(1, 1), IM_COL32(255, 255, 255, 0));
    d->PrimWriteVtx(P(x0, y1), ImVec2(0, 1), IM_COL32(255, 255, 255, 0));
    d->PopTextureID();
}

float ease(float cur, float target, float speed) {
    float k = 1 - std::exp(-speed * app().dt);
    return cur + (target - cur) * k;
}

}  // namespace sanctum
