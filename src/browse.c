#include "app.h"
#include "images.h"
#include "net.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static App *G;

static const char *tab_names[TAB_COUNT] = {
    "", "Home", "Shows", "Movies", "Anime", "My Library", "Nyaa", "Downloads"
};

void app_toast(App *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->toast, sizeof(a->toast), fmt, ap);
    va_end(ap);
    a->toast_until = SDL_GetTicks() + 3500;
}

static int hero_h(void)
{
    int h = (int)(win_h * 0.60);
    return h < 360 ? 360 : h;
}

static int poster_w(void)
{
    int w = (win_w - 120) / 7;
    if (w < 130) w = 130;
    if (w > 230) w = 230;
    return w;
}

static void card_size(const Row *row, int *w, int *h)
{
    int pw = poster_w();
    if (row->style == ROW_WIDE) {
        *w = pw * 16 / 9 + 20;
        *h = *w * 9 / 16;
    } else {
        *w = pw;
        *h = pw * 3 / 2;
    }
}

static int row_height(const Row *row)
{
    int w, h;
    card_size(row, &w, &h);
    return h + 74;
}

static void row_clear(Row *row)
{
    free(row->items);
    memset(row, 0, sizeof(*row));
}

static Row *row_new(App *a, const char *title, int style)
{
    if (a->row_count >= MAX_ROWS) return NULL;
    Row *row = &a->rows[a->row_count++];
    memset(row, 0, sizeof(*row));
    copy_text(row->title, sizeof(row->title), title);
    row->style = style;
    return row;
}

static void parse_list(Job *job)
{
    job->result = job->ok ? parse_meta_list(job->data) : NULL;
}

static void apply_list(Job *job)
{
    MetaList *l = job->result;
    if (G && job->gen == G->rows_gen && job->tag < G->row_count) {
        Row *row = &G->rows[job->tag];
        row->loading = false;
        if (l && l->count > 0) {
            free(row->items);
            row->items = l->items;
            row->count = l->count > 40 ? 40 : l->count;
            l->items = NULL;
        } else {
            row->failed = true;
        }
    }
    meta_list_free(l);
}

static void row_remote(App *a, const char *title, const char *base, const char *type,
                       const char *catalog, const char *genre, const char *search)
{
    Row *row = row_new(a, title, ROW_POSTER);
    if (!row) return;
    char url[URL_LEN];
    catalog_url(url, sizeof(url), base, type, catalog, genre, search);
    Job *job = job_new(url, search ? 3600 : 6 * 3600, parse_list, apply_list);
    if (!job) return;
    row->loading = true;
    job->tag = a->row_count - 1;
    job->gen = a->rows_gen;
    net_submit(job);
}

static void local_meta(App *a, int c, Meta *m)
{
    Collection *col = &a->lib->cols[c];
    memset(m, 0, sizeof(*m));
    if (col->matched && col->match.id[0]) {
        *m = col->match;
    } else {
        copy_text(m->name, sizeof(m->name), col->name);
        copy_text(m->type, sizeof(m->type), col->count > 1 ? "series" : "movie");
    }
    if (!m->name[0]) copy_text(m->name, sizeof(m->name), col->name);
    m->local_col = c;
    m->local_item = library_next_item(a->lib, c);
    m->hist = -1;
    m->progress = -1;
    if (col->count > 1)
        snprintf(m->sub, sizeof(m->sub), "%d episodes downloaded", col->count);
    else
        copy_text(m->sub, sizeof(m->sub), "Downloaded");
}

static int cmp_recent(const void *x, const void *y)
{
    const Meta *a = x, *b = y;
    return (a->progress < b->progress) - (a->progress > b->progress);
}

static void build_continue(App *a, Row *row)
{
    int cap = a->lib->col_count + history_count();
    row->items = calloc(cap ? (size_t)cap : 1, sizeof(Meta));
    if (!row->items) return;
    for (int c = 0; c < a->lib->col_count; c++) {
        Collection *col = &a->lib->cols[c];
        if (!col->last_played || col->count == 0) continue;
        Meta *m = &row->items[row->count++];
        local_meta(a, c, m);
        int it = m->local_item;
        if (it >= 0) {
            LocalItem *li = &a->lib->items[it];
            char clock[32];
            format_clock(li->progress_sec, clock, sizeof(clock));
            if (col->count > 1)
                snprintf(m->sub, sizeof(m->sub), "S%d:E%d  ·  %s", li->season, li->episode,
                         li->has_progress ? clock : "next");
            else
                snprintf(m->sub, sizeof(m->sub), "Resume at %s", clock);
        }
        m->progress = (double)col->last_played;
    }
    for (int i = 0; i < history_count(); i++) {
        const HistoryEntry *h = history_get(i);
        if (library_find_by_name(a->lib, h->name) >= 0) continue;
        Meta *m = &row->items[row->count++];
        memset(m, 0, sizeof(*m));
        copy_text(m->id, sizeof(m->id), h->meta_id);
        copy_text(m->type, sizeof(m->type), h->type);
        copy_text(m->name, sizeof(m->name), h->name);
        copy_text(m->poster, sizeof(m->poster), h->poster);
        copy_text(m->background, sizeof(m->background), h->background);
        if (h->season || h->episode)
            snprintf(m->sub, sizeof(m->sub), "S%d:E%d  ·  %s", h->season, h->episode, h->ep_title);
        m->local_col = m->local_item = -1;
        m->hist = i;
        m->progress = (double)h->when;
    }
    qsort(row->items, (size_t)row->count, sizeof(Meta), cmp_recent);
    for (int i = 0; i < row->count; i++) row->items[i].progress = -1;
}

static void build_local(App *a, Row *row, int min_eps, int max_eps, const char *filter)
{
    row->items = calloc(a->lib->col_count ? (size_t)a->lib->col_count : 1, sizeof(Meta));
    if (!row->items) return;
    for (int c = 0; c < a->lib->col_count; c++) {
        Collection *col = &a->lib->cols[c];
        if (col->count < min_eps || col->count > max_eps) continue;
        if (filter && filter[0] && !strcasestr(col->name, filter) &&
            !(col->matched && strcasestr(col->match.name, filter)))
            continue;
        local_meta(a, c, &row->items[row->count++]);
    }
}

static void add_local_rows(App *a)
{
    if (a->tab == TAB_HOME || a->tab == TAB_LIBRARY) {
        Row *row = row_new(a, "Continue Watching", ROW_WIDE);
        if (row) {
            build_continue(a, row);
            if (row->count == 0) {
                row_clear(row);
                a->row_count--;
            }
        }
    }
    if (a->tab == TAB_HOME) {
        Row *row = row_new(a, "My Library", ROW_POSTER);
        if (row) {
            build_local(a, row, 1, 1 << 30, NULL);
            if (row->count == 0) {
                row_clear(row);
                a->row_count--;
            }
        }
    } else if (a->tab == TAB_LIBRARY) {
        Row *row = row_new(a, "Shows", ROW_POSTER);
        if (row) build_local(a, row, 2, 1 << 30, NULL);
        if (row && row->count == 0) {
            row_clear(row);
            a->row_count--;
        }
        row = row_new(a, "Movies & Videos", ROW_POSTER);
        if (row) build_local(a, row, 1, 1, NULL);
        if (row && row->count == 0) {
            row_clear(row);
            a->row_count--;
        }
    } else if (a->tab == TAB_SEARCH && a->query[0]) {
        Row *row = row_new(a, "In Your Library", ROW_POSTER);
        if (row) {
            build_local(a, row, 1, 1 << 30, a->query);
            if (row->count == 0) {
                row_clear(row);
                a->row_count--;
            }
        }
    }
}

void browse_build(App *a)
{
    G = a;
    for (int i = 0; i < a->row_count; i++) row_clear(&a->rows[i]);
    a->row_count = 0;
    a->rows_gen++;
    a->row_anim = 0;
    if (a->focus >= 0) a->focus = FOCUS_HERO;

    add_local_rows(a);
    static const char *show_genres[] = {"Action", "Comedy", "Drama", "Crime", "Sci-Fi", "Mystery", "Fantasy", "Documentary", NULL};
    static const char *movie_genres[] = {"Action", "Comedy", "Thriller", "Sci-Fi", "Horror", "Romance", "Animation", "Family", NULL};
    static const char *anime_genres[] = {"Action", "Romance", "Comedy", "Fantasy", "Psychological", "Sports", "Slice of Life", NULL};
    char title[64];

    switch (a->tab) {
    case TAB_HOME:
        row_remote(a, "Popular Shows", CINEMETA, "series", "top", NULL, NULL);
        row_remote(a, "Trending Anime", KITSU, "anime", "kitsu-anime-trending", NULL, NULL);
        row_remote(a, "Popular Movies", CINEMETA, "movie", "top", NULL, NULL);
        row_remote(a, "Top Rated Shows", CINEMETA, "series", "imdbRating", NULL, NULL);
        row_remote(a, "Airing Anime", KITSU, "anime", "kitsu-anime-airing", NULL, NULL);
        row_remote(a, "Top Rated Movies", CINEMETA, "movie", "imdbRating", NULL, NULL);
        break;
    case TAB_SHOWS:
        row_remote(a, "Popular Shows", CINEMETA, "series", "top", NULL, NULL);
        row_remote(a, "Top Rated", CINEMETA, "series", "imdbRating", NULL, NULL);
        for (int i = 0; show_genres[i]; i++) {
            snprintf(title, sizeof(title), "%s Shows", show_genres[i]);
            row_remote(a, title, CINEMETA, "series", "top", show_genres[i], NULL);
        }
        break;
    case TAB_MOVIES:
        row_remote(a, "Popular Movies", CINEMETA, "movie", "top", NULL, NULL);
        row_remote(a, "Top Rated", CINEMETA, "movie", "imdbRating", NULL, NULL);
        for (int i = 0; movie_genres[i]; i++) {
            snprintf(title, sizeof(title), "%s Movies", movie_genres[i]);
            row_remote(a, title, CINEMETA, "movie", "top", movie_genres[i], NULL);
        }
        break;
    case TAB_ANIME:
        row_remote(a, "Trending Now", KITSU, "anime", "kitsu-anime-trending", NULL, NULL);
        row_remote(a, "Airing This Season", KITSU, "anime", "kitsu-anime-airing", NULL, NULL);
        row_remote(a, "Most Popular", KITSU, "anime", "kitsu-anime-popular", NULL, NULL);
        row_remote(a, "Top Rated", KITSU, "anime", "kitsu-anime-rating", NULL, NULL);
        for (int i = 0; anime_genres[i]; i++)
            row_remote(a, anime_genres[i], KITSU, "anime", "kitsu-anime-popular", anime_genres[i], NULL);
        break;
    case TAB_SEARCH:
        if (a->query[0]) {
            row_remote(a, "Shows", CINEMETA, "series", "top", NULL, a->query);
            row_remote(a, "Anime", KITSU, "anime", "kitsu-anime-list", NULL, a->query);
            row_remote(a, "Movies", CINEMETA, "movie", "top", NULL, a->query);
        }
        break;
    default:
        break;
    }
}

void browse_refresh_local(App *a)
{
    for (int i = 0; i < a->row_count; i++) {
        Row *row = &a->rows[i];
        int sel = row->sel;
        if (strcmp(row->title, "Continue Watching") == 0) {
            free(row->items);
            row->items = NULL;
            row->count = 0;
            build_continue(a, row);
        } else if (strcmp(row->title, "My Library") == 0 || strcmp(row->title, "In Your Library") == 0) {
            free(row->items);
            row->items = NULL;
            row->count = 0;
            build_local(a, row, 1, 1 << 30, strcmp(row->title, "In Your Library") == 0 ? a->query : NULL);
        } else if (a->tab == TAB_LIBRARY && (strcmp(row->title, "Shows") == 0 || strcmp(row->title, "Movies & Videos") == 0)) {
            free(row->items);
            row->items = NULL;
            row->count = 0;
            if (row->title[0] == 'S') build_local(a, row, 2, 1 << 30, NULL);
            else build_local(a, row, 1, 1, NULL);
        } else {
            continue;
        }
        row->sel = sel < row->count ? sel : (row->count ? row->count - 1 : 0);
    }
}

static bool hero_requested;

static void parse_hero(Job *job)
{
    job->result = job->ok ? parse_meta_full(job->data) : NULL;
}

static void apply_hero(Job *job)
{
    MetaFull *mf = job->result;
    if (G && mf && strcmp(G->hero.id, job->text) == 0) {
        Meta *h = &G->hero;
        const Meta *f = &mf->meta;
        if (!h->desc[0]) copy_text(h->desc, sizeof(h->desc), f->desc);
        if (!h->logo[0]) copy_text(h->logo, sizeof(h->logo), f->logo);
        if (!h->genres[0]) copy_text(h->genres, sizeof(h->genres), f->genres);
        if (!h->rating[0]) copy_text(h->rating, sizeof(h->rating), f->rating);
        if (!h->runtime[0]) copy_text(h->runtime, sizeof(h->runtime), f->runtime);
        if (!h->background[0]) copy_text(h->background, sizeof(h->background), f->background);
    }
    meta_full_free(mf);
}

static void enrich_hero(App *a)
{
    Meta *h = &a->hero;
    if (hero_requested || !h->id[0] || (h->desc[0] && h->logo[0])) return;
    if (SDL_GetTicks() - a->hero_changed < 300) return;
    hero_requested = true;
    char url[URL_LEN];
    meta_url(url, sizeof(url), h->type, h->id);
    Job *job = job_new(url, 6 * 3600, parse_hero, apply_hero);
    if (!job) return;
    copy_text(job->text, sizeof(job->text), h->id);
    net_submit(job);
}

static const Meta *focused_meta(App *a)
{
    if (a->focus >= 0 && a->focus < a->row_count) {
        Row *row = &a->rows[a->focus];
        if (row->sel >= 0 && row->sel < row->count) return &row->items[row->sel];
    }
    for (int i = 0; i < a->row_count; i++)
        if (a->rows[i].count > 0) return &a->rows[i].items[a->rows[i].sel];
    return NULL;
}

static SDL_Texture *meta_backdrop(App *a, const Meta *m)
{
    if (m->background[0]) return img_get(m->background);
    if (m->local_item >= 0 && m->local_item < a->lib->item_count)
        return img_thumb(a->lib->items[m->local_item].path);
    return m->poster[0] ? img_get(m->poster) : NULL;
}

static SDL_Texture *meta_poster(App *a, const Meta *m)
{
    if (m->poster[0]) return img_get(m->poster);
    if (m->local_item >= 0 && m->local_item < a->lib->item_count)
        return img_thumb(a->lib->items[m->local_item].path);
    return NULL;
}

static void meta_line(const Meta *m, char *out, size_t len)
{
    char rating[40] = "";
    if (m->rating[0] && strcmp(m->rating, "0") != 0) snprintf(rating, sizeof(rating), "IMDb %s", m->rating);
    const char *parts[] = {m->year, rating, m->runtime, m->genres};
    out[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < 4; i++) {
        if (!parts[i][0]) continue;
        int w = snprintf(out + used, len - used, "%s%s", used ? "     " : "", parts[i]);
        if (w < 0 || (size_t)w >= len - used) break;
        used += (size_t)w;
    }
}

static void draw_button(App *a, int x, int y, const char *label, bool primary, bool focused, bool icon, int *w_out)
{
    int tw = text_width(a->ui.mdb, label);
    int w = tw + (icon ? 76 : 52), h = 50;
    Color bg = primary ? C_WHITE : (Color){109, 109, 110, 180};
    Color fg = primary ? C_BLACK : C_WHITE;
    if (focused) {
        fill_round(a->r, x - 4, y - 4, w + 8, h + 8, 10, (Color){255, 255, 255, 90});
        if (!primary) bg = (Color){150, 150, 150, 220};
    }
    fill_round(a->r, x, y, w, h, 6, bg);
    int tx = x + 26;
    if (icon) {
        draw_play_icon(a->r, x + 24, y + 14, 22, fg);
        tx = x + 56;
    }
    draw_text(a->r, a->ui.mdb, label, tx, y + (h - text_height(a->ui.mdb)) / 2, fg, ALIGN_LEFT);
    if (w_out) *w_out = w;
}

static void hero_buttons(App *a, const Meta *m, const char **play, const char **info)
{
    static char label[64];
    *info = "More Info";
    *play = "Play";
    if (m->local_item >= 0 && m->local_item < a->lib->item_count) {
        LocalItem *li = &a->lib->items[m->local_item];
        if (li->has_progress) *play = "Resume";
        if (a->lib->cols[m->local_col].count > 1) {
            snprintf(label, sizeof(label), "%s S%d:E%d", *play, li->season, li->episode);
            *play = label;
        }
    }
}

static void render_hero(App *a)
{
    int H = hero_h();
    const Meta *m = focused_meta(a);
    if (a->tab == TAB_SEARCH) {
        (void)H;
        int x = 60, y = NAV_H + 40, w = win_w - 120;
        fill_round(a->r, x, y, w, 64, 8, (Color){45, 45, 45, 255});
        if (a->focus == FOCUS_HERO) stroke_round(a->r, x, y, w, 64, 8, 2, C_WHITE);
        draw_search_icon(a->r, x + 22, y + 20, 24, C_DIM);
        char q[TITLE_LEN + 4];
        snprintf(q, sizeof(q), "%s%s", a->query, a->focus == FOCUS_HERO && (SDL_GetTicks() / 500) % 2 ? "|" : "");
        if (a->query[0]) draw_text(a->r, a->ui.lg, q, x + 64, y + 16, C_TEXT, ALIGN_LEFT);
        else draw_text(a->r, a->ui.md, "Search shows, movies and anime", x + 64, y + 20, C_FAINT, ALIGN_LEFT);
        if (!a->query[0])
            draw_text(a->r, a->ui.sm, "Type to search. Results come from Cinemeta, Kitsu and your library.",
                      x, y + 84, C_FAINT, ALIGN_LEFT);
        return;
    }
    if (!m) {
        fill_rect(a->r, 0, 0, win_w, H, C_BG);
        const char *msg = a->tab == TAB_LIBRARY ? "Your library is empty. Download something or press D to add a folder."
                                                : "Loading...";
        draw_text(a->r, a->ui.lg, msg, 60, H / 2, C_DIM, ALIGN_LEFT);
        return;
    }
    if (strcmp(a->hero.name, m->name) != 0 || strcmp(a->hero.id, m->id) != 0) {
        a->hero = *m;
        a->hero_changed = SDL_GetTicks();
        hero_requested = false;
    } else if (strcmp(a->hero.sub, m->sub) != 0) {
        copy_text(a->hero.sub, sizeof(a->hero.sub), m->sub);
    }
    enrich_hero(a);
    Uint32 age = SDL_GetTicks() - a->hero_changed;
    Uint8 alpha = age > 350 ? 255 : (Uint8)(age * 255 / 350);
    m = &a->hero;

    fill_rect(a->r, 0, 0, win_w, H, C_BLACK);
    SDL_Texture *bg = meta_backdrop(a, m);
    int bx = win_w / 4;
    if (bg) draw_cover(a->r, bg, bx, 0, win_w - bx, H, alpha);
    else draw_placeholder(a->r, m->name, bx, 0, win_w - bx, H);
    fill_rect(a->r, 0, 0, bx, H, C_BLACK);
    fade_left(a->r, bx, 0, (win_w - bx) * 2 / 3, H, (Color){0, 0, 0, 255});
    fade_down(a->r, 0, H - H / 3, win_w, H / 3 + 1, (Color){20, 20, 20, 255});
    fade_up(a->r, 0, 0, win_w, 140, (Color){0, 0, 0, 200});

    int x = 60, y = NAV_H + 30, max_w = (int)(win_w * 0.40);
    SDL_Texture *logo = m->logo[0] ? img_get(m->logo) : NULL;
    int title_h = (int)(H * 0.22);
    if (logo) {
        draw_contain(a->r, logo, x, y, max_w, title_h, ALIGN_LEFT, alpha);
        y += title_h + 18;
    } else {
        TTF_Font *f = text_width(a->ui.hero, m->name) > max_w ? a->ui.xl : a->ui.hero;
        y += draw_text_wrap(a->r, f, m->name, x, y + 10, max_w, 2, C_WHITE) + 22;
    }
    char line[400];
    meta_line(m, line, sizeof(line));
    if (m->sub[0]) {
        draw_text(a->r, a->ui.mdb, m->sub, x, y, C_TEXT, ALIGN_LEFT);
        y += 30;
    }
    if (line[0]) {
        draw_text_fit(a->r, a->ui.sm, line, x, y, max_w, C_DIM);
        y += 30;
    }
    int lh = TTF_FontLineSkip(a->ui.md);
    int room = (H - 116 - 18 - y) / (lh > 0 ? lh : 1);
    if (room > 3) room = 3;
    if (m->desc[0] && room > 0) y += draw_text_wrap(a->r, a->ui.md, m->desc, x, y, max_w, room, C_TEXT) + 18;
    else y += 8;

    int by = y < H - 116 ? y : H - 116;
    const char *play, *info;
    hero_buttons(a, m, &play, &info);
    int w1;
    draw_button(a, x, by, play, true, a->focus == FOCUS_HERO && a->hero_button == 0, true, &w1);
    draw_button(a, x + w1 + 14, by, info, false, a->focus == FOCUS_HERO && a->hero_button == 1, false, NULL);
}

static int rows_top(void)
{
    if (G && G->tab == TAB_SEARCH) return NAV_H + 150;
    return hero_h() - 46;
}

static void render_card(App *a, const Row *row, const Meta *m, int x, int y, int w, int h, bool sel)
{
    SDL_Texture *t = row->style == ROW_WIDE ? meta_backdrop(a, m) : meta_poster(a, m);
    if (sel) fill_round(a->r, x - 5, y - 5, w + 10, h + 10, 8, C_WHITE);
    if (t) draw_cover(a->r, t, x, y, w, h, 255);
    else draw_placeholder(a->r, m->name, x, y, w, h);

    bool show_name = !t || row->style == ROW_WIDE || !m->poster[0];
    if (show_name) {
        fade_down(a->r, x, y + h / 2, w, h - h / 2, (Color){0, 0, 0, 230});
        int ty = y + h - (row->style == ROW_WIDE ? 58 : 50);
        draw_text_wrap(a->r, a->ui.mdb, m->name, x + 12, ty, w - 24, 1, C_WHITE);
        if (m->sub[0]) draw_text_fit(a->r, a->ui.xs, m->sub, x + 12, ty + 26, w - 24, C_DIM);
    }
    if (m->local_col >= 0 && row->style == ROW_POSTER) {
        fill_round(a->r, x + 8, y + 8, 24, 24, 4, C_RED);
        fill_rect(a->r, x + 19, y + 13, 3, 10, C_WHITE);
        for (int k = 0; k < 5; k++) fill_rect(a->r, x + 15 + k, y + 19 + k, 11 - 2 * k, 1, C_WHITE);
        fill_rect(a->r, x + 14, y + 26, 13, 2, C_WHITE);
    }
    if (m->local_item >= 0 && row->style == ROW_WIDE) {
        LocalItem *li = &a->lib->items[m->local_item];
        if (li->has_progress && li->duration > 0) draw_progress(a->r, x + 12, y + h - 10, w - 24, 4, li->progress_sec / li->duration);
    }
}

static void render_rows(App *a)
{
    int top = rows_top();
    SDL_Rect clip = {0, top - 10, win_w, win_h - top + 10};
    SDL_RenderSetClipRect(a->r, &clip);

    float target = a->focus > 0 ? (float)a->focus : 0.0f;
    a->row_anim += (target - a->row_anim) * 0.22f;
    if (fabsf(target - a->row_anim) < 0.01f) a->row_anim = target;

    int y = top;
    int base = (int)a->row_anim;
    float frac = a->row_anim - base;
    for (int i = 0; i < base && i < a->row_count; i++) y -= row_height(&a->rows[i]);
    if (base < a->row_count) y -= (int)(frac * row_height(&a->rows[base]));

    for (int i = 0; i < a->row_count; i++) {
        Row *row = &a->rows[i];
        int rh = row_height(row);
        if (y + rh < top - 10) {
            y += rh;
            continue;
        }
        if (y > win_h) break;
        bool focused_row = a->focus == i;
        draw_text(a->r, a->ui.lg, row->title, 60, y, focused_row ? C_WHITE : C_TEXT, ALIGN_LEFT);
        int cw, ch;
        card_size(row, &cw, &ch);
        int gap = 12;
        int cy = y + 40;
        if (row->loading) {
            for (int k = 0; k * (cw + gap) < win_w; k++)
                fill_round(a->r, 60 + k * (cw + gap), cy, cw, ch, 4, (Color){38, 38, 38, 255});
        } else if (row->count == 0) {
            draw_text(a->r, a->ui.sm, row->failed ? "Could not load this row (offline?)" : "Nothing here yet",
                      60, cy + 10, C_FAINT, ALIGN_LEFT);
        }
        int visible = (win_w - 120) / (cw + gap);
        float max_scroll = (float)(row->count - visible);
        if (max_scroll < 0) max_scroll = 0;
        float want = (float)row->sel - 1.0f;
        if (want < 0) want = 0;
        if (want > max_scroll) want = max_scroll;
        row->scroll += (want - row->scroll) * 0.25f;
        for (int k = 0; k < row->count; k++) {
            int cx = 60 + (int)((k - row->scroll) * (cw + gap));
            if (cx + cw < -20 || cx > win_w + 20) continue;
            render_card(a, row, &row->items[k], cx, cy, cw, ch, focused_row && row->sel == k);
        }
        int pre_l = (int)row->scroll - 2, pre_r = (int)row->scroll + visible + 3;
        for (int k = pre_l; k < pre_r; k++)
            if (k >= 0 && k < row->count && row->style == ROW_POSTER && row->items[k].poster[0])
                img_prefetch(row->items[k].poster);
        y += rh;
    }
    SDL_RenderSetClipRect(a->r, NULL);
}

void nav_render(App *a)
{
    fade_up(a->r, 0, 0, win_w, NAV_H + 40, (Color){0, 0, 0, a->tab == TAB_SEARCH ? 0 : 160});
    draw_text(a->r, a->ui.xl, "CWATCH", 60, 18, C_RED, ALIGN_LEFT);
    int x = 60 + text_width(a->ui.xl, "CWATCH") + 40;
    for (int t = 0; t < TAB_COUNT; t++) {
        char label[64];
        if (t == TAB_DOWNLOADS && downloads_active() > 0)
            snprintf(label, sizeof(label), "Downloads (%d)", downloads_active());
        else
            copy_text(label, sizeof(label), tab_names[t]);
        int w = t == TAB_SEARCH ? 44 : text_width(a->ui.md, label) + 30;
        bool active = a->tab == t;
        bool foc = a->focus == FOCUS_NAV && a->nav_sel == t;
        if (active) fill_round(a->r, x, 22, w, 38, 19, (Color){235, 235, 235, 255});
        else if (foc) fill_round(a->r, x, 22, w, 38, 19, (Color){255, 255, 255, 50});
        if (foc) stroke_round(a->r, x - 2, 20, w + 4, 42, 21, 2, C_WHITE);
        Color fg = active ? C_BLACK : C_TEXT;
        if (t == TAB_SEARCH) draw_search_icon(a->r, x + 12, 31, 19, fg);
        else draw_text(a->r, a->ui.md, label, x + 15, 29, fg, ALIGN_LEFT);
        x += w + 8;
    }
    char pl[64];
    snprintf(pl, sizeof(pl), "player: %s", a->players[a->player_idx].name);
    if (x + text_width(a->ui.xs, pl) + 30 < win_w)
        draw_text(a->r, a->ui.xs, pl, win_w - 40, 34, C_FAINT, ALIGN_RIGHT);
}

int nav_hit(App *a, int mx, int my)
{
    if (my < 18 || my > 64) return -1;
    int x = 60 + text_width(a->ui.xl, "CWATCH") + 40;
    for (int t = 0; t < TAB_COUNT; t++) {
        char label[64];
        if (t == TAB_DOWNLOADS && downloads_active() > 0)
            snprintf(label, sizeof(label), "Downloads (%d)", downloads_active());
        else
            copy_text(label, sizeof(label), tab_names[t]);
        int w = t == TAB_SEARCH ? 44 : text_width(a->ui.md, label) + 30;
        if (mx >= x && mx < x + w) return t;
        x += w + 8;
    }
    return -1;
}

void browse_render(App *a)
{
    fill_rect(a->r, 0, 0, win_w, win_h, C_BG);
    render_hero(a);
    render_rows(a);
}

void set_tab(App *a, int tab)
{
    if (tab < 0 || tab >= TAB_COUNT) return;
    a->tab = tab;
    a->nav_sel = tab;
    a->focus = tab == TAB_SEARCH ? FOCUS_HERO : (a->focus == FOCUS_NAV ? FOCUS_NAV : FOCUS_HERO);
    a->hero_button = 0;
    a->typing = tab == TAB_SEARCH;
    if (tab == TAB_NYAA) {
        a->typing = a->nyaa == NULL;
        a->focus = FOCUS_HERO;
    }
    if (tab == TAB_DOWNLOADS) a->focus = FOCUS_HERO;
    if (tab != TAB_NYAA && tab != TAB_DOWNLOADS) browse_build(a);
}

void play_meta(App *a, const Meta *m)
{
    if (m->local_item >= 0 && m->local_item < a->lib->item_count) {
        LocalItem *li = &a->lib->items[m->local_item];
        char title[TITLE_LEN * 2];
        if (a->lib->cols[li->collection].count > 1)
            snprintf(title, sizeof(title), "%s  S%d:E%d", m->name, li->season, li->episode);
        else
            copy_text(title, sizeof(title), m->name);
        play_file(li->path, &a->players[a->player_idx], title);
        app_toast(a, "Playing %s", title);
        return;
    }
    if (m->hist >= 0) {
        const HistoryEntry *h = history_get(m->hist);
        if (h && h->ep_id[0]) {
            char heading[TITLE_LEN * 2], play[TITLE_LEN], q[TITLE_LEN];
            snprintf(heading, sizeof(heading), "%s  ·  S%d:E%d  %s", h->name, h->season, h->episode, h->ep_title);
            snprintf(play, sizeof(play), "%s  S%d:E%d", h->name, h->season, h->episode);
            snprintf(q, sizeof(q), "%s %02d", h->name, h->episode);
            sources_open(a, "series", h->ep_id, heading, h->name, play, q, h->season, h->episode, h);
            return;
        }
    }
    if (!meta_is_series(m) && m->id[0]) {
        HistoryEntry h = {0};
        copy_text(h.meta_id, sizeof(h.meta_id), m->id);
        copy_text(h.type, sizeof(h.type), "movie");
        copy_text(h.name, sizeof(h.name), m->name);
        copy_text(h.poster, sizeof(h.poster), m->poster);
        copy_text(h.background, sizeof(h.background), m->background);
        char q[TITLE_LEN];
        snprintf(q, sizeof(q), "%s %s", m->name, m->year);
        sources_open(a, "movie", m->id, m->name, m->name, m->name, q, 0, 0, &h);
        return;
    }
    open_meta(a, m);
}

void open_meta(App *a, const Meta *m)
{
    detail_open_meta(a, m);
}

static void activate(App *a)
{
    const Meta *m = focused_meta(a);
    if (a->focus == FOCUS_HERO) {
        if (!m) return;
        if (a->hero_button == 0) play_meta(a, m);
        else open_meta(a, m);
        return;
    }
    if (a->focus >= 0 && m) {
        Row *row = &a->rows[a->focus];
        if (row->style == ROW_WIDE) play_meta(a, m);
        else open_meta(a, m);
    }
}

bool browse_key(App *a, SDL_Keycode key)
{
    Row *row = a->focus >= 0 && a->focus < a->row_count ? &a->rows[a->focus] : NULL;
    switch (key) {
    case SDLK_UP:
        if (a->focus > 0) a->focus--;
        else if (a->focus == 0) a->focus = FOCUS_HERO;
        else if (a->focus == FOCUS_HERO) a->focus = FOCUS_NAV;
        return true;
    case SDLK_DOWN:
        if (a->focus == FOCUS_NAV) a->focus = FOCUS_HERO;
        else if (a->focus < a->row_count - 1) a->focus++;
        return true;
    case SDLK_LEFT:
        if (a->focus == FOCUS_NAV && a->nav_sel > 0) a->nav_sel--;
        else if (a->focus == FOCUS_HERO) a->hero_button = 0;
        else if (row && row->sel > 0) row->sel--;
        return true;
    case SDLK_RIGHT:
        if (a->focus == FOCUS_NAV && a->nav_sel < TAB_COUNT - 1) a->nav_sel++;
        else if (a->focus == FOCUS_HERO) a->hero_button = 1;
        else if (row && row->sel < row->count - 1) row->sel++;
        return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_SPACE:
        if (a->focus == FOCUS_NAV) set_tab(a, a->nav_sel);
        else activate(a);
        return true;
    case SDLK_x:
    case SDLK_DELETE:
        if (row && row->style == ROW_WIDE && row->sel < row->count && row->items[row->sel].hist >= 0) {
            history_remove(row->items[row->sel].hist);
            browse_refresh_local(a);
        }
        return true;
    default:
        return false;
    }
}

static int row_at(App *a, int my, int *card_y, int *ch_out)
{
    int y = rows_top();
    int base = (int)a->row_anim;
    for (int i = 0; i < base && i < a->row_count; i++) y -= row_height(&a->rows[i]);
    if (base < a->row_count) y -= (int)((a->row_anim - base) * row_height(&a->rows[base]));
    for (int i = 0; i < a->row_count; i++) {
        int rh = row_height(&a->rows[i]);
        int cw, ch;
        card_size(&a->rows[i], &cw, &ch);
        if (my >= y + 40 && my < y + 40 + ch && my >= rows_top() - 10) {
            *card_y = y + 40;
            *ch_out = ch;
            return i;
        }
        y += rh;
    }
    return -1;
}

static int card_at(App *a, int ri, int mx)
{
    Row *row = &a->rows[ri];
    int cw, ch;
    card_size(row, &cw, &ch);
    for (int k = 0; k < row->count; k++) {
        int cx = 60 + (int)((k - row->scroll) * (cw + 12));
        if (mx >= cx && mx < cx + cw) return k;
    }
    return -1;
}

void browse_hover(App *a, int x, int y)
{
    int cy, ch;
    int ri = row_at(a, y, &cy, &ch);
    if (ri < 0) return;
    int k = card_at(a, ri, x);
    if (k < 0) return;
    if (a->focus != ri && ri > a->focus + 1) return;
    a->focus = ri;
    a->rows[ri].sel = k;
}

void browse_click(App *a, int x, int y)
{
    int t = nav_hit(a, x, y);
    if (t >= 0) {
        set_tab(a, t);
        return;
    }
    int H = hero_h();
    if (y < rows_top() - 10 && y < H) {
        if (a->tab == TAB_SEARCH) {
            a->focus = FOCUS_HERO;
            a->typing = true;
            return;
        }
        if (y > NAV_H && x < win_w / 2) {
            a->focus = FOCUS_HERO;
            const Meta *m = focused_meta(a);
            if (m && y > H - 170) {
                const char *play, *info;
                hero_buttons(a, m, &play, &info);
                int w1 = text_width(a->ui.mdb, play) + 76;
                a->hero_button = x < 60 + w1 + 7 ? 0 : 1;
                activate(a);
            }
        }
        return;
    }
    int cy, ch;
    int ri = row_at(a, y, &cy, &ch);
    if (ri < 0) return;
    int k = card_at(a, ri, x);
    if (k < 0) return;
    a->focus = ri;
    a->rows[ri].sel = k;
    activate(a);
}

void browse_wheel(App *a, int dx, int dy)
{
    if (dx) {
        if (a->focus >= 0 && a->focus < a->row_count) {
            Row *row = &a->rows[a->focus];
            row->sel -= dx;
            if (row->sel < 0) row->sel = 0;
            if (row->sel >= row->count) row->sel = row->count > 0 ? row->count - 1 : 0;
        }
        return;
    }
    if (dy < 0 && a->focus < a->row_count - 1) a->focus = a->focus < 0 ? 0 : a->focus + 1;
    else if (dy > 0 && a->focus > FOCUS_HERO) a->focus--;
}

void browse_tick(App *a)
{
    if (a->tab == TAB_SEARCH && a->query_dirty && SDL_GetTicks() - a->query_changed > 450) {
        a->query_dirty = false;
        browse_build(a);
        a->focus = FOCUS_HERO;
    }
}
