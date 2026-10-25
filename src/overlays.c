#include "app.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SRC_ROW_H 70
#define FILE_ROW_H 46

static App *G;

static void panel(App *a, int *x, int *y, int *w, int *h)
{
    fill_rect(a->r, 0, 0, win_w, win_h, (Color){0, 0, 0, 190});
    *w = win_w - 120 < 1100 ? win_w - 120 : 1100;
    *h = win_h - 100;
    *x = (win_w - *w) / 2;
    *y = 50;
    fill_round(a->r, *x, *y, *w, *h, 10, (Color){24, 24, 24, 255});
}

static void parse_sources(Job *job)
{
    if (!job->ok) return;
    job->result = job->tag == 0 ? parse_torrentio(job->data) : parse_nyaa(job->data);
}

static void apply_sources(Job *job)
{
    SourceList *l = job->result;
    if (!G || !G->src.open || job->gen != G->src.gen) {
        source_list_free(l);
        return;
    }
    Sources *s = &G->src;
    source_list_free(s->lists[job->tag]);
    s->lists[job->tag] = l;
    s->loading[job->tag] = false;
}

static void fetch_nyaa(App *a)
{
    Sources *s = &a->src;
    char url[URL_LEN];
    nyaa_url(url, sizeof(url), s->nyaa_query);
    Job *job = job_new(url, 600, parse_sources, apply_sources);
    if (!job) return;
    job->tag = 1;
    job->gen = s->gen;
    s->loading[1] = true;
    net_submit(job);
}

void sources_open(App *a, const char *type, const char *stream_id, const char *heading,
                  const char *dest_name, const char *play_title, const char *nyaa_query,
                  int season, int episode, const HistoryEntry *hist)
{
    G = a;
    Sources *s = &a->src;
    source_list_free(s->lists[0]);
    source_list_free(s->lists[1]);
    unsigned gen = s->gen + 1;
    memset(s, 0, sizeof(*s));
    s->gen = gen;
    s->open = true;
    copy_text(s->type, sizeof(s->type), type);
    copy_text(s->stream_id, sizeof(s->stream_id), stream_id);
    copy_text(s->heading, sizeof(s->heading), heading);
    copy_text(s->dest_name, sizeof(s->dest_name), dest_name);
    copy_text(s->play_title, sizeof(s->play_title), play_title);
    copy_text(s->nyaa_query, sizeof(s->nyaa_query), nyaa_query);
    s->season = season;
    s->episode = episode;
    if (hist) s->hist = *hist;

    char url[URL_LEN];
    streams_url(url, sizeof(url), type, stream_id);
    Job *job = job_new(url, 1800, parse_sources, apply_sources);
    if (job) {
        job->tag = 0;
        job->gen = s->gen;
        s->loading[0] = true;
        net_submit(job);
    }
    fetch_nyaa(a);
}

static SourceList *cur_list(Sources *s)
{
    return s->lists[s->tab];
}

static void act_source(App *a, bool download)
{
    Sources *s = &a->src;
    SourceList *l = cur_list(s);
    if (!l || s->sel >= l->count) return;
    Source *src = &l->items[s->sel];
    HistoryEntry *h = s->hist.meta_id[0] ? &s->hist : NULL;
    if (s->tab == 1 || src->file_idx < 0) {
        char magnet[URL_LEN * 2];
        make_magnet(src->hash, src->title, magnet, sizeof(magnet));
        files_open(a, magnet, src->title, s->dest_name, s->play_title, s->episode, download ? 2 : 1, h);
        return;
    }
    start_source(a, src, download, s->dest_name, s->play_title, s->episode, h);
    s->open = false;
}

static void quality_pill(App *a, const char *q, int x, int y)
{
    if (!q[0]) return;
    bool hi = strstr(q, "4k") || strstr(q, "4K") || strstr(q, "2160");
    int w = text_width(a->ui.xs, q) + 16;
    if (w < 64) w = 64;
    fill_round(a->r, x, y, w, 24, 4, hi ? C_RED : (Color){70, 70, 70, 255});
    draw_text(a->r, a->ui.xs, q, x + w / 2, y + 3, C_WHITE, ALIGN_CENTER);
}

void draw_list_scrollbar(App *a, int x, int y, int h, int total, int visible, float scroll)
{
    if (total <= visible || total <= 0) return;
    int bh = h * visible / total;
    if (bh < 30) bh = 30;
    int by = y + (int)((h - bh) * scroll / (total - visible));
    fill_round(a->r, x, by, 4, bh, 2, (Color){120, 120, 120, 200});
}

void sources_render(App *a)
{
    Sources *s = &a->src;
    int px, py, pw, ph;
    panel(a, &px, &py, &pw, &ph);
    draw_text_fit(a->r, a->ui.lg, s->heading, px + 30, py + 24, pw - 60, C_WHITE);
    const char *mode = s->download_mode ? "Choose a source to download" : "Choose a source to play";
    draw_text(a->r, a->ui.sm, mode, px + 30, py + 60, C_DIM, ALIGN_LEFT);

    int tx = px + 30, ty = py + 96;
    const char *names[2] = {"Torrentio", "Nyaa"};
    for (int t = 0; t < 2; t++) {
        char label[64];
        if (s->loading[t]) snprintf(label, sizeof(label), "%s  ...", names[t]);
        else snprintf(label, sizeof(label), "%s  %d", names[t], s->lists[t] ? s->lists[t]->count : 0);
        int w = text_width(a->ui.md, label) + 32;
        bool sel = s->tab == t;
        fill_round(a->r, tx, ty, w, 38, 19, sel ? (Color){235, 235, 235, 255} : (Color){50, 50, 50, 255});
        draw_text(a->r, a->ui.md, label, tx + 16, ty + 7, sel ? C_BLACK : C_TEXT, ALIGN_LEFT);
        tx += w + 10;
    }
    if (s->tab == 1) {
        char q[TITLE_LEN + 32];
        snprintf(q, sizeof(q), "search: %s%s", s->nyaa_query, s->typing && (SDL_GetTicks() / 500) % 2 ? "|" : "");
        int qx = tx + 16, qw = px + pw - 30 - qx;
        fill_round(a->r, qx, ty, qw, 38, 6, s->typing ? (Color){60, 60, 60, 255} : (Color){36, 36, 36, 255});
        draw_text_fit(a->r, a->ui.sm, q, qx + 12, ty + 9, qw - 24, s->typing ? C_WHITE : C_DIM);
    }

    int ly = ty + 56, lh = py + ph - 56 - ly;
    SourceList *l = cur_list(s);
    int count = l ? l->count : 0;
    int visible = lh / SRC_ROW_H;
    float want = (float)(s->sel - visible / 2);
    if (want > count - visible) want = (float)(count - visible);
    if (want < 0) want = 0;
    s->scroll += (want - s->scroll) * 0.3f;

    SDL_Rect clip = {px, ly, pw, lh};
    SDL_RenderSetClipRect(a->r, &clip);
    if (s->loading[s->tab] && count == 0)
        draw_text(a->r, a->ui.md, "Searching...", px + 30, ly + 10, C_DIM, ALIGN_LEFT);
    else if (count == 0)
        draw_text(a->r, a->ui.md, s->tab == 0 ? "No Torrentio results. Try the Nyaa tab."
                                             : "No Nyaa results. Press / to change the search.",
                  px + 30, ly + 10, C_DIM, ALIGN_LEFT);
    for (int i = 0; i < count; i++) {
        int ry = ly + (int)((i - s->scroll) * SRC_ROW_H);
        if (ry + SRC_ROW_H < ly || ry > ly + lh) continue;
        Source *src = &l->items[i];
        bool sel = s->sel == i;
        if (sel) fill_round(a->r, px + 16, ry, pw - 32, SRC_ROW_H - 6, 6, (Color){255, 255, 255, 30});
        if (sel) stroke_round(a->r, px + 16, ry, pw - 32, SRC_ROW_H - 6, 6, 2, (Color){255, 255, 255, 180});
        quality_pill(a, src->quality[0] ? src->quality : "?", px + 30, ry + 12);
        int textx = px + 110, textw = pw - 110 - 230;
        draw_text_fit(a->r, a->ui.md, src->title, textx, ry + 8, textw, C_WHITE);
        const char *second = src->file[0] && strcmp(src->file, src->title) != 0 ? src->file : "";
        if (second[0]) draw_text_fit(a->r, a->ui.xs, second, textx, ry + 36, textw, C_FAINT);
        else if (src->trusted) draw_text(a->r, a->ui.xs, "trusted uploader", textx, ry + 36, C_GREEN, ALIGN_LEFT);
        char seeds[32];
        snprintf(seeds, sizeof(seeds), "%d seeds", src->seeds);
        int rx = px + pw - 36;
        draw_text(a->r, a->ui.sm, seeds, rx, ry + 8, src->seeds > 10 ? C_GREEN : src->seeds > 0 ? C_DIM : C_RED, ALIGN_RIGHT);
        char info[96];
        snprintf(info, sizeof(info), "%s%s%s", src->size, src->size[0] && src->provider[0] ? "  ·  " : "", src->provider);
        draw_text(a->r, a->ui.xs, info, rx, ry + 36, C_FAINT, ALIGN_RIGHT);
    }
    SDL_RenderSetClipRect(a->r, NULL);
    draw_list_scrollbar(a, px + pw - 10, ly, lh, count, visible, s->scroll);

    const char *hint = s->download_mode
        ? "Enter download   P play instead   Tab switch source   / edit Nyaa search   Esc close"
        : "Enter play   D download   Tab switch source   / edit Nyaa search   Esc close";
    draw_text(a->r, a->ui.xs, hint, px + 30, py + ph - 38, C_FAINT, ALIGN_LEFT);
}

bool sources_key(App *a, SDL_Keycode key)
{
    Sources *s = &a->src;
    SourceList *l = cur_list(s);
    int count = l ? l->count : 0;
    if (s->typing) {
        if (key == SDLK_ESCAPE) s->typing = false;
        else if (key == SDLK_BACKSPACE) {
            size_t n = strlen(s->nyaa_query);
            if (n) s->nyaa_query[n - 1] = '\0';
        } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            s->typing = false;
            s->sel = 0;
            source_list_free(s->lists[1]);
            s->lists[1] = NULL;
            fetch_nyaa(a);
        }
        return true;
    }
    switch (key) {
    case SDLK_ESCAPE:
    case SDLK_BACKSPACE:
        s->open = false;
        return true;
    case SDLK_TAB:
    case SDLK_LEFT:
    case SDLK_RIGHT:
        s->tab = key == SDLK_LEFT ? 0 : key == SDLK_RIGHT ? 1 : 1 - s->tab;
        s->sel = 0;
        s->scroll = 0;
        return true;
    case SDLK_UP:
        if (s->sel > 0) s->sel--;
        return true;
    case SDLK_DOWN:
        if (s->sel < count - 1) s->sel++;
        return true;
    case SDLK_PAGEDOWN:
        s->sel = s->sel + 8 < count ? s->sel + 8 : (count ? count - 1 : 0);
        return true;
    case SDLK_PAGEUP:
        s->sel = s->sel > 8 ? s->sel - 8 : 0;
        return true;
    case SDLK_SLASH:
        s->tab = 1;
        s->typing = true;
        s->nyaa_query[0] = '\0';
        return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        act_source(a, s->download_mode);
        return true;
    case SDLK_d:
        act_source(a, true);
        return true;
    case SDLK_p:
        act_source(a, false);
        return true;
    default:
        return false;
    }
}

void sources_text(App *a, const char *txt)
{
    Sources *s = &a->src;
    if (!s->typing) return;
    size_t n = strlen(s->nyaa_query);
    if (n + strlen(txt) < sizeof(s->nyaa_query)) strcat(s->nyaa_query, txt);
}

void sources_click(App *a, int x, int y)
{
    Sources *s = &a->src;
    int pw = win_w - 120 < 1100 ? win_w - 120 : 1100, ph = win_h - 100;
    int px = (win_w - pw) / 2, py = 50;
    if (!point_in(x, y, px, py, pw, ph)) {
        s->open = false;
        return;
    }
    int ty = py + 96;
    if (y >= ty && y < ty + 38) {
        if (x < px + 180) s->tab = 0;
        else if (x < px + 300) s->tab = 1;
        else if (s->tab == 1) s->typing = true;
        s->sel = 0;
        return;
    }
    int ly = ty + 56;
    int i = (int)((y - ly) / SRC_ROW_H + s->scroll);
    SourceList *l = cur_list(s);
    if (y >= ly && l && i >= 0 && i < l->count) {
        if (s->sel == i) act_source(a, s->download_mode);
        else s->sel = i;
    }
}

void sources_wheel(App *a, int dy)
{
    sources_key(a, dy < 0 ? SDLK_DOWN : SDLK_UP);
}

static bool is_video_name(const char *n)
{
    const char *ext = strrchr(n, '.');
    if (!ext) return false;
    const char *v[] = {".mkv", ".mp4", ".avi", ".webm", ".mov", ".m4v", ".ts", NULL};
    for (int i = 0; v[i]; i++)
        if (strcasecmp(ext, v[i]) == 0) return true;
    return false;
}

typedef struct {
    int idx;
    long long size;
    char name[256];
} FileRow;

static int cmp_file(const void *x, const void *y)
{
    return natural_compare(((const FileRow *)x)->name, ((const FileRow *)y)->name);
}

static void parse_files(Job *job)
{
    char *out = helper_capture("files", job->text, 90);
    if (!out) return;
    FileRow *rows = calloc(1024, sizeof(FileRow));
    int n = 0, videos = 0;
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line && rows && n < 1024; line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "FILE ", 5) != 0) continue;
        int idx;
        long long size;
        int off = 0;
        if (sscanf(line + 5, "%d %lld %n", &idx, &size, &off) < 2) continue;
        rows[n].idx = idx;
        rows[n].size = size;
        copy_text(rows[n].name, sizeof(rows[n].name), line + 5 + off);
        if (is_video_name(rows[n].name)) videos++;
        n++;
    }
    free(out);
    if (!rows) return;
    if (videos > 0) {
        int w = 0;
        for (int i = 0; i < n; i++)
            if (is_video_name(rows[i].name)) rows[w++] = rows[i];
        n = w;
    }
    qsort(rows, (size_t)n, sizeof(FileRow), cmp_file);
    job->tag = n;
    job->result = rows;
}

static void file_action(App *a, int which, bool download, bool all)
{
    Files *f = &a->files;
    char dest[PATH_MAX];
    media_dest(f->dest_name, dest, sizeof(dest));
    if (all) {
        download_start(f->magnet, -1, dest, f->title);
        app_toast(a, "Downloading all files to %s", dest);
        f->open = false;
        a->src.open = false;
        return;
    }
    if (which < 0 || which >= f->count) return;
    if (download) {
        download_start(f->magnet, f->idx[which], dest, basename_of(f->names[which]));
        app_toast(a, "Download started: %s", basename_of(f->names[which]));
    } else {
        char title[TITLE_LEN];
        if (f->play_title[0]) copy_text(title, sizeof(title), f->play_title);
        else clean_title(basename_of(f->names[which]), title, sizeof(title));
        play_torrent(f->magnet, f->idx[which], title[0] ? title : f->names[which], f->has_hist ? &f->hist : NULL);
    }
    f->open = false;
    a->src.open = false;
}

static void apply_files(Job *job)
{
    FileRow *rows = job->result;
    if (!G || !G->files.open || job->gen != G->files.gen) {
        free(rows);
        return;
    }
    Files *f = &G->files;
    f->loading = false;
    if (!rows || job->tag == 0) {
        copy_text(f->error, sizeof(f->error), "Could not read the torrent (no peers with metadata?)");
        free(rows);
        return;
    }
    f->count = job->tag;
    for (int i = 0; i < f->count; i++) {
        f->idx[i] = rows[i].idx;
        f->size[i] = rows[i].size;
        copy_text(f->names[i], sizeof(f->names[i]), rows[i].name);
        int s, e;
        if (f->want_episode > 0 && parse_episode_tag(basename_of(rows[i].name), &s, &e) && e == f->want_episode)
            f->sel = i;
    }
    free(rows);
    if (f->count == 1 && f->auto_action) file_action(G, 0, f->auto_action == 2, false);
}

void files_open(App *a, const char *magnet, const char *title, const char *dest_name,
                const char *play_title, int want_episode, int auto_action, const HistoryEntry *hist)
{
    G = a;
    Files *f = &a->files;
    unsigned gen = f->gen + 1;
    memset(f, 0, sizeof(*f));
    f->gen = gen;
    f->open = true;
    f->loading = true;
    copy_text(f->magnet, sizeof(f->magnet), magnet);
    copy_text(f->title, sizeof(f->title), title);
    copy_text(f->dest_name, sizeof(f->dest_name), dest_name);
    copy_text(f->play_title, sizeof(f->play_title), play_title ? play_title : "");
    f->want_episode = want_episode;
    f->auto_action = auto_action;
    if (hist) {
        f->hist = *hist;
        f->has_hist = true;
    }
    Job *job = job_new("", 0, parse_files, apply_files);
    if (!job) return;
    copy_text(job->text, sizeof(job->text), magnet);
    job->gen = f->gen;
    net_submit(job);
}

void files_render(App *a)
{
    Files *f = &a->files;
    int px, py, pw, ph;
    panel(a, &px, &py, &pw, &ph);
    draw_text_fit(a->r, a->ui.lg, f->title, px + 30, py + 24, pw - 60, C_WHITE);
    char sub[128];
    if (f->loading) snprintf(sub, sizeof(sub), "Getting the file list from peers...");
    else if (f->error[0]) copy_text(sub, sizeof(sub), f->error);
    else snprintf(sub, sizeof(sub), "%d video files", f->count);
    draw_text(a->r, a->ui.sm, sub, px + 30, py + 60, f->error[0] ? C_RED : C_DIM, ALIGN_LEFT);

    int ly = py + 100, lh = py + ph - 56 - ly;
    int visible = lh / FILE_ROW_H;
    float want = (float)(f->sel - visible / 2);
    if (want > f->count - visible) want = (float)(f->count - visible);
    if (want < 0) want = 0;
    f->scroll += (want - f->scroll) * 0.3f;
    SDL_Rect clip = {px, ly, pw, lh};
    SDL_RenderSetClipRect(a->r, &clip);
    for (int i = 0; i < f->count; i++) {
        int ry = ly + (int)((i - f->scroll) * FILE_ROW_H);
        if (ry + FILE_ROW_H < ly || ry > ly + lh) continue;
        bool sel = f->sel == i;
        if (sel) fill_round(a->r, px + 16, ry, pw - 32, FILE_ROW_H - 4, 6, (Color){255, 255, 255, 34});
        draw_text_fit(a->r, a->ui.md, basename_of(f->names[i]), px + 30, ry + 9, pw - 220, sel ? C_WHITE : C_TEXT);
        char size[32];
        format_bytes((double)f->size[i], size, sizeof(size));
        draw_text(a->r, a->ui.sm, size, px + pw - 36, ry + 11, C_DIM, ALIGN_RIGHT);
    }
    SDL_RenderSetClipRect(a->r, NULL);
    draw_list_scrollbar(a, px + pw - 10, ly, lh, f->count, visible, f->scroll);
    draw_text(a->r, a->ui.xs, "Enter play   D download file   A download all   Esc back",
              px + 30, py + ph - 38, C_FAINT, ALIGN_LEFT);
}

bool files_key(App *a, SDL_Keycode key)
{
    Files *f = &a->files;
    switch (key) {
    case SDLK_ESCAPE:
    case SDLK_BACKSPACE:
        f->open = false;
        return true;
    case SDLK_UP:
        if (f->sel > 0) f->sel--;
        return true;
    case SDLK_DOWN:
        if (f->sel < f->count - 1) f->sel++;
        return true;
    case SDLK_PAGEDOWN:
        f->sel = f->sel + 10 < f->count ? f->sel + 10 : (f->count ? f->count - 1 : 0);
        return true;
    case SDLK_PAGEUP:
        f->sel = f->sel > 10 ? f->sel - 10 : 0;
        return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_p:
        file_action(a, f->sel, false, false);
        return true;
    case SDLK_d:
        file_action(a, f->sel, true, false);
        return true;
    case SDLK_a:
        if (!f->loading) file_action(a, -1, true, true);
        return true;
    default:
        return false;
    }
}

void files_click(App *a, int x, int y)
{
    Files *f = &a->files;
    int pw = win_w - 120 < 1100 ? win_w - 120 : 1100;
    int px = (win_w - pw) / 2, py = 50;
    if (!point_in(x, y, px, py, pw, win_h - 100)) {
        f->open = false;
        return;
    }
    int ly = py + 100;
    int i = (int)((y - ly) / FILE_ROW_H + f->scroll);
    if (y >= ly && i >= 0 && i < f->count) {
        if (f->sel == i) file_action(a, i, false, false);
        else f->sel = i;
    }
}

void files_wheel(App *a, int dy)
{
    files_key(a, dy < 0 ? SDLK_DOWN : SDLK_UP);
}
