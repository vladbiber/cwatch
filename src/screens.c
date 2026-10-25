#include "app.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NYAA_ROW_H 58
#define DL_ROW_H 84

static App *G;

void media_dest(const char *name, char *out, size_t len)
{
    char clean[TITLE_LEN];
    size_t o = 0;
    for (const char *p = name; *p && o + 1 < sizeof(clean); p++)
        clean[o++] = (*p == '/' || *p == ':' || *p == '\\') ? ' ' : *p;
    clean[o] = '\0';
    trim_line(clean);
    char base[PATH_MAX];
    expand_path(G && G->lib->root_count ? G->lib->roots[0] : "~/media", base, sizeof(base));
    path_join(out, len, base, clean[0] ? clean : "Downloads");
}

void start_source(App *a, const Source *s, bool download, const char *dest_name,
                  const char *play_title, int want_episode, const HistoryEntry *hist)
{
    (void)want_episode;
    G = a;
    char magnet[URL_LEN * 2];
    make_magnet(s->hash, s->file[0] ? s->file : s->title, magnet, sizeof(magnet));
    if (download) {
        char dest[PATH_MAX];
        media_dest(dest_name, dest, sizeof(dest));
        const char *t = s->file[0] ? s->file : s->title;
        if (download_start(magnet, s->file_idx, dest, t)) app_toast(a, "Download started: %s", t);
        else app_toast(a, "Could not start the download");
        return;
    }
    if (!play_torrent(magnet, s->file_idx, play_title, hist))
        app_toast(a, "%s", stream_status()->error);
}

static void parse_nyaa_job(Job *job)
{
    job->result = job->ok ? parse_nyaa(job->data) : NULL;
}

static void apply_nyaa_job(Job *job)
{
    SourceList *l = job->result;
    if (!G || job->gen != G->nyaa_gen) {
        source_list_free(l);
        return;
    }
    source_list_free(G->nyaa);
    G->nyaa = l ? l : calloc(1, sizeof(SourceList));
    G->nyaa_loading = false;
    G->nyaa_sel = 0;
    G->nyaa_scroll = 0;
}

void nyaa_search(App *a, const char *query)
{
    G = a;
    if (query != a->nyaa_query) copy_text(a->nyaa_query, sizeof(a->nyaa_query), query);
    char url[URL_LEN];
    nyaa_url(url, sizeof(url), query);
    Job *job = job_new(url, 300, parse_nyaa_job, apply_nyaa_job);
    if (!job) return;
    job->gen = ++a->nyaa_gen;
    a->nyaa_loading = true;
    net_submit(job);
}

static void nyaa_act(App *a, bool download)
{
    if (!a->nyaa || a->nyaa_sel >= a->nyaa->count) return;
    Source *s = &a->nyaa->items[a->nyaa_sel];
    char magnet[URL_LEN * 2], name[TITLE_LEN];
    make_magnet(s->hash, s->title, magnet, sizeof(magnet));
    clean_title(s->title, name, sizeof(name));
    if (!name[0]) copy_text(name, sizeof(name), a->nyaa_query);
    files_open(a, magnet, s->title, name, NULL, 0, download ? 0 : 1, NULL);
}

void nyaa_render(App *a)
{
    fill_rect(a->r, 0, 0, win_w, win_h, C_BG);
    int x = 60, y = NAV_H + 24, w = win_w - 120;
    fill_round(a->r, x, y, w, 56, 8, (Color){45, 45, 45, 255});
    if (a->typing) stroke_round(a->r, x, y, w, 56, 8, 2, C_WHITE);
    draw_search_icon(a->r, x + 20, y + 17, 22, C_DIM);
    char q[TITLE_LEN + 4];
    snprintf(q, sizeof(q), "%s%s", a->nyaa_query, a->typing && (SDL_GetTicks() / 500) % 2 ? "|" : "");
    if (q[0]) draw_text(a->r, a->ui.lg, q, x + 60, y + 12, C_TEXT, ALIGN_LEFT);
    else draw_text(a->r, a->ui.md, "Search nyaa.si (e.g. frieren 1080p)", x + 60, y + 16, C_FAINT, ALIGN_LEFT);

    int ly = y + 80, lh = win_h - ly - 50;
    int count = a->nyaa ? a->nyaa->count : 0;
    if (a->nyaa_loading) draw_text(a->r, a->ui.md, "Searching nyaa.si...", x, ly, C_DIM, ALIGN_LEFT);
    else if (a->nyaa && count == 0) draw_text(a->r, a->ui.md, "No results.", x, ly, C_DIM, ALIGN_LEFT);
    else if (!a->nyaa) draw_text(a->r, a->ui.md, "Type a search and press Enter.", x, ly, C_DIM, ALIGN_LEFT);

    int visible = lh / NYAA_ROW_H;
    float want = (float)(a->nyaa_sel - visible / 2);
    if (want > count - visible) want = (float)(count - visible);
    if (want < 0) want = 0;
    a->nyaa_scroll += (want - a->nyaa_scroll) * 0.3f;
    SDL_Rect clip = {0, ly, win_w, lh};
    SDL_RenderSetClipRect(a->r, &clip);
    for (int i = 0; i < count; i++) {
        int ry = ly + (int)((i - a->nyaa_scroll) * NYAA_ROW_H);
        if (ry + NYAA_ROW_H < ly || ry > ly + lh) continue;
        Source *s = &a->nyaa->items[i];
        bool sel = !a->typing && a->nyaa_sel == i;
        if (sel) fill_round(a->r, x - 14, ry, w + 28, NYAA_ROW_H - 4, 6, (Color){255, 255, 255, 30});
        if (sel) stroke_round(a->r, x - 14, ry, w + 28, NYAA_ROW_H - 4, 6, 2, (Color){255, 255, 255, 180});
        if (s->trusted) fill_rect(a->r, x - 14, ry + 6, 4, NYAA_ROW_H - 16, C_GREEN);
        draw_text_fit(a->r, a->ui.md, s->title, x, ry + 6, w - 260, C_WHITE);
        draw_text(a->r, a->ui.xs, s->quality[0] ? s->quality : "", x, ry + 32, C_FAINT, ALIGN_LEFT);
        char seeds[32];
        snprintf(seeds, sizeof(seeds), "%d seeds", s->seeds);
        draw_text(a->r, a->ui.sm, seeds, x + w, ry + 6, s->seeds > 10 ? C_GREEN : s->seeds > 0 ? C_DIM : C_RED, ALIGN_RIGHT);
        draw_text(a->r, a->ui.xs, s->size, x + w, ry + 32, C_FAINT, ALIGN_RIGHT);
    }
    SDL_RenderSetClipRect(a->r, NULL);
    draw_list_scrollbar(a, win_w - 30, ly, lh, count, visible, a->nyaa_scroll);
    draw_text(a->r, a->ui.xs, "Enter play (choose episode)   D download   / new search   green bar = trusted uploader",
              x, win_h - 34, C_FAINT, ALIGN_LEFT);
}

bool nyaa_key(App *a, SDL_Keycode key)
{
    int count = a->nyaa ? a->nyaa->count : 0;
    if (a->typing) {
        if (key == SDLK_ESCAPE) a->typing = false;
        else if (key == SDLK_BACKSPACE) {
            size_t n = strlen(a->nyaa_query);
            if (n) a->nyaa_query[n - 1] = '\0';
        } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            if (a->nyaa_query[0]) {
                nyaa_search(a, a->nyaa_query);
                a->typing = false;
            }
        } else if (key == SDLK_UP) {
            a->typing = false;
            a->focus = FOCUS_NAV;
        } else if (key == SDLK_DOWN && count) {
            a->typing = false;
        }
        return true;
    }
    switch (key) {
    case SDLK_SLASH:
        a->typing = true;
        a->nyaa_query[0] = '\0';
        return true;
    case SDLK_UP:
        if (a->nyaa_sel > 0) a->nyaa_sel--;
        else a->typing = true;
        return true;
    case SDLK_DOWN:
        if (a->nyaa_sel < count - 1) a->nyaa_sel++;
        return true;
    case SDLK_PAGEDOWN:
        a->nyaa_sel = a->nyaa_sel + 10 < count ? a->nyaa_sel + 10 : (count ? count - 1 : 0);
        return true;
    case SDLK_PAGEUP:
        a->nyaa_sel = a->nyaa_sel > 10 ? a->nyaa_sel - 10 : 0;
        return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        nyaa_act(a, false);
        return true;
    case SDLK_d:
        nyaa_act(a, true);
        return true;
    default:
        return false;
    }
}

void nyaa_click(App *a, int x, int y)
{
    int ly = NAV_H + 24 + 80;
    if (y >= NAV_H + 24 && y < NAV_H + 80) {
        a->typing = true;
        return;
    }
    int i = (int)((y - ly) / NYAA_ROW_H + a->nyaa_scroll);
    (void)x;
    if (y >= ly && a->nyaa && i >= 0 && i < a->nyaa->count) {
        a->typing = false;
        if (a->nyaa_sel == i) nyaa_act(a, false);
        else a->nyaa_sel = i;
    }
}

void nyaa_wheel(App *a, int dy)
{
    if (a->typing) return;
    nyaa_key(a, dy < 0 ? SDLK_DOWN : SDLK_UP);
}

void downloads_render(App *a)
{
    fill_rect(a->r, 0, 0, win_w, win_h, C_BG);
    int x = 60, y = NAV_H + 30, w = win_w - 120;
    draw_text(a->r, a->ui.xl, "Downloads", x, y, C_WHITE, ALIGN_LEFT);
    char sub[PATH_MAX + 64];
    snprintf(sub, sizeof(sub), "Saved to %s  ·  finished downloads show up in My Library", a->lib->roots[0]);
    draw_text_fit(a->r, a->ui.sm, sub, x, y + 48, w, C_DIM);
    int n = download_count();
    if (n == 0) {
        draw_text(a->r, a->ui.md, "Nothing downloading. Press D on an episode or source to download it.",
                  x, y + 100, C_FAINT, ALIGN_LEFT);
        return;
    }
    if (a->dl_sel >= n) a->dl_sel = n - 1;
    int ly = y + 90;
    for (int i = n - 1, row = 0; i >= 0; i--, row++) {
        const Download *d = download_get(i);
        int ry = ly + row * DL_ROW_H;
        if (ry > win_h - 60) break;
        bool sel = a->dl_sel == i;
        fill_round(a->r, x - 14, ry, w + 28, DL_ROW_H - 10, 8, sel ? (Color){255, 255, 255, 30} : (Color){32, 32, 32, 255});
        if (sel) stroke_round(a->r, x - 14, ry, w + 28, DL_ROW_H - 10, 8, 2, (Color){255, 255, 255, 180});
        draw_text_fit(a->r, a->ui.mdb, d->title, x, ry + 10, w - 260, C_WHITE);
        char state[128], rate[32];
        format_bytes(d->rate, rate, sizeof(rate));
        switch (d->state) {
        case DL_ACTIVE:
            snprintf(state, sizeof(state), "%.1f%%  ·  %s/s  ·  %d peers", d->progress * 100, rate, d->peers);
            break;
        case DL_DONE: copy_text(state, sizeof(state), "Done"); break;
        case DL_FAILED: snprintf(state, sizeof(state), "Failed %s", d->error); break;
        default: snprintf(state, sizeof(state), "Paused at %.1f%%", d->progress * 100); break;
        }
        Color sc = d->state == DL_DONE ? C_GREEN : d->state == DL_FAILED ? C_RED : C_DIM;
        draw_text(a->r, a->ui.sm, state, x + w, ry + 12, sc, ALIGN_RIGHT);
        if (d->state != DL_DONE) draw_progress(a->r, x, ry + 46, w, 6, d->progress);
        else draw_text_fit(a->r, a->ui.xs, d->dest, x, ry + 42, w, C_FAINT);
    }
    draw_text(a->r, a->ui.xs, "Space pause/resume   Delete remove from list   R rescan library",
              x, win_h - 34, C_FAINT, ALIGN_LEFT);
}

bool downloads_key(App *a, SDL_Keycode key)
{
    int n = download_count();
    switch (key) {
    case SDLK_UP:
        if (a->dl_sel < n - 1) a->dl_sel++;
        else a->focus = FOCUS_NAV;
        return true;
    case SDLK_DOWN:
        if (a->focus == FOCUS_NAV) a->focus = FOCUS_HERO;
        else if (a->dl_sel > 0) a->dl_sel--;
        return true;
    case SDLK_SPACE:
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        download_cancel(a->dl_sel);
        return true;
    case SDLK_DELETE:
    case SDLK_x:
        download_remove(a->dl_sel);
        return true;
    default:
        return false;
    }
}

void downloads_click(App *a, int x, int y)
{
    (void)x;
    int ly = NAV_H + 30 + 90;
    int row = (y - ly) / DL_ROW_H;
    int n = download_count();
    if (y >= ly && row >= 0 && row < n) a->dl_sel = n - 1 - row;
}

void status_render(App *a)
{
    const StreamStatus *st = stream_status();
    Uint32 now = SDL_GetTicks();
    if (st->state == ST_CONNECTING) {
        fill_rect(a->r, 0, 0, win_w, win_h, (Color){0, 0, 0, 200});
        int w = 620, h = 200, x = (win_w - w) / 2, y = (win_h - h) / 2;
        fill_round(a->r, x, y, w, h, 12, (Color){28, 28, 28, 255});
        draw_text_fit(a->r, a->ui.lg, st->title, x + 30, y + 26, w - 60, C_WHITE);
        char line[160], rate[32];
        format_bytes(st->rate, rate, sizeof(rate));
        if (st->peers == 0 && st->progress == 0)
            snprintf(line, sizeof(line), "Finding peers...  %us", (now - st->started) / 1000);
        else
            snprintf(line, sizeof(line), "%d %s  ·  %s/s  ·  buffering", st->peers, st->peers == 1 ? "peer" : "peers", rate);
        draw_text(a->r, a->ui.md, line, x + 30, y + 74, C_DIM, ALIGN_LEFT);
        int bw = w - 60;
        fill_round(a->r, x + 30, y + 120, bw, 6, 3, (Color){70, 70, 70, 255});
        int seg = bw / 4;
        int pos = (int)((now / 4) % (Uint32)(bw + seg)) - seg;
        int sx = x + 30 + (pos < 0 ? 0 : pos);
        int sw = seg - (pos < 0 ? -pos : 0);
        if (sx + sw > x + 30 + bw) sw = x + 30 + bw - sx;
        if (sw > 0) fill_round(a->r, sx, y + 120, sw, 6, 3, C_RED);
        draw_text(a->r, a->ui.xs, "Esc cancel", x + 30, y + 152, C_FAINT, ALIGN_LEFT);
    } else if (st->state == ST_PLAYING) {
        char line[200], rate[32];
        format_bytes(st->rate, rate, sizeof(rate));
        snprintf(line, sizeof(line), "Streaming in mpv: %s   ·   %d peers  ·  %s/s  ·  %.0f%% cached",
                 st->title, st->peers, rate, st->progress * 100);
        int w = text_width(a->ui.sm, line) + 40;
        if (w > win_w - 40) w = win_w - 40;
        fill_round(a->r, (win_w - w) / 2, win_h - 64, w, 40, 20, (Color){40, 40, 40, 235});
        draw_text_fit(a->r, a->ui.sm, line, (win_w - w) / 2 + 20, win_h - 54, w - 40, C_TEXT);
    }
    if (a->toast[0] && now < a->toast_until) {
        int w = text_width(a->ui.md, a->toast) + 48;
        if (w > win_w - 40) w = win_w - 40;
        int y = st->state == ST_PLAYING ? win_h - 120 : win_h - 80;
        fill_round(a->r, (win_w - w) / 2, y, w, 46, 23, (Color){235, 235, 235, 245});
        draw_text_fit(a->r, a->ui.md, a->toast, (win_w - w) / 2 + 24, y + 11, w - 48, C_BLACK);
    }
}
