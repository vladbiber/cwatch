#include "library.h"
#include "catalog.h"
#include "net.h"

#include <ctype.h>
#include <dirent.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_WATCHED_EPS 4096

static Library *active_lib;
static char watched_eps[MAX_WATCHED_EPS][96];
static int watched_ep_count;
static unsigned scan_gen;
static bool dirty;

static bool is_video(const char *name)
{
    static const char *exts[] = {".mp4", ".mkv", ".webm", ".avi", ".mov", ".m4v",
                                 ".flv", ".wmv", ".ogv", ".ts", NULL};
    const char *ext = strrchr(name, '.');
    if (!ext) return false;
    for (int i = 0; exts[i]; i++)
        if (strcasecmp(ext, exts[i]) == 0) return true;
    return false;
}

void library_init(Library *lib, const char *root)
{
    memset(lib, 0, sizeof(*lib));
    char path[PATH_MAX];
    expand_path(root, path, sizeof(path));
    copy_text(lib->roots[0], PATH_MAX, path);
    lib->root_count = 1;
    active_lib = lib;

    char file[PATH_MAX];
    state_file(file, sizeof(file), "roots.txt");
    FILE *fp = fopen(file, "r");
    if (!fp) return;
    char line[PATH_MAX];
    while (fgets(line, sizeof(line), fp) && lib->root_count < MAX_ROOTS) {
        trim_line(line);
        if (line[0] && strcmp(line, lib->roots[0]) != 0)
            copy_text(lib->roots[lib->root_count++], PATH_MAX, line);
    }
    fclose(fp);
}

static int collection_get(Library *lib, const char *key, const char *name)
{
    for (int i = 0; i < lib->col_count; i++)
        if (strcmp(lib->cols[i].key, key) == 0) return i;
    if (lib->col_count >= MAX_COLLECTIONS) return -1;
    Collection *c = &lib->cols[lib->col_count];
    memset(c, 0, sizeof(*c));
    copy_text(c->key, sizeof(c->key), key);
    copy_text(c->name, sizeof(c->name), name);
    c->match.local_item = -1;
    return lib->col_count++;
}

static void add_item(Library *lib, const char *path, const char *fname, int col)
{
    if (lib->item_count >= MAX_ITEMS || col < 0) return;
    Collection *c = &lib->cols[col];
    if (c->count >= (int)(sizeof(c->items) / sizeof(c->items[0]))) return;
    LocalItem *it = &lib->items[lib->item_count];
    memset(it, 0, sizeof(*it));
    copy_text(it->path, sizeof(it->path), path);
    copy_text(it->title, sizeof(it->title), fname);
    char *dot = strrchr(it->title, '.');
    if (dot) *dot = '\0';
    prettify_name(it->title, it->title, sizeof(it->title));
    it->collection = col;
    c->items[c->count++] = lib->item_count;
    lib->item_count++;
}

static void scan_dir(Library *lib, const char *dir_path, const char *root, int col, int depth)
{
    if (depth > 12) return;
    DIR *dir = opendir(dir_path);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char full[PATH_MAX];
        if (!path_join(full, sizeof(full), dir_path, entry->d_name)) continue;
        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            int c = col;
            if (depth == 0) {
                char name[TITLE_LEN];
                prettify_name(entry->d_name, name, sizeof(name));
                c = collection_get(lib, full, name);
            }
            scan_dir(lib, full, root, c, depth + 1);
            continue;
        }
        if (!S_ISREG(st.st_mode) || !is_video(entry->d_name)) continue;
        if (st.st_size < 1024 * 1024) continue;

        int c = col;
        if (depth == 0) {
            char name[TITLE_LEN], key[PATH_MAX];
            clean_title(entry->d_name, name, sizeof(name));
            if (!name[0]) prettify_name(entry->d_name, name, sizeof(name));
            snprintf(key, sizeof(key), "group:%s/%s", root, name);
            c = collection_get(lib, key, name);
        }
        add_item(lib, full, entry->d_name, c);
    }
    closedir(dir);
}

static int cmp_items(const void *a, const void *b)
{
    const LocalItem *x = &active_lib->items[*(const int *)a];
    const LocalItem *y = &active_lib->items[*(const int *)b];
    if (x->season && y->season && (x->season != y->season || x->episode != y->episode))
        return x->season != y->season ? x->season - y->season : x->episode - y->episode;
    return natural_compare(x->title, y->title);
}

double watch_later_progress(const char *target, long *mtime)
{
    static char cached_dir[PATH_MAX];
    if (!cached_dir[0]) watch_later_dir(cached_dir, sizeof(cached_dir));
    DIR *dir = opendir(cached_dir);
    if (!dir) return 0;
    double result = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char full[PATH_MAX];
        if (!path_join(full, sizeof(full), cached_dir, entry->d_name)) continue;
        FILE *fp = fopen(full, "r");
        if (!fp) continue;
        char line[PATH_MAX + 64];
        bool match = false;
        double start = 0;
        while (fgets(line, sizeof(line), fp)) {
            trim_line(line);
            if (strncmp(line, "# ", 2) == 0) match = strcmp(line + 2, target) == 0;
            else if (strncmp(line, "start=", 6) == 0) start = atof(line + 6);
        }
        fclose(fp);
        if (match && start > 0) {
            struct stat st;
            if (mtime && stat(full, &st) == 0) *mtime = (long)st.st_mtime;
            result = start;
            break;
        }
    }
    closedir(dir);
    return result;
}

static void load_progress(Library *lib)
{
    char dir_path[PATH_MAX];
    watch_later_dir(dir_path, sizeof(dir_path));
    for (int i = 0; i < lib->item_count; i++) {
        lib->items[i].has_progress = false;
        lib->items[i].progress_sec = 0;
    }
    for (int i = 0; i < lib->col_count; i++) lib->cols[i].last_played = 0;

    DIR *dir = opendir(dir_path);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char full[PATH_MAX];
        if (!path_join(full, sizeof(full), dir_path, entry->d_name)) continue;
        FILE *fp = fopen(full, "r");
        if (!fp) continue;
        char line[PATH_MAX + 64], target[PATH_MAX] = "";
        double start = 0;
        while (fgets(line, sizeof(line), fp)) {
            trim_line(line);
            if (strncmp(line, "# ", 2) == 0 && !target[0]) copy_text(target, sizeof(target), line + 2);
            else if (strncmp(line, "start=", 6) == 0) start = atof(line + 6);
        }
        fclose(fp);
        if (!target[0] || target[0] != '/') continue;
        struct stat st;
        long mt = stat(full, &st) == 0 ? (long)st.st_mtime : 0;
        for (int i = 0; i < lib->item_count; i++) {
            LocalItem *it = &lib->items[i];
            if (strcmp(it->path, target) != 0) continue;
            it->has_progress = start > 0;
            it->progress_sec = start;
            it->progress_mtime = mt;
            Collection *c = &lib->cols[it->collection];
            if (mt > c->last_played) c->last_played = mt;
        }
    }
    closedir(dir);

    for (int i = 0; i < lib->item_count; i++) lib->items[i].watched = false;
    watched_ep_count = 0;
    const char *files[] = {"watched.txt", "watched_auto.txt"};
    for (int f = 0; f < 2; f++) {
        char wpath[PATH_MAX];
        state_file(wpath, sizeof(wpath), files[f]);
        FILE *fp = fopen(wpath, "r");
        if (!fp) continue;
        char line[PATH_MAX + 16];
        while (fgets(line, sizeof(line), fp)) {
            trim_line(line);
            if (!line[0]) continue;
            if (line[0] != '/') {
                if (watched_ep_count < MAX_WATCHED_EPS && !library_ep_watched(line))
                    copy_text(watched_eps[watched_ep_count++], sizeof(watched_eps[0]), line);
                continue;
            }
            for (int i = 0; i < lib->item_count; i++)
                if (strcmp(lib->items[i].path, line) == 0) lib->items[i].watched = true;
        }
        fclose(fp);
    }
}

bool library_ep_watched(const char *ep_id)
{
    if (!ep_id || !ep_id[0]) return false;
    for (int i = 0; i < watched_ep_count; i++)
        if (strcmp(watched_eps[i], ep_id) == 0) return true;
    return false;
}

static void load_matches(Library *lib)
{
    char path[PATH_MAX];
    state_file(path, sizeof(path), "matches.json");
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    fclose(fp);
    json_object *root = json_object_from_file(path);
    if (!root) return;
    for (int i = 0; i < lib->col_count; i++) {
        json_object *o;
        if (!json_object_object_get_ex(root, lib->cols[i].key, &o)) continue;
        Collection *c = &lib->cols[i];
        c->matched = true;
        json_object *v;
#define GET(field) if (json_object_object_get_ex(o, #field, &v)) copy_text(c->match.field, sizeof(c->match.field), json_object_get_string(v))
        GET(id); GET(type); GET(name); GET(poster); GET(background); GET(logo);
        GET(year); GET(rating); GET(genres); GET(desc);
#undef GET
    }
    json_object_put(root);
}

static void save_matches(Library *lib)
{
    char path[PATH_MAX];
    state_file(path, sizeof(path), "matches.json");
    json_object *root = json_object_from_file(path);
    if (!root) root = json_object_new_object();
    for (int i = 0; i < lib->col_count; i++) {
        Collection *c = &lib->cols[i];
        if (!c->matched) continue;
        json_object *o = json_object_new_object();
#define PUT(field) json_object_object_add(o, #field, json_object_new_string(c->match.field))
        PUT(id); PUT(type); PUT(name); PUT(poster); PUT(background); PUT(logo);
        PUT(year); PUT(rating); PUT(genres); PUT(desc);
#undef PUT
        json_object_object_add(root, c->key, o);
    }
    json_object_to_file_ext(path, root, JSON_C_TO_STRING_PRETTY);
    json_object_put(root);
}

void library_scan(Library *lib)
{
    active_lib = lib;
    scan_gen++;
    lib->item_count = 0;
    lib->col_count = 0;
    for (int r = 0; r < lib->root_count; r++) {
        mkdir_p(lib->roots[r]);
        scan_dir(lib, lib->roots[r], lib->roots[r], -1, 0);
    }
    for (int i = 0; i < lib->item_count; i++) {
        LocalItem *it = &lib->items[i];
        if (!parse_episode_tag(basename_of(it->path), &it->season, &it->episode))
            it->season = it->episode = 0;
    }
    for (int c = 0; c < lib->col_count; c++) {
        Collection *col = &lib->cols[c];
        qsort(col->items, (size_t)col->count, sizeof(int), cmp_items);
        for (int j = 0; j < col->count; j++) {
            LocalItem *it = &lib->items[col->items[j]];
            it->order = j + 1;
            if (!it->episode) {
                it->season = 1;
                it->episode = j + 1;
            }
        }
    }
    load_progress(lib);
    load_matches(lib);
}

bool library_add_root(Library *lib, const char *input)
{
    char path[PATH_MAX];
    expand_path(input, path, sizeof(path));
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    for (int i = 0; i < lib->root_count; i++)
        if (strcmp(lib->roots[i], path) == 0) return true;
    if (lib->root_count >= MAX_ROOTS) return false;
    copy_text(lib->roots[lib->root_count++], PATH_MAX, path);

    char file[PATH_MAX];
    state_file(file, sizeof(file), "roots.txt");
    FILE *fp = fopen(file, "w");
    if (fp) {
        for (int i = 1; i < lib->root_count; i++) fprintf(fp, "%s\n", lib->roots[i]);
        fclose(fp);
    }
    library_scan(lib);
    library_request_matches(lib);
    return true;
}

void library_reload_progress(Library *lib)
{
    load_progress(lib);
}

static void rewrite_without(const char *name, const char *drop)
{
    char path[PATH_MAX], tmp[PATH_MAX + 8];
    state_file(path, sizeof(path), name);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *in = fopen(path, "r");
    if (!in) return;
    FILE *out = fopen(tmp, "w");
    if (!out) {
        fclose(in);
        return;
    }
    char line[PATH_MAX + 16];
    while (fgets(line, sizeof(line), in)) {
        trim_line(line);
        if (line[0] && strcmp(line, drop) != 0) fprintf(out, "%s\n", line);
    }
    fclose(in);
    fclose(out);
    rename(tmp, path);
}

void library_toggle_watched(Library *lib, int item)
{
    if (item < 0 || item >= lib->item_count) return;
    LocalItem *it = &lib->items[item];
    it->watched = !it->watched;
    if (it->watched) {
        char path[PATH_MAX];
        state_file(path, sizeof(path), "watched.txt");
        FILE *fp = fopen(path, "a");
        if (fp) {
            fprintf(fp, "%s\n", it->path);
            fclose(fp);
        }
    } else {
        rewrite_without("watched.txt", it->path);
        rewrite_without("watched_auto.txt", it->path);
    }
}

void library_set_match(Library *lib, int col, const Meta *m)
{
    if (col < 0 || col >= lib->col_count) return;
    Collection *c = &lib->cols[col];
    c->matched = true;
    c->match = *m;
    c->match.local_item = -1;
    save_matches(lib);
    dirty = true;
}

static int tokenize(const char *in, char tokens[][32], int max)
{
    int n = 0, len = 0;
    for (const char *p = in;; p++) {
        unsigned char c = (unsigned char)*p;
        if (c && (isalnum(c) || c >= 0x80)) {
            if (len < 31 && n < max) tokens[n][len++] = (char)tolower(c);
            continue;
        }
        if (len > 0 && n < max) {
            tokens[n][len] = '\0';
            n++;
        }
        len = 0;
        if (!c) break;
    }
    return n;
}

static double name_score(const char *query, const char *name)
{
    char q[16][32], r[16][32];
    int nq = tokenize(query, q, 16), nr = tokenize(name, r, 16);
    if (nq == 0 || nr == 0) return 0;
    int common = 0;
    for (int i = 0; i < nq; i++)
        for (int j = 0; j < nr; j++)
            if (strcmp(q[i], r[j]) == 0) {
                common++;
                break;
            }
    return (double)common / (nq > nr ? nq : nr);
}

static void apply_match(Job *job)
{
    MetaList *l = job->result;
    if (job->gen == scan_gen && active_lib && job->tag < active_lib->col_count &&
        strcmp(active_lib->cols[job->tag].key, job->text) == 0) {
        Meta none = {0};
        copy_text(none.name, sizeof(none.name), active_lib->cols[job->tag].name);
        const Meta *best = NULL;
        double best_score = 0.74;
        for (int i = 0; l && i < l->count && i < 8; i++) {
            double sc = name_score(active_lib->cols[job->tag].name, l->items[i].name);
            if (sc > best_score) {
                best = &l->items[i];
                best_score = sc;
            }
        }
        if (job->ok) library_set_match(active_lib, job->tag, best ? best : &none);
    }
    meta_list_free(l);
}

static void parse_match(Job *job)
{
    job->result = parse_meta_list(job->data);
}

void library_request_matches(Library *lib)
{
    for (int i = 0; i < lib->col_count; i++) {
        Collection *c = &lib->cols[i];
        if (c->matched || c->count == 0) continue;
        char url[URL_LEN];
        catalog_url(url, sizeof(url), CINEMETA, c->count > 1 ? "series" : "movie", "top", NULL, c->name);
        Job *job = job_new(url, 86400, parse_match, apply_match);
        if (!job) continue;
        job->tag = i;
        job->gen = scan_gen;
        copy_text(job->text, sizeof(job->text), c->key);
        net_submit(job);
    }
}

int library_next_item(const Library *lib, int col)
{
    if (col < 0 || col >= lib->col_count) return -1;
    const Collection *c = &lib->cols[col];
    int best = -1;
    long best_mt = -1;
    for (int j = 0; j < c->count; j++) {
        const LocalItem *it = &lib->items[c->items[j]];
        if (it->has_progress && it->progress_mtime > best_mt) {
            best = j;
            best_mt = it->progress_mtime;
        }
    }
    if (best >= 0) {
        const LocalItem *it = &lib->items[c->items[best]];
        bool done = it->watched || (it->duration > 0 && it->progress_sec > it->duration * 0.9);
        if (!done) return c->items[best];
        for (int j = best + 1; j < c->count; j++)
            if (!lib->items[c->items[j]].watched) return c->items[j];
        return c->items[best];
    }
    for (int j = 0; j < c->count; j++)
        if (!lib->items[c->items[j]].watched) return c->items[j];
    return c->count > 0 ? c->items[0] : -1;
}

int library_find_episode(const Library *lib, int col, int season, int episode)
{
    if (col < 0 || col >= lib->col_count) return -1;
    const Collection *c = &lib->cols[col];
    for (int j = 0; j < c->count; j++) {
        const LocalItem *it = &lib->items[c->items[j]];
        if (it->season == season && it->episode == episode) return c->items[j];
    }
    return -1;
}

int library_find_by_name(const Library *lib, const char *name)
{
    for (int i = 0; i < lib->col_count; i++) {
        const Collection *c = &lib->cols[i];
        if (c->count == 0) continue;
        if (strcasecmp(c->name, name) == 0) return i;
        if (c->matched && strcasecmp(c->match.name, name) == 0) return i;
    }
    return -1;
}

bool library_take_dirty(void)
{
    bool d = dirty;
    dirty = false;
    return d;
}

static void parse_duration(Job *job)
{
    char cmd[PATH_MAX + 128], quoted[PATH_MAX * 2];
    size_t o = 0;
    quoted[o++] = '\'';
    for (const char *p = job->text; *p && o + 6 < sizeof(quoted); p++) {
        if (*p == '\'') {
            memcpy(quoted + o, "'\\''", 4);
            o += 4;
        } else {
            quoted[o++] = *p;
        }
    }
    quoted[o++] = '\'';
    quoted[o] = '\0';
    snprintf(cmd, sizeof(cmd), "ffprobe -v error -show_entries format=duration -of csv=p=0 %s 2>/dev/null", quoted);
    FILE *fp = popen(cmd, "r");
    if (!fp) return;
    char buf[64] = "";
    if (fgets(buf, sizeof(buf), fp)) {
        double *d = malloc(sizeof(double));
        if (d) {
            *d = atof(buf);
            job->result = d;
        }
    }
    pclose(fp);
}

static void apply_duration(Job *job)
{
    double *d = job->result;
    if (d && active_lib && job->gen == scan_gen) {
        for (int i = 0; i < active_lib->item_count; i++)
            if (strcmp(active_lib->items[i].path, job->text) == 0) active_lib->items[i].duration = *d;
        dirty = true;
    }
    free(d);
}

void library_request_durations(Library *lib)
{
    if (!command_exists("ffprobe")) return;
    for (int i = 0; i < lib->item_count; i++) {
        LocalItem *it = &lib->items[i];
        if (!it->has_progress || it->duration > 0) continue;
        Job *job = job_new("", 0, parse_duration, apply_duration);
        if (!job) continue;
        copy_text(job->text, sizeof(job->text), it->path);
        job->gen = scan_gen;
        job->low_priority = true;
        net_submit(job);
    }
}
