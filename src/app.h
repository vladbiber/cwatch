#ifndef CWATCH_APP_H
#define CWATCH_APP_H

#include "catalog.h"
#include "common.h"
#include "gfx.h"
#include "library.h"
#include "playback.h"

#define MAX_ROWS 24
#define NAV_H 76

enum { TAB_SEARCH, TAB_HOME, TAB_SHOWS, TAB_MOVIES, TAB_ANIME, TAB_LIBRARY, TAB_NYAA, TAB_DOWNLOADS, TAB_COUNT };
enum { ROW_POSTER, ROW_WIDE };
enum { FOCUS_NAV = -2, FOCUS_HERO = -1 };
enum { DF_BUTTONS, DF_SEASONS, DF_EPISODES };

typedef struct {
    char title[64];
    int style;
    Meta *items;
    int count;
    int sel;
    float scroll;
    bool loading;
    bool failed;
} Row;

typedef struct {
    bool open;
    bool loading;
    MetaFull *mf;
    int col;
    int seasons[64];
    int season_count;
    int season_sel;
    int focus;
    int button;
    int ep_sel;
    float ep_scroll;
    unsigned gen;
} Detail;

typedef struct {
    bool open;
    char heading[TITLE_LEN * 2];
    char type[16];
    char stream_id[96];
    char dest_name[TITLE_LEN];
    char play_title[TITLE_LEN];
    char nyaa_query[TITLE_LEN];
    int season;
    int episode;
    SourceList *lists[2];
    bool loading[2];
    int tab;
    int sel;
    float scroll;
    bool typing;
    bool download_mode;
    unsigned gen;
    HistoryEntry hist;
} Sources;

typedef struct {
    bool open;
    bool loading;
    char magnet[URL_LEN * 2];
    char title[TITLE_LEN * 2];
    char dest_name[TITLE_LEN];
    int idx[1024];
    long long size[1024];
    char names[1024][256];
    int count;
    int sel;
    float scroll;
    int want_episode;
    unsigned gen;
    int auto_action;
    char error[TITLE_LEN];
    char play_title[TITLE_LEN];
    HistoryEntry hist;
    bool has_hist;
} Files;

typedef struct {
    SDL_Renderer *r;
    UI ui;
    Library *lib;
    Player players[MAX_PLAYERS];
    int player_count;
    int player_idx;

    int tab;
    int nav_sel;
    int focus;
    int hero_button;
    Row rows[MAX_ROWS];
    int row_count;
    float row_anim;
    unsigned rows_gen;
    Meta hero;
    Uint32 hero_changed;

    char query[TITLE_LEN];
    bool typing;
    Uint32 query_changed;
    bool query_dirty;

    SourceList *nyaa;
    bool nyaa_loading;
    int nyaa_sel;
    float nyaa_scroll;
    char nyaa_query[TITLE_LEN];
    unsigned nyaa_gen;

    int dl_sel;
    char input_root[PATH_MAX];
    bool input_root_active;

    Detail det;
    Sources src;
    Files files;

    char toast[TITLE_LEN * 2];
    Uint32 toast_until;
    int mouse_x, mouse_y;
    bool running;
} App;

void app_toast(App *a, const char *fmt, ...);
void browse_build(App *a);
void browse_refresh_local(App *a);
void browse_render(App *a);
bool browse_key(App *a, SDL_Keycode key);
void browse_click(App *a, int x, int y);
void browse_wheel(App *a, int dx, int dy);
void browse_hover(App *a, int x, int y);
void browse_tick(App *a);
void nav_render(App *a);
int nav_hit(App *a, int x, int y);
void set_tab(App *a, int tab);
void open_meta(App *a, const Meta *m);
void play_meta(App *a, const Meta *m);
void play_local(App *a, int item, const char *show);
void binge_tick(App *a);

void detail_open_meta(App *a, const Meta *m);
void detail_close(App *a);
void detail_render(App *a);
bool detail_key(App *a, SDL_Keycode key);
void detail_click(App *a, int x, int y);
void detail_wheel(App *a, int dy);

void sources_open(App *a, const char *type, const char *stream_id, const char *heading,
                  const char *dest_name, const char *play_title, const char *nyaa_query,
                  int season, int episode, const HistoryEntry *hist);
void sources_render(App *a);
bool sources_key(App *a, SDL_Keycode key);
void sources_text(App *a, const char *txt);
void sources_click(App *a, int x, int y);
void sources_wheel(App *a, int dy);

void files_open(App *a, const char *magnet, const char *title, const char *dest_name,
                const char *play_title, int want_episode, int auto_action, const HistoryEntry *hist);
void files_render(App *a);
bool files_key(App *a, SDL_Keycode key);
void files_click(App *a, int x, int y);
void files_wheel(App *a, int dy);

void nyaa_search(App *a, const char *query);
void nyaa_render(App *a);
bool nyaa_key(App *a, SDL_Keycode key);
void nyaa_click(App *a, int x, int y);
void nyaa_wheel(App *a, int dy);
void downloads_render(App *a);
bool downloads_key(App *a, SDL_Keycode key);
void downloads_click(App *a, int x, int y);
void status_render(App *a);
void start_source(App *a, const Source *s, bool download, const char *dest_name,
                  const char *play_title, int want_episode, const HistoryEntry *hist);
void media_dest(const char *name, char *out, size_t len);
void draw_list_scrollbar(App *a, int x, int y, int h, int total, int visible, float scroll);

#endif
