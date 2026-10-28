#include "app.h"
#include "images.h"
#include "net.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EP_ROW_H 132
#define SEASON_LOCAL -1

static App *G;

static void build_seasons(App *a)
{
    Detail *d = &a->det;
    d->season_count = 0;
    MetaFull *mf = d->mf;
    bool has_zero = false;
    for (int i = 0; i < mf->ep_count && d->season_count < 62; i++) {
        int s = mf->eps[i].season;
        if (s == 0) {
            has_zero = true;
            continue;
        }
        bool seen = false;
        for (int k = 0; k < d->season_count; k++)
            if (d->seasons[k] == s) seen = true;
        if (!seen) d->seasons[d->season_count++] = s;
    }
    if (has_zero) d->seasons[d->season_count++] = 0;

    int unmatched = 0;
    if (d->col >= 0) {
        Collection *c = &a->lib->cols[d->col];
        for (int j = 0; j < c->count; j++) {
            LocalItem *li = &a->lib->items[c->items[j]];
            bool found = false;
            for (int i = 0; i < mf->ep_count && !found; i++)
                found = mf->eps[i].season == li->season && mf->eps[i].episode == li->episode;
            if (!found) unmatched++;
        }
        if (unmatched > 0 || mf->ep_count == 0) d->seasons[d->season_count++] = SEASON_LOCAL;
    }
    for (int i = 0; i < mf->ep_count; i++)
        mf->eps[i].local_item = library_find_episode(a->lib, d->col, mf->eps[i].season, mf->eps[i].episode);

    d->season_sel = 0;
    if (d->col >= 0 && mf->ep_count == 0) return;
    int next = d->col >= 0 ? library_next_item(a->lib, d->col) : -1;
    int want = next >= 0 ? a->lib->items[next].season : 1;
    for (int k = 0; k < d->season_count; k++)
        if (d->seasons[k] == want) d->season_sel = k;
}

static int season_eps(App *a, Episode *out, int max)
{
    Detail *d = &a->det;
    if (d->season_count == 0) return 0;
    int s = d->seasons[d->season_sel];
    int n = 0;
    if (s == SEASON_LOCAL) {
        Collection *c = &a->lib->cols[d->col];
        for (int j = 0; j < c->count && n < max; j++) {
            LocalItem *li = &a->lib->items[c->items[j]];
            Episode *e = &out[n++];
            memset(e, 0, sizeof(*e));
            e->season = li->season;
            e->episode = li->episode;
            copy_text(e->title, sizeof(e->title), li->title);
            e->local_item = c->items[j];
        }
        return n;
    }
    for (int i = 0; i < d->mf->ep_count && n < max; i++)
        if (d->mf->eps[i].season == s) out[n++] = d->mf->eps[i];
    return n;
}

static void parse_full(Job *job)
{
    job->result = job->ok ? parse_meta_full(job->data) : NULL;
}

static void apply_full(Job *job)
{
    MetaFull *mf = job->result;
    if (!G || !G->det.open || job->gen != G->det.gen || !mf) {
        meta_full_free(mf);
        if (G && G->det.open && job->gen == G->det.gen) G->det.loading = false;
        return;
    }
    Detail *d = &G->det;
    Meta keep = d->mf->meta;
    meta_full_free(d->mf);
    d->mf = mf;
    if (!mf->meta.background[0]) copy_text(mf->meta.background, sizeof(mf->meta.background), keep.background);
    if (!mf->meta.poster[0]) copy_text(mf->meta.poster, sizeof(mf->meta.poster), keep.poster);
    mf->meta.local_col = keep.local_col;
    mf->meta.local_item = keep.local_item;
    mf->meta.hist = -1;
    d->loading = false;
    build_seasons(G);
}

void detail_open_meta(App *a, const Meta *m)
{
    G = a;
    Detail *d = &a->det;
    meta_full_free(d->mf);
    memset(d, 0, sizeof(*d));
    d->gen = (unsigned)SDL_GetTicks() ^ 0x5bd1e995u;
    d->open = true;
    d->mf = calloc(1, sizeof(MetaFull));
    if (!d->mf) {
        d->open = false;
        return;
    }
    d->mf->meta = *m;
    d->col = m->local_col >= 0 ? m->local_col : library_find_by_name(a->lib, m->name);
    d->mf->meta.local_col = d->col;
    if (m->id[0]) {
        char url[URL_LEN];
        meta_url(url, sizeof(url), m->type, m->id);
        Job *job = job_new(url, 6 * 3600, parse_full, apply_full);
        if (job) {
            job->gen = d->gen;
            d->loading = true;
            net_submit(job);
        }
    }
    build_seasons(a);
}

void detail_close(App *a)
{
    a->det.open = false;
    meta_full_free(a->det.mf);
    a->det.mf = NULL;
}

static void episode_heading(App *a, const Episode *e, char *heading, size_t hl, char *play, size_t pl)
{
    const char *name = a->det.mf->meta.name;
    snprintf(heading, hl, "%s  ·  S%d:E%d  %s", name, e->season, e->episode, e->title);
    snprintf(play, pl, "%s  S%d:E%d", name, e->season, e->episode);
}

static void open_episode(App *a, const Episode *e, bool download)
{
    if (!download && e->local_item >= 0) {
        play_local(a, e->local_item, a->det.mf->meta.name);
        return;
    }
    if (!e->id[0]) {
        app_toast(a, "This episode only exists on disk");
        return;
    }
    const Meta *m = &a->det.mf->meta;
    char heading[TITLE_LEN * 2], play[TITLE_LEN], q[TITLE_LEN];
    episode_heading(a, e, heading, sizeof(heading), play, sizeof(play));
    snprintf(q, sizeof(q), "%s %02d", m->name, e->episode);
    HistoryEntry h = {0};
    copy_text(h.meta_id, sizeof(h.meta_id), m->id);
    copy_text(h.type, sizeof(h.type), "series");
    copy_text(h.name, sizeof(h.name), m->name);
    copy_text(h.poster, sizeof(h.poster), m->poster);
    copy_text(h.background, sizeof(h.background), m->background);
    copy_text(h.ep_id, sizeof(h.ep_id), e->id);
    copy_text(h.ep_title, sizeof(h.ep_title), e->title);
    h.season = e->season;
    h.episode = e->episode;
    sources_open(a, "series", e->id, heading, m->name, play, q, e->season, e->episode, &h);
    a->src.tab = 0;
    a->src.download_mode = download;
}

static void detail_play(App *a, bool download)
{
    Detail *d = &a->det;
    const Meta *m = &d->mf->meta;
    if (meta_is_series(m) && (d->mf->ep_count > 0 || d->col >= 0)) {
        Episode eps[512];
        int n = season_eps(a, eps, 512);
        if (n == 0) return;
        int pick = 0;
        int next = d->col >= 0 ? library_next_item(a->lib, d->col) : -1;
        for (int i = 0; i < n; i++)
            if (next >= 0 && eps[i].local_item == next) pick = i;
        if (d->focus == DF_EPISODES && d->ep_sel < n) pick = d->ep_sel;
        open_episode(a, &eps[pick], download);
        return;
    }
    if (!download && d->col >= 0) {
        int it = library_next_item(a->lib, d->col);
        if (it >= 0) {
            play_local(a, it, m->name);
            return;
        }
    }
    if (!m->id[0]) return;
    Meta copy = *m;
    copy.local_item = -1;
    copy.hist = -1;
    play_meta(a, &copy);
    a->src.download_mode = download;
}

static int page_offset(App *a, int eps_top)
{
    Detail *d = &a->det;
    if (d->focus != DF_EPISODES && d->focus != DF_SEASONS) return 0;
    int target = eps_top - NAV_H - 20;
    if (d->focus == DF_EPISODES) target += (d->ep_sel > 1 ? d->ep_sel - 1 : 0) * EP_ROW_H;
    return target < 0 ? 0 : target;
}

static int detail_layout(App *a, int *buttons_y, int *seasons_y)
{
    const Meta *m = &a->det.mf->meta;
    int max_w = (int)(win_w * 0.45);
    int y = NAV_H + 30;
    y += (int)(win_h * 0.18) + 18;
    y += 30;
    if (m->desc[0]) {
        int lh = TTF_FontLineSkip(a->ui.md);
        int chars = max_w / 9;
        int lines = (int)strlen(m->desc) / (chars > 0 ? chars : 1) + 1;
        y += (lines > 4 ? 4 : lines) * lh + 18;
    }
    *buttons_y = y;
    *seasons_y = y + 90;
    return max_w;
}

void detail_render(App *a)
{
    Detail *d = &a->det;
    const Meta *m = &d->mf->meta;
    fill_rect(a->r, 0, 0, win_w, win_h, C_BLACK);
    SDL_Texture *bg = m->background[0] ? img_get(m->background) : (m->poster[0] ? img_get(m->poster) : NULL);
    if (!bg && d->col >= 0) {
        int it = library_next_item(a->lib, d->col);
        if (it >= 0) bg = img_thumb(a->lib->items[it].path);
    }
    int bx = win_w / 3;
    if (bg) draw_cover(a->r, bg, bx, 0, win_w - bx, (win_w - bx) * 9 / 16, 255);
    fade_left(a->r, bx, 0, (win_w - bx) * 3 / 4, win_h, C_BLACK);
    fade_down(a->r, bx, (win_w - bx) * 9 / 16 - 240, win_w - bx, 240, C_BLACK);
    fill_rect(a->r, bx, (win_w - bx) * 9 / 16, win_w - bx, win_h, C_BLACK);
    fade_up(a->r, 0, 0, win_w, 120, (Color){0, 0, 0, 200});

    int buttons_y, seasons_y;
    int max_w = detail_layout(a, &buttons_y, &seasons_y);
    static float scroll;
    int target = page_offset(a, seasons_y);
    scroll += (target - scroll) * 0.22f;
    if (fabsf(target - scroll) < 1) scroll = (float)target;
    int off = (int)scroll;

    int x = 60, y = NAV_H + 30 - off;
    SDL_Texture *logo = m->logo[0] ? img_get(m->logo) : NULL;
    int th = (int)(win_h * 0.18);
    if (logo) draw_contain(a->r, logo, x, y, max_w, th, ALIGN_LEFT, 255);
    else draw_text_wrap(a->r, text_width(a->ui.hero, m->name) > max_w ? a->ui.xl : a->ui.hero,
                        m->name, x, y + th - 80, max_w, 2, C_WHITE);
    y += th + 18;
    char line[400] = "";
    size_t used = 0;
    const char *parts[] = {m->year, m->runtime, m->genres};
    if (m->rating[0] && strcmp(m->rating, "0") != 0)
        used += (size_t)snprintf(line, sizeof(line), "IMDb %s", m->rating);
    for (int i = 0; i < 3; i++)
        if (parts[i][0] && used < sizeof(line))
            used += (size_t)snprintf(line + used, sizeof(line) - used, "%s%s", used ? "     " : "", parts[i]);
    if (d->col >= 0 && used < sizeof(line))
        snprintf(line + used, sizeof(line) - used, "%s%d on disk", used ? "     " : "", a->lib->cols[d->col].count);
    draw_text_fit(a->r, a->ui.sm, line, x, y, max_w + 200, C_DIM);
    y += 30;
    if (m->desc[0]) draw_text_wrap(a->r, a->ui.md, m->desc, x, y, max_w, 4, C_TEXT);
    else if (d->loading) draw_text(a->r, a->ui.md, "Loading details...", x, y, C_FAINT, ALIGN_LEFT);

    y = buttons_y - off;
    char play_label[64] = "Play";
    bool series = meta_is_series(m) && (d->mf->ep_count > 0 || d->col >= 0);
    if (d->col >= 0) {
        int it = library_next_item(a->lib, d->col);
        if (it >= 0 && a->lib->items[it].has_progress) copy_text(play_label, sizeof(play_label), "Resume");
        if (it >= 0 && series)
            snprintf(play_label + strlen(play_label), sizeof(play_label) - strlen(play_label), " S%d:E%d",
                     a->lib->items[it].season, a->lib->items[it].episode);
    }
    const char *labels[2] = {play_label, "Download"};
    int bxp = x;
    for (int b = 0; b < 2; b++) {
        bool foc = d->focus == DF_BUTTONS && d->button == b;
        int tw = text_width(a->ui.mdb, labels[b]) + (b == 0 ? 76 : 52);
        if (foc) fill_round(a->r, bxp - 4, y - 4, tw + 8, 58, 10, (Color){255, 255, 255, 90});
        fill_round(a->r, bxp, y, tw, 50, 6, b == 0 ? C_WHITE : (Color){109, 109, 110, 200});
        Color fg = b == 0 ? C_BLACK : C_WHITE;
        if (b == 0) draw_play_icon(a->r, bxp + 24, y + 14, 22, fg);
        draw_text(a->r, a->ui.mdb, labels[b], bxp + (b == 0 ? 56 : 26), y + 13, fg, ALIGN_LEFT);
        bxp += tw + 14;
    }

    if (!series) {
        draw_text(a->r, a->ui.xs, "Enter play   D download   Esc back", 60, win_h - 34, C_FAINT, ALIGN_LEFT);
        return;
    }

    y = seasons_y - off;
    int sx = 60;
    for (int k = 0; k < d->season_count; k++) {
        char label[32];
        int s = d->seasons[k];
        if (s == SEASON_LOCAL) copy_text(label, sizeof(label), "On Disk");
        else if (s == 0) copy_text(label, sizeof(label), "Specials");
        else snprintf(label, sizeof(label), "Season %d", s);
        int w = text_width(a->ui.md, label) + 32;
        bool sel = d->season_sel == k;
        fill_round(a->r, sx, y, w, 40, 20, sel ? (Color){235, 235, 235, 255} : (Color){50, 50, 50, 220});
        if (sel && d->focus == DF_SEASONS) stroke_round(a->r, sx - 3, y - 3, w + 6, 46, 23, 2, C_WHITE);
        draw_text(a->r, a->ui.md, label, sx + 16, y + 8, sel ? C_BLACK : C_TEXT, ALIGN_LEFT);
        sx += w + 10;
    }
    if (d->loading && d->season_count == 0)
        draw_text(a->r, a->ui.md, "Loading episodes...", 60, y + 8, C_FAINT, ALIGN_LEFT);

    Episode eps[512];
    int n = season_eps(a, eps, 512);
    y += 64;
    int list_w = win_w - 120;
    for (int i = 0; i < n; i++) {
        int ey = y + i * EP_ROW_H;
        if (ey + EP_ROW_H < 0) continue;
        if (ey > win_h) break;
        Episode *e = &eps[i];
        bool sel = d->focus == DF_EPISODES && d->ep_sel == i;
        if (sel) fill_round(a->r, 44, ey - 8, list_w + 32, EP_ROW_H - 4, 8, (Color){255, 255, 255, 28});
        if (sel) stroke_round(a->r, 44, ey - 8, list_w + 32, EP_ROW_H - 4, 8, 2, (Color){255, 255, 255, 200});
        char num[16];
        snprintf(num, sizeof(num), "%d", e->episode);
        draw_text(a->r, a->ui.lg, num, 84, ey + 38, C_DIM, ALIGN_CENTER);
        int tw = 192, thh = 108, tx = 120;
        SDL_Texture *t = e->thumb[0] ? img_get(e->thumb) : NULL;
        if (!t && e->local_item >= 0) t = img_thumb(a->lib->items[e->local_item].path);
        if (t) draw_cover(a->r, t, tx, ey, tw, thh, 255);
        else draw_placeholder(a->r, e->title, tx, ey, tw, thh);
        LocalItem *li = e->local_item >= 0 ? &a->lib->items[e->local_item] : NULL;
        bool watched = (li && li->watched) || library_ep_watched(e->id);
        if (watched) {
            fill_rect(a->r, tx, ey, tw, thh, (Color){0, 0, 0, 120});
            draw_progress(a->r, tx, ey + thh - 5, tw, 5, 1.0);
        } else if (li && li->has_progress && li->duration > 0) {
            draw_progress(a->r, tx, ey + thh - 5, tw, 5, li->progress_sec / li->duration);
        }
        int txx = tx + tw + 22, tww = list_w - tw - 120;
        draw_text_fit(a->r, a->ui.mdb, e->title[0] ? e->title : "Episode", txx, ey + 4, tww - 160,
                      watched ? C_DIM : C_WHITE);
        int bx2 = 60 + list_w;
        if (li) {
            const char *badge = li->has_progress && !watched ? "In progress" : "On disk";
            int bw = text_width(a->ui.xs, badge) + 18;
            bx2 -= bw;
            fill_round(a->r, bx2, ey + 6, bw, 22, 4, C_GREEN);
            draw_text(a->r, a->ui.xs, badge, bx2 + 9, ey + 8, C_BLACK, ALIGN_LEFT);
            bx2 -= 8;
        }
        if (watched) {
            int bw = text_width(a->ui.xs, "Watched") + 36;
            bx2 -= bw;
            fill_round(a->r, bx2, ey + 6, bw, 22, 4, (Color){230, 230, 230, 255});
            SDL_SetRenderDrawColor(a->r, 0, 0, 0, 255);
            for (int t = 0; t < 2; t++) {
                SDL_RenderDrawLine(a->r, bx2 + 8, ey + 16 + t, bx2 + 12, ey + 20 + t);
                SDL_RenderDrawLine(a->r, bx2 + 12, ey + 20 + t, bx2 + 20, ey + 11 + t);
            }
            draw_text(a->r, a->ui.xs, "Watched", bx2 + 26, ey + 8, C_BLACK, ALIGN_LEFT);
        }
        if (!li && !watched && e->released[0]) {
            char date[16];
            copy_text(date, 11, e->released);
            draw_text(a->r, a->ui.xs, date, 60 + list_w, ey + 8, C_FAINT, ALIGN_RIGHT);
        }
        if (e->overview[0]) draw_text_wrap(a->r, a->ui.sm, e->overview, txx, ey + 36, tww, 3, C_DIM);
        else if (li) draw_text_fit(a->r, a->ui.sm, basename_of(li->path), txx, ey + 36, tww, C_FAINT);
    }
    fade_down(a->r, 0, win_h - 84, win_w, 40, C_BLACK);
    fill_rect(a->r, 0, win_h - 44, win_w, 44, C_BLACK);
    draw_text(a->r, a->ui.xs, "Enter play   D download   M mark watched   Left/Right season   Esc back",
              60, win_h - 32, C_FAINT, ALIGN_LEFT);
}

bool detail_key(App *a, SDL_Keycode key)
{
    Detail *d = &a->det;
    bool series = meta_is_series(&d->mf->meta) && (d->mf->ep_count > 0 || d->col >= 0);
    Episode eps[512];
    int n = series ? season_eps(a, eps, 512) : 0;
    switch (key) {
    case SDLK_ESCAPE:
    case SDLK_BACKSPACE:
        detail_close(a);
        return true;
    case SDLK_UP:
        if (d->focus == DF_EPISODES && d->ep_sel > 0) d->ep_sel--;
        else if (d->focus == DF_EPISODES) d->focus = DF_SEASONS;
        else if (d->focus == DF_SEASONS) d->focus = DF_BUTTONS;
        return true;
    case SDLK_DOWN:
        if (!series) return true;
        if (d->focus == DF_BUTTONS) d->focus = d->season_count > 1 ? DF_SEASONS : DF_EPISODES;
        else if (d->focus == DF_SEASONS) d->focus = DF_EPISODES;
        else if (d->ep_sel < n - 1) d->ep_sel++;
        if (d->focus == DF_EPISODES && n == 0) d->focus = DF_SEASONS;
        return true;
    case SDLK_LEFT:
        if (d->focus == DF_BUTTONS) d->button = 0;
        else if (d->season_sel > 0) {
            d->season_sel--;
            d->ep_sel = 0;
        }
        return true;
    case SDLK_RIGHT:
        if (d->focus == DF_BUTTONS) d->button = 1;
        else if (d->season_sel < d->season_count - 1) {
            d->season_sel++;
            d->ep_sel = 0;
        }
        return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_SPACE:
        if (d->focus == DF_EPISODES && d->ep_sel < n) open_episode(a, &eps[d->ep_sel], false);
        else if (d->focus == DF_SEASONS) d->focus = DF_EPISODES;
        else detail_play(a, d->button == 1);
        return true;
    case SDLK_d:
        if (d->focus == DF_EPISODES && d->ep_sel < n) open_episode(a, &eps[d->ep_sel], true);
        else detail_play(a, true);
        return true;
    case SDLK_m:
        if (d->focus == DF_EPISODES && d->ep_sel < n && eps[d->ep_sel].local_item >= 0)
            library_toggle_watched(a->lib, eps[d->ep_sel].local_item);
        return true;
    default:
        return false;
    }
}

void detail_click(App *a, int x, int y)
{
    Detail *d = &a->det;
    int buttons_y, seasons_y;
    detail_layout(a, &buttons_y, &seasons_y);
    int off = page_offset(a, seasons_y);
    if (y >= buttons_y - off && y < buttons_y - off + 50 && x >= 60 && x < 60 + 460) {
        d->focus = DF_BUTTONS;
        d->button = x < 60 + 230 ? 0 : 1;
        detail_key(a, SDLK_RETURN);
        return;
    }
    if (y >= seasons_y - off && y < seasons_y - off + 40) {
        int sx = 60;
        for (int k = 0; k < d->season_count; k++) {
            char label[32];
            int s = d->seasons[k];
            if (s == SEASON_LOCAL) copy_text(label, sizeof(label), "On Disk");
            else if (s == 0) copy_text(label, sizeof(label), "Specials");
            else snprintf(label, sizeof(label), "Season %d", s);
            int w = text_width(a->ui.md, label) + 32;
            if (x >= sx && x < sx + w) {
                d->season_sel = k;
                d->ep_sel = 0;
                d->focus = DF_SEASONS;
            }
            sx += w + 10;
        }
        return;
    }
    int ey0 = seasons_y - off + 64;
    if (y >= ey0 - 8) {
        int i = (y - ey0 + 8) / EP_ROW_H;
        Episode eps[512];
        int n = season_eps(a, eps, 512);
        if (i >= 0 && i < n) {
            d->focus = DF_EPISODES;
            d->ep_sel = i;
            open_episode(a, &eps[i], false);
        }
    }
}

void detail_wheel(App *a, int dy)
{
    Detail *d = &a->det;
    if (dy < 0) {
        if (d->focus != DF_EPISODES) {
            d->focus = DF_EPISODES;
            d->ep_sel = 0;
        } else {
            detail_key(a, SDLK_DOWN);
        }
    } else if (dy > 0) {
        if (d->focus == DF_EPISODES && d->ep_sel == 0) d->focus = DF_BUTTONS;
        else detail_key(a, SDLK_UP);
    }
}
