#include "images.h"
#include "net.h"

#include <SDL2/SDL_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define IMG_SLOTS 4096
#define IMG_KEEP 260

enum { IMG_EMPTY, IMG_LOADING, IMG_READY, IMG_FAILED };

typedef struct {
    char *key;
    int state;
    SDL_Texture *tex;
    unsigned long used;
} ImgEntry;

static SDL_Renderer *ren;
static ImgEntry slots[IMG_SLOTS];
static int ready_count;

void images_init(SDL_Renderer *r)
{
    ren = r;
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG | IMG_INIT_WEBP);
}

void images_shutdown(void)
{
    for (int i = 0; i < IMG_SLOTS; i++) {
        if (slots[i].tex) SDL_DestroyTexture(slots[i].tex);
        if (slots[i].state != IMG_LOADING) free(slots[i].key);
    }
    IMG_Quit();
}

static ImgEntry *find_slot(const char *key, bool create)
{
    unsigned long h = hash_string(key);
    for (int i = 0; i < IMG_SLOTS; i++) {
        ImgEntry *e = &slots[(h + (unsigned long)i) % IMG_SLOTS];
        if (!e->key) {
            if (!create) return NULL;
            e->key = strdup(key);
            return e->key ? e : NULL;
        }
        if (strcmp(e->key, key) == 0) return e;
    }
    return NULL;
}

static SDL_Surface *shrink(SDL_Surface *s, int max_w)
{
    if (!s || s->w <= max_w) return s;
    int w = max_w, h = s->h * max_w / s->w;
    SDL_Surface *src = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (src && dst) {
        SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_NONE);
        SDL_BlitScaled(src, NULL, dst, NULL);
        SDL_FreeSurface(src);
        SDL_FreeSurface(s);
        return dst;
    }
    if (src) SDL_FreeSurface(src);
    if (dst) SDL_FreeSurface(dst);
    return s;
}

static void parse_image(Job *job)
{
    if (!job->ok || !job->data) return;
    SDL_RWops *rw = SDL_RWFromConstMem(job->data, (int)job->len);
    if (!rw) return;
    job->result = shrink(IMG_Load_RW(rw, 1), 1600);
}

static bool make_thumbnail(const char *video, char *out, size_t out_len)
{
    char dir[PATH_MAX], name[96];
    cache_dir(dir, sizeof(dir), "thumbs");
    struct stat st;
    long mtime = stat(video, &st) == 0 ? (long)st.st_mtime : 0;
    snprintf(name, sizeof(name), "%016lx_%ld_v3.jpg", hash_string(video), mtime);
    if (!path_join(out, out_len, dir, name)) return false;
    if (access(out, R_OK) == 0) return true;
    if (!command_exists("ffmpeg")) return false;

    pid_t pid = fork();
    if (pid == 0) {
        execlp("ffmpeg", "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
               "-ss", "90", "-i", video, "-frames:v", "1",
               "-vf", "thumbnail=60,scale=640:360:force_original_aspect_ratio=increase,crop=640:360",
               "-q:v", "4", out, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return false;
    int status = 0;
    waitpid(pid, &status, 0);
    if (access(out, R_OK) == 0) return true;

    pid = fork();
    if (pid == 0) {
        execlp("ffmpeg", "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
               "-i", video, "-frames:v", "1",
               "-vf", "scale=640:360:force_original_aspect_ratio=increase,crop=640:360",
               "-q:v", "4", out, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return false;
    waitpid(pid, &status, 0);
    return access(out, R_OK) == 0;
}

static void parse_thumb(Job *job)
{
    char path[PATH_MAX];
    if (make_thumbnail(job->text[0] ? job->text : "", path, sizeof(path)))
        job->result = IMG_Load(path);
}

static void apply_image(Job *job)
{
    ImgEntry *e = find_slot(job->url[0] ? job->url : job->text, false);
    SDL_Surface *s = job->result;
    if (!e) {
        if (s) SDL_FreeSurface(s);
        return;
    }
    if (s) {
        e->tex = SDL_CreateTextureFromSurface(ren, s);
        SDL_FreeSurface(s);
    }
    if (e->tex) {
        SDL_SetTextureBlendMode(e->tex, SDL_BLENDMODE_BLEND);
        e->state = IMG_READY;
        e->used = frame_no;
        ready_count++;
    } else {
        e->state = IMG_FAILED;
    }
}

static SDL_Texture *request(const char *key, bool thumb)
{
    if (!key || !key[0]) return NULL;
    ImgEntry *e = find_slot(key, true);
    if (!e) return NULL;
    e->used = frame_no;
    if (e->state == IMG_READY) return e->tex;
    if (e->state == IMG_EMPTY) {
        e->state = IMG_LOADING;
        Job *job;
        if (thumb) {
            job = job_new("", 0, parse_thumb, apply_image);
            if (job) copy_text(job->text, sizeof(job->text), key);
        } else {
            job = job_new(key, -1, parse_image, apply_image);
        }
        if (!job) {
            e->state = IMG_FAILED;
            return NULL;
        }
        job->low_priority = true;
        net_submit(job);
    }
    return NULL;
}

SDL_Texture *img_get(const char *url)
{
    return request(url, false);
}

SDL_Texture *img_thumb(const char *video_path)
{
    if (strlen(video_path) >= PATH_MAX) return NULL;
    return request(video_path, true);
}

void img_prefetch(const char *url)
{
    request(url, false);
}

void images_gc(void)
{
    if (ready_count <= IMG_KEEP) return;
    for (int i = 0; i < IMG_SLOTS && ready_count > IMG_KEEP; i++) {
        ImgEntry *e = &slots[i];
        if (e->state == IMG_READY && frame_no - e->used > 240) {
            SDL_DestroyTexture(e->tex);
            e->tex = NULL;
            e->state = IMG_EMPTY;
            ready_count--;
        }
    }
}
