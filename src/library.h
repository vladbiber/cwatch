#ifndef CWATCH_LIBRARY_H
#define CWATCH_LIBRARY_H

#include "common.h"

#define MAX_ITEMS 4096
#define MAX_COLLECTIONS 512
#define MAX_ROOTS 32

typedef struct {
    char path[PATH_MAX];
    char title[TITLE_LEN];
    int collection;
    int season;
    int episode;
    int order;
    bool has_progress;
    double progress_sec;
    double duration;
    long progress_mtime;
    bool watched;
} LocalItem;

typedef struct {
    char name[TITLE_LEN];
    char key[PATH_MAX];
    int items[512];
    int count;
    long last_played;
    bool matched;
    Meta match;
} Collection;

typedef struct {
    LocalItem items[MAX_ITEMS];
    int item_count;
    Collection cols[MAX_COLLECTIONS];
    int col_count;
    char roots[MAX_ROOTS][PATH_MAX];
    int root_count;
} Library;

void library_init(Library *lib, const char *root);
void library_scan(Library *lib);
bool library_add_root(Library *lib, const char *path);
void library_reload_progress(Library *lib);
void library_toggle_watched(Library *lib, int item);
void library_set_match(Library *lib, int col, const Meta *m);
void library_request_matches(Library *lib);
int library_next_item(const Library *lib, int col);
int library_find_episode(const Library *lib, int col, int season, int episode);
int library_find_by_name(const Library *lib, const char *name);
double watch_later_progress(const char *target, long *mtime);
bool library_take_dirty(void);
bool library_ep_watched(const char *ep_id);
void library_request_durations(Library *lib);

#endif
