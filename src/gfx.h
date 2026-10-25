#ifndef CWATCH_GFX_H
#define CWATCH_GFX_H

#include "common.h"

typedef struct {
    Uint8 r, g, b, a;
} Color;

#define C_BG ((Color){20, 20, 20, 255})
#define C_TEXT ((Color){245, 245, 241, 255})
#define C_DIM ((Color){170, 170, 170, 255})
#define C_FAINT ((Color){110, 110, 110, 255})
#define C_RED ((Color){229, 9, 20, 255})
#define C_GREEN ((Color){70, 211, 105, 255})
#define C_WHITE ((Color){255, 255, 255, 255})
#define C_BLACK ((Color){0, 0, 0, 255})

enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

bool gfx_init(SDL_Renderer *r, UI *ui);
void gfx_shutdown(UI *ui);
void gfx_frame_end(void);

void fill_rect(SDL_Renderer *r, int x, int y, int w, int h, Color c);
void fill_round(SDL_Renderer *r, int x, int y, int w, int h, int rad, Color c);
void stroke_round(SDL_Renderer *r, int x, int y, int w, int h, int rad, int thick, Color c);
void fade_left(SDL_Renderer *r, int x, int y, int w, int h, Color c);
void fade_down(SDL_Renderer *r, int x, int y, int w, int h, Color c);
void fade_up(SDL_Renderer *r, int x, int y, int w, int h, Color c);

int text_width(TTF_Font *f, const char *txt);
int text_height(TTF_Font *f);
int draw_text(SDL_Renderer *r, TTF_Font *f, const char *txt, int x, int y, Color c, int align);
int draw_text_fit(SDL_Renderer *r, TTF_Font *f, const char *txt, int x, int y, int max_w, Color c);
int draw_text_wrap(SDL_Renderer *r, TTF_Font *f, const char *txt, int x, int y, int max_w,
                   int max_lines, Color c);

void draw_cover(SDL_Renderer *r, SDL_Texture *t, int x, int y, int w, int h, Uint8 alpha);
void draw_contain(SDL_Renderer *r, SDL_Texture *t, int x, int y, int w, int h, int align, Uint8 alpha);
void draw_placeholder(SDL_Renderer *r, const char *seed, int x, int y, int w, int h);
void draw_play_icon(SDL_Renderer *r, int x, int y, int size, Color c);
void draw_search_icon(SDL_Renderer *r, int x, int y, int size, Color c);
void draw_progress(SDL_Renderer *r, int x, int y, int w, int h, double frac);
bool point_in(int px, int py, int x, int y, int w, int h);

#endif
