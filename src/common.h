#ifndef CWATCH_COMMON_H
#define CWATCH_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define APP_TITLE "cwatch"
#define TITLE_LEN 256
#define SECTION_LEN 128
#define URL_LEN 1024
#define DESC_LEN 1200
#define MAX_PLAYERS 8

#define CINEMETA "https://v3-cinemeta.strem.io"
#define KITSU "https://anime-kitsu.strem.fun"
#define TORRENTIO "https://torrentio.strem.fun/qualityfilter=scr,cam"

typedef struct {
    char id[64];
    char type[16];
    char name[TITLE_LEN];
    char poster[URL_LEN];
    char background[URL_LEN];
    char logo[URL_LEN];
    char year[32];
    char rating[16];
    char genres[160];
    char runtime[32];
    char desc[DESC_LEN];
    char sub[128];
    int local_col;
    int local_item;
    int hist;
    double progress;
} Meta;

typedef struct {
    char id[96];
    int season;
    int episode;
    char title[TITLE_LEN];
    char thumb[URL_LEN];
    char overview[600];
    char released[16];
    int local_item;
} Episode;

typedef struct {
    Meta meta;
    Episode *eps;
    int ep_count;
} MetaFull;

typedef struct {
    char provider[32];
    char quality[24];
    char title[TITLE_LEN * 2];
    char file[TITLE_LEN];
    char size[32];
    char hash[48];
    char torrent_url[URL_LEN];
    int seeds;
    int file_idx;
    bool trusted;
} Source;

typedef struct {
    TTF_Font *xs;
    TTF_Font *sm;
    TTF_Font *md;
    TTF_Font *mdb;
    TTF_Font *lg;
    TTF_Font *xl;
    TTF_Font *hero;
} UI;

typedef struct {
    char name[32];
    char exec[64];
} Player;

extern int win_w;
extern int win_h;
extern unsigned long frame_no;

void copy_text(char *dst, size_t dst_len, const char *src);
bool path_join(char *out, size_t out_len, const char *left, const char *right);
int natural_compare(const char *a, const char *b);
bool command_exists(const char *cmd);
unsigned long hash_string(const char *s);
void expand_path(const char *in, char *out, size_t out_len);
const char *basename_of(const char *path);
void prettify_name(const char *in, char *out, size_t out_len);
void trim_line(char *line);
void mkdir_p(const char *path);
void state_dir(char *out, size_t out_len);
void state_file(char *out, size_t out_len, const char *name);
void cache_dir(char *out, size_t out_len, const char *sub);
void watch_later_dir(char *out, size_t out_len);
void url_encode(const char *in, char *out, size_t out_len);
void format_clock(double seconds, char *out, size_t out_len);
void format_bytes(double bytes, char *out, size_t out_len);
void clean_title(const char *in, char *out, size_t out_len);
bool parse_episode_tag(const char *name, int *season, int *episode);
void make_magnet(const char *hash, const char *name, char *out, size_t out_len);

#endif
