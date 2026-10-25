#include "gfx.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TEXT_CACHE 2048
#define TEXT_PROBE 8

typedef struct {
    unsigned long key;
    TTF_Font *font;
    Uint32 color;
    char *str;
    SDL_Texture *tex;
    int w, h;
    unsigned long used;
} TextEntry;

static SDL_Renderer *ren;
static TextEntry cache[TEXT_CACHE];
static TTF_Font *fb_main[8], *fb_font[8];
static int fb_count;
static SDL_Texture *grad_h, *grad_v;

static const char *regular_fonts[] = {
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
    NULL
};

static const char *bold_fonts[] = {
    "/usr/share/fonts/noto/NotoSans-Bold.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
    NULL
};

static const char *black_fonts[] = {
    "/usr/share/fonts/noto/NotoSans-Black.ttf",
    "/usr/share/fonts/noto/NotoSans-ExtraBold.ttf",
    NULL
};

static TTF_Font *open_font(const char **paths, int pt, const char **fallback)
{
    for (int i = 0; paths[i]; i++) {
        TTF_Font *f = TTF_OpenFont(paths[i], pt);
        if (f) return f;
    }
    if (fallback) return open_font(fallback, pt, NULL);
    return NULL;
}

static void add_fallback(TTF_Font *main, int pt, bool bold)
{
    static const char *cjk[2][3] = {
        {"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", NULL},
        {"/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc", "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc", NULL},
    };
    if (!main || fb_count >= 8) return;
    for (int i = 0; cjk[bold][i]; i++) {
        TTF_Font *f = TTF_OpenFontIndex(cjk[bold][i], pt, 0);
        if (f) {
            fb_main[fb_count] = main;
            fb_font[fb_count++] = f;
            return;
        }
    }
}

static TTF_Font *pick_font(TTF_Font *f, const char *txt)
{
    TTF_Font *fb = NULL;
    for (int i = 0; i < fb_count; i++)
        if (fb_main[i] == f) fb = fb_font[i];
    if (!fb) return f;
    for (const unsigned char *p = (const unsigned char *)txt; *p;) {
        Uint32 cp = *p;
        int n = 1;
        if (cp >= 0xF0) { cp &= 0x07; n = 4; }
        else if (cp >= 0xE0) { cp &= 0x0F; n = 3; }
        else if (cp >= 0xC0) { cp &= 0x1F; n = 2; }
        for (int k = 1; k < n && p[k]; k++) cp = (cp << 6) | (p[k] & 0x3F);
        if (cp > 0x7F && !TTF_GlyphIsProvided32(f, cp)) return fb;
        for (int k = 0; k < n && *p; k++) p++;
    }
    return f;
}

static SDL_Texture *make_gradient(int w, int h)
{
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return NULL;
    Uint32 px[256];
    for (int i = 0; i < 256; i++) {
        double f = i / 255.0;
        double a = f * f * (3.0 - 2.0 * f);
        px[i] = ((Uint32)(a * 255.0) << 24) | 0xFFFFFF;
    }
    SDL_UpdateTexture(t, NULL, px, w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

bool gfx_init(SDL_Renderer *r, UI *ui)
{
    ren = r;
    ui->xs = open_font(regular_fonts, 13, NULL);
    ui->sm = open_font(regular_fonts, 15, NULL);
    ui->md = open_font(regular_fonts, 18, NULL);
    ui->mdb = open_font(bold_fonts, 18, regular_fonts);
    ui->lg = open_font(bold_fonts, 24, regular_fonts);
    ui->xl = open_font(bold_fonts, 34, regular_fonts);
    ui->hero = open_font(black_fonts, 64, bold_fonts);
    add_fallback(ui->xs, 13, false);
    add_fallback(ui->sm, 15, false);
    add_fallback(ui->md, 18, false);
    add_fallback(ui->mdb, 18, true);
    add_fallback(ui->lg, 24, true);
    add_fallback(ui->xl, 34, true);
    add_fallback(ui->hero, 64, true);
    grad_h = make_gradient(256, 1);
    grad_v = make_gradient(1, 256);
    return ui->sm && ui->md;
}

void gfx_shutdown(UI *ui)
{
    for (int i = 0; i < TEXT_CACHE; i++) {
        if (cache[i].tex) SDL_DestroyTexture(cache[i].tex);
        free(cache[i].str);
    }
    memset(cache, 0, sizeof(cache));
    TTF_Font **fonts[] = {&ui->xs, &ui->sm, &ui->md, &ui->mdb, &ui->lg, &ui->xl, &ui->hero};
    for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
        if (*fonts[i]) TTF_CloseFont(*fonts[i]);
        *fonts[i] = NULL;
    }
    for (int i = 0; i < fb_count; i++) TTF_CloseFont(fb_font[i]);
    fb_count = 0;
    if (grad_h) SDL_DestroyTexture(grad_h);
    if (grad_v) SDL_DestroyTexture(grad_v);
}

void gfx_frame_end(void)
{
    for (int i = (int)(frame_no % 64); i < TEXT_CACHE; i += 64) {
        TextEntry *e = &cache[i];
        if (e->tex && frame_no - e->used > 600) {
            SDL_DestroyTexture(e->tex);
            free(e->str);
            memset(e, 0, sizeof(*e));
        }
    }
}

void fill_rect(SDL_Renderer *r, int x, int y, int w, int h, Color c)
{
    SDL_SetRenderDrawBlendMode(r, c.a < 255 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderFillRect(r, &rect);
}

void fill_round(SDL_Renderer *r, int x, int y, int w, int h, int rad, Color c)
{
    if (rad * 2 > h) rad = h / 2;
    if (rad * 2 > w) rad = w / 2;
    if (rad <= 0) {
        fill_rect(r, x, y, w, h, c);
        return;
    }
    SDL_SetRenderDrawBlendMode(r, c.a < 255 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_Rect mid = {x, y + rad, w, h - 2 * rad};
    SDL_RenderFillRect(r, &mid);
    for (int i = 0; i < rad; i++) {
        double dy = rad - i - 0.5;
        int dx = (int)(rad - sqrt((double)rad * rad - dy * dy) + 0.5);
        SDL_Rect top = {x + dx, y + i, w - 2 * dx, 1};
        SDL_Rect bot = {x + dx, y + h - 1 - i, w - 2 * dx, 1};
        SDL_RenderFillRect(r, &top);
        SDL_RenderFillRect(r, &bot);
    }
}

void stroke_round(SDL_Renderer *r, int x, int y, int w, int h, int rad, int thick, Color c)
{
    for (int t = 0; t < thick; t++) {
        int rr = rad - t > 0 ? rad - t : 0;
        int xx = x + t, yy = y + t, ww = w - 2 * t, hh = h - 2 * t;
        SDL_SetRenderDrawBlendMode(r, c.a < 255 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
        SDL_RenderDrawLine(r, xx + rr, yy, xx + ww - 1 - rr, yy);
        SDL_RenderDrawLine(r, xx + rr, yy + hh - 1, xx + ww - 1 - rr, yy + hh - 1);
        SDL_RenderDrawLine(r, xx, yy + rr, xx, yy + hh - 1 - rr);
        SDL_RenderDrawLine(r, xx + ww - 1, yy + rr, xx + ww - 1, yy + hh - 1 - rr);
        for (int i = 0; i <= rr * 2; i++) {
            double a = (M_PI / 2.0) * i / (rr * 2 + 1);
            int dx = (int)(rr - rr * cos(a) + 0.5);
            int dy = (int)(rr - rr * sin(a) + 0.5);
            SDL_RenderDrawPoint(r, xx + dx, yy + dy);
            SDL_RenderDrawPoint(r, xx + ww - 1 - dx, yy + dy);
            SDL_RenderDrawPoint(r, xx + dx, yy + hh - 1 - dy);
            SDL_RenderDrawPoint(r, xx + ww - 1 - dx, yy + hh - 1 - dy);
        }
    }
}

static void draw_grad(SDL_Renderer *r, SDL_Texture *t, int x, int y, int w, int h, Color c, SDL_RendererFlip flip)
{
    if (!t || w <= 0 || h <= 0) return;
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t, c.a);
    SDL_Rect dst = {x, y, w, h};
    SDL_RenderCopyEx(r, t, NULL, &dst, 0, NULL, flip);
}

void fade_left(SDL_Renderer *r, int x, int y, int w, int h, Color c)
{
    draw_grad(r, grad_h, x, y, w, h, c, SDL_FLIP_HORIZONTAL);
}

void fade_down(SDL_Renderer *r, int x, int y, int w, int h, Color c)
{
    draw_grad(r, grad_v, x, y, w, h, c, SDL_FLIP_NONE);
}

void fade_up(SDL_Renderer *r, int x, int y, int w, int h, Color c)
{
    draw_grad(r, grad_v, x, y, w, h, c, SDL_FLIP_VERTICAL);
}

static TextEntry *text_lookup(TTF_Font *f, const char *txt, Color c)
{
    if (!f || !txt || !txt[0]) return NULL;
    Uint32 col = ((Uint32)c.r << 24) | ((Uint32)c.g << 16) | ((Uint32)c.b << 8) | c.a;
    unsigned long key = hash_string(txt) ^ ((unsigned long)(uintptr_t)f * 31u) ^ ((unsigned long)col * 2654435761u);
    int base = (int)(key % TEXT_CACHE);
    TextEntry *victim = NULL;
    for (int i = 0; i < TEXT_PROBE; i++) {
        TextEntry *e = &cache[(base + i) % TEXT_CACHE];
        if (e->tex && e->key == key && e->font == f && e->color == col && strcmp(e->str, txt) == 0) {
            e->used = frame_no;
            return e;
        }
        if (!victim || !e->tex || (victim->tex && e->used < victim->used)) victim = e;
    }
    SDL_Surface *s = TTF_RenderUTF8_Blended(pick_font(f, txt), txt, (SDL_Color){c.r, c.g, c.b, 255});
    if (!s) return NULL;
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s);
    int w = s->w, h = s->h;
    SDL_FreeSurface(s);
    if (!t) return NULL;
    if (c.a < 255) SDL_SetTextureAlphaMod(t, c.a);
    if (victim->tex) SDL_DestroyTexture(victim->tex);
    free(victim->str);
    victim->key = key;
    victim->font = f;
    victim->color = col;
    victim->str = strdup(txt);
    victim->tex = t;
    victim->w = w;
    victim->h = h;
    victim->used = frame_no;
    return victim;
}

int text_width(TTF_Font *f, const char *txt)
{
    int w = 0, h = 0;
    if (!f || !txt || !txt[0]) return 0;
    TTF_SizeUTF8(pick_font(f, txt), txt, &w, &h);
    return w;
}

int text_height(TTF_Font *f)
{
    return f ? TTF_FontHeight(f) : 0;
}

int draw_text(SDL_Renderer *r, TTF_Font *f, const char *txt, int x, int y, Color c, int align)
{
    TextEntry *e = text_lookup(f, txt, c);
    if (!e) return 0;
    if (align == ALIGN_CENTER) x -= e->w / 2;
    else if (align == ALIGN_RIGHT) x -= e->w;
    SDL_Rect dst = {x, y, e->w, e->h};
    SDL_RenderCopy(r, e->tex, NULL, &dst);
    return e->w;
}

int draw_text_fit(SDL_Renderer *r, TTF_Font *f, const char *txt, int x, int y, int max_w, Color c)
{
    if (!txt || !txt[0]) return 0;
    if (text_width(f, txt) <= max_w) return draw_text(r, f, txt, x, y, c, ALIGN_LEFT);
    char buf[TITLE_LEN * 2];
    copy_text(buf, sizeof(buf), txt);
    size_t len = strlen(buf);
    while (len > 1) {
        len--;
        while (len > 0 && ((unsigned char)buf[len] & 0xC0) == 0x80) len--;
        buf[len] = '\0';
        char tmp[TITLE_LEN * 2 + 4];
        snprintf(tmp, sizeof(tmp), "%s...", buf);
        if (text_width(f, tmp) <= max_w) return draw_text(r, f, tmp, x, y, c, ALIGN_LEFT);
    }
    return 0;
}

int draw_text_wrap(SDL_Renderer *r, TTF_Font *f, const char *txt, int x, int y, int max_w,
                   int max_lines, Color c)
{
    if (!f || !txt || !txt[0]) return 0;
    int lh = TTF_FontLineSkip(f);
    int lines = 0;
    const char *p = txt;
    char line[1024];
    while (*p && lines < max_lines) {
        while (*p == ' ' || *p == '\n') p++;
        size_t best = 0;
        size_t i = 0;
        bool hard = false;
        while (p[i]) {
            size_t j = i;
            while (p[j] && p[j] != ' ' && p[j] != '\n') j++;
            if (j >= sizeof(line) - 4) break;
            memcpy(line, p, j);
            line[j] = '\0';
            if (text_width(f, line) > max_w && best > 0) break;
            best = j;
            if (p[j] == '\n') { hard = true; break; }
            if (!p[j]) break;
            i = j + 1;
        }
        if (best == 0) {
            best = strlen(p) < sizeof(line) - 4 ? strlen(p) : sizeof(line) - 4;
        }
        memcpy(line, p, best);
        line[best] = '\0';
        bool last = lines == max_lines - 1 && p[best] && !hard;
        if (last) draw_text_fit(r, f, p, x, y + lines * lh, max_w, c);
        else draw_text(r, f, line, x, y + lines * lh, c, ALIGN_LEFT);
        lines++;
        p += best;
    }
    return lines * lh;
}

void draw_cover(SDL_Renderer *r, SDL_Texture *t, int x, int y, int w, int h, Uint8 alpha)
{
    if (!t) return;
    int tw, th;
    SDL_QueryTexture(t, NULL, NULL, &tw, &th);
    if (tw <= 0 || th <= 0) return;
    SDL_Rect src = {0, 0, tw, th};
    double ta = (double)tw / th, da = (double)w / h;
    if (ta > da) {
        src.w = (int)(th * da);
        src.x = (tw - src.w) / 2;
    } else {
        src.h = (int)(tw / da);
        src.y = (th - src.h) / 4;
    }
    SDL_SetTextureAlphaMod(t, alpha);
    SDL_Rect dst = {x, y, w, h};
    SDL_RenderCopy(r, t, &src, &dst);
}

void draw_contain(SDL_Renderer *r, SDL_Texture *t, int x, int y, int w, int h, int align, Uint8 alpha)
{
    if (!t) return;
    int tw, th;
    SDL_QueryTexture(t, NULL, NULL, &tw, &th);
    if (tw <= 0 || th <= 0) return;
    double s = fmin((double)w / tw, (double)h / th);
    int dw = (int)(tw * s), dh = (int)(th * s);
    int dx = align == ALIGN_CENTER ? x + (w - dw) / 2 : align == ALIGN_RIGHT ? x + w - dw : x;
    SDL_SetTextureAlphaMod(t, alpha);
    SDL_Rect dst = {dx, y + h - dh, dw, dh};
    SDL_RenderCopy(r, t, NULL, &dst);
}

void draw_placeholder(SDL_Renderer *r, const char *seed, int x, int y, int w, int h)
{
    unsigned long hs = hash_string(seed ? seed : "");
    Color a = {(Uint8)(30 + (hs & 31)), (Uint8)(30 + ((hs >> 8) & 31)), (Uint8)(38 + ((hs >> 16) & 47)), 255};
    fill_rect(r, x, y, w, h, a);
    fade_down(r, x, y + h / 3, w, h - h / 3, (Color){0, 0, 0, 160});
}

void draw_play_icon(SDL_Renderer *r, int x, int y, int size, Color c)
{
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    for (int i = 0; i < size; i++) {
        int half = (i <= size / 2) ? i : size - i;
        int len = half * 2 * 9 / 10;
        SDL_RenderDrawLine(r, x, y + i, x + len, y + i);
    }
}

void draw_search_icon(SDL_Renderer *r, int x, int y, int size, Color c)
{
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    double rad = size * 0.36;
    double cx = x + rad + 1, cy = y + rad + 1;
    for (int t = 0; t < 2; t++)
        for (int i = 0; i < 64; i++) {
            double a0 = i * 2 * M_PI / 64, a1 = (i + 1) * 2 * M_PI / 64;
            SDL_RenderDrawLine(r, (int)(cx + (rad - t) * cos(a0)), (int)(cy + (rad - t) * sin(a0)),
                               (int)(cx + (rad - t) * cos(a1)), (int)(cy + (rad - t) * sin(a1)));
        }
    for (int t = -1; t <= 1; t++)
        SDL_RenderDrawLine(r, (int)(cx + rad * 0.7) + t, (int)(cy + rad * 0.7),
                           x + size + t, y + size);
}

void draw_progress(SDL_Renderer *r, int x, int y, int w, int h, double frac)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    fill_rect(r, x, y, w, h, (Color){90, 90, 90, 220});
    fill_rect(r, x, y, (int)(w * frac), h, C_RED);
}

bool point_in(int px, int py, int x, int y, int w, int h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}
