#include "playback.h"
#include "mpvscript.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_HISTORY 60
#define MAX_REAP 32

typedef struct {
    pid_t helper;
    int out_fd;
    int in_fd;
    pid_t mpv;
    char buf[2048];
    size_t buf_len;
    char magnet[URL_LEN * 2];
    int file_idx;
    bool retried;
    HistoryEntry hist;
    bool has_hist;
    char next_label[TITLE_LEN];
    bool want_next;
} Session;

static StreamStatus status;
static Session sess = {.out_fd = -1, .in_fd = -1};
static Download downloads[MAX_DOWNLOADS];
static int download_n;
static bool dl_changed;
static HistoryEntry history[MAX_HISTORY];
static int history_n;
static pid_t reap[MAX_REAP];
static unsigned serial;

static void reap_later(pid_t pid)
{
    if (pid <= 0) return;
    for (int i = 0; i < MAX_REAP; i++)
        if (!reap[i]) {
            reap[i] = pid;
            return;
        }
    waitpid(pid, NULL, 0);
}

static void helper_path(char *out, size_t len)
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            snprintf(out, len, "%s/cwatch-stream", exe);
            if (access(out, X_OK) == 0) return;
        }
    }
    copy_text(out, len, "cwatch-stream");
}

static pid_t spawn(char *const argv[], int *out_fd, int *in_fd)
{
    int out[2], in[2];
    if (pipe(out) != 0) return -1;
    if (pipe(in) != 0) {
        close(out[0]);
        close(out[1]);
        return -1;
    }
    char log[PATH_MAX];
    cache_dir(log, sizeof(log), "");
    strncat(log, "/stream.log", sizeof(log) - strlen(log) - 1);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(out[1], STDOUT_FILENO);
        dup2(in[0], STDIN_FILENO);
        int lfd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (lfd >= 0) dup2(lfd, STDERR_FILENO);
        for (int fd = 3; fd < 256; fd++) close(fd);
        execvp(argv[0], argv);
        printf("ERROR cannot run %s\n", argv[0]);
        _exit(127);
    }
    close(out[1]);
    close(in[0]);
    if (pid < 0) {
        close(out[0]);
        close(in[1]);
        return -1;
    }
    fcntl(out[0], F_SETFL, O_NONBLOCK);
    fcntl(in[1], F_SETFD, FD_CLOEXEC);
    fcntl(out[0], F_SETFD, FD_CLOEXEC);
    *out_fd = out[0];
    *in_fd = in[1];
    return pid;
}

static bool read_line(int fd, char *buf, size_t cap, size_t *len, char *line, size_t line_len, bool *eof)
{
    for (;;) {
        char *nl = memchr(buf, '\n', *len);
        if (nl) {
            size_t n = (size_t)(nl - buf);
            copy_text(line, line_len, "");
            if (n < line_len) {
                memcpy(line, buf, n);
                line[n] = '\0';
            }
            memmove(buf, nl + 1, *len - n - 1);
            *len -= n + 1;
            return true;
        }
        if (*len >= cap - 1) *len = 0;
        ssize_t r = read(fd, buf + *len, cap - 1 - *len);
        if (r > 0) {
            *len += (size_t)r;
            continue;
        }
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) *eof = true;
        return false;
    }
}

char *helper_capture(const char *mode, const char *source, int timeout_s)
{
    char helper[PATH_MAX];
    helper_path(helper, sizeof(helper));
    char *argv[] = {helper, (char *)mode, (char *)source, NULL};
    int out_fd, in_fd;
    pid_t pid = spawn(argv, &out_fd, &in_fd);
    if (pid <= 0) return NULL;
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    Uint32 deadline = SDL_GetTicks() + (Uint32)timeout_s * 1000;
    int status = 0;
    bool exited = false;
    while (buf && SDL_GetTicks() < deadline) {
        if (len + 4096 > cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) break;
            buf = nb;
            cap *= 2;
        }
        ssize_t r = read(out_fd, buf + len, cap - len - 1);
        if (r > 0) {
            len += (size_t)r;
            continue;
        }
        if (r == 0) break;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            exited = true;
            while ((r = read(out_fd, buf + len, cap - len - 1)) > 0) len += (size_t)r;
            break;
        }
        SDL_Delay(50);
    }
    close(out_fd);
    close(in_fd);
    if (!exited) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
    }
    if (buf) buf[len] = '\0';
    return buf;
}

int discover_players(Player *players)
{
    const char *names[] = {"mpv", "vlc", "celluloid", "parole", "xdg-open"};
    int count = 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && count < MAX_PLAYERS; i++) {
        if (!command_exists(names[i])) continue;
        copy_text(players[count].name, sizeof(players[count].name), names[i]);
        copy_text(players[count].exec, sizeof(players[count].exec), names[i]);
        count++;
    }
    if (count == 0) {
        copy_text(players[0].name, sizeof(players[0].name), "xdg-open");
        copy_text(players[0].exec, sizeof(players[0].exec), "xdg-open");
        count = 1;
    }
    return count;
}

typedef struct {
    const char *mode;
    const char *next;
    const char *track_key;
    const char *ep_key;
} MpvCtx;

static char script_path[PATH_MAX];

static void write_script(void)
{
    state_file(script_path, sizeof(script_path), "cwatch.lua");
    FILE *fp = fopen(script_path, "w");
    if (!fp) {
        script_path[0] = '\0';
        return;
    }
    fputs(MPV_SCRIPT, fp);
    fclose(fp);
}

void track_key_for(const char *show, char *out, size_t len)
{
    char lower[TITLE_LEN];
    size_t i = 0;
    for (; show && show[i] && i + 1 < sizeof(lower); i++) lower[i] = (char)tolower((unsigned char)show[i]);
    lower[i] = '\0';
    snprintf(out, len, "%016lx", hash_string(lower));
}

static pid_t launch_mpv(const char *target, const char *title, const MpvCtx *ctx, bool stream)
{
    char wl[PATH_MAX], wl_arg[PATH_MAX + 40], title_arg[TITLE_LEN + 32], script_arg[PATH_MAX + 16], state[PATH_MAX];
    watch_later_dir(wl, sizeof(wl));
    state_dir(state, sizeof(state));
    snprintf(wl_arg, sizeof(wl_arg), "--watch-later-directory=%s", wl);
    snprintf(title_arg, sizeof(title_arg), "--force-media-title=%s", title ? title : "");
    if (!title) copy_text(title_arg, sizeof(title_arg), "--title=${media-title}");
    snprintf(script_arg, sizeof(script_arg), "--script=%s", script_path);
    if (!script_path[0]) copy_text(script_arg, sizeof(script_arg), "--idle=no");
    bool dry = getenv("CWATCH_DRYRUN") != NULL;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        setenv("CWATCH_STATE", dry ? "" : state, 1);
        setenv("CWATCH_MODE", ctx && ctx->mode ? ctx->mode : "local", 1);
        setenv("CWATCH_NEXT", ctx && ctx->next ? ctx->next : "", 1);
        setenv("CWATCH_TRACKKEY", ctx && ctx->track_key ? ctx->track_key : "", 1);
        setenv("CWATCH_EPKEY", ctx && ctx->ep_key ? ctx->ep_key : "", 1);
        const char *keep = stream ? "--keep-open=yes" : "--keep-open=no";
        if (dry)
            execlp("mpv", "mpv", "--vo=null", "--ao=null", "--really-quiet", "--network-timeout=300",
                   getenv("CWATCH_DRYRUN_FULL") ? "--speed=100" : "--frames=120", "--playlist-start=0",
                   "--loop-playlist=no", "--reset-on-next-file=all", "--prefetch-playlist=no",
                   keep, script_arg, title_arg, target, (char *)NULL);
        else
            execlp("mpv", "mpv", "--save-position-on-quit=yes", "--resume-playback=yes",
                   "--write-filename-in-watch-later-config=yes", "--watch-later-options=start",
                   "--network-timeout=300", "--force-window=immediate", "--slang=eng,en", keep,
                   wl_arg, script_arg, title_arg, target, (char *)NULL);
        _exit(127);
    }
    return pid;
}

void play_file(const char *path, const Player *player, const char *title)
{
    if (strcmp(player->exec, "mpv") == 0 || getenv("CWATCH_DRYRUN")) {
        reap_later(launch_mpv(path, title, NULL, false));
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execlp(player->exec, player->exec, path, (char *)NULL);
        _exit(127);
    }
    reap_later(pid);
}

void play_playlist(char paths[][PATH_MAX], char titles[][TITLE_LEN], int n, const Player *player, const char *show)
{
    if (n <= 0) return;
    if (n == 1 || (strcmp(player->exec, "mpv") != 0 && !getenv("CWATCH_DRYRUN"))) {
        if (strcmp(player->exec, "mpv") != 0) {
            play_file(paths[0], player, titles[0]);
            return;
        }
    }
    char m3u[PATH_MAX];
    state_file(m3u, sizeof(m3u), "playlist.m3u");
    FILE *fp = fopen(m3u, "w");
    if (!fp) {
        play_file(paths[0], player, titles[0]);
        return;
    }
    fprintf(fp, "#EXTM3U\n");
    for (int i = 0; i < n; i++) fprintf(fp, "#EXTINF:-1,%s\n%s\n", titles[i], paths[i]);
    fclose(fp);
    char key[32];
    track_key_for(show, key, sizeof(key));
    MpvCtx ctx = {"local", "", key, ""};
    reap_later(launch_mpv(m3u, NULL, &ctx, false));
}

static void history_save(void)
{
    char path[PATH_MAX];
    state_file(path, sizeof(path), "history.json");
    json_object *arr = json_object_new_array();
    for (int i = 0; i < history_n; i++) {
        HistoryEntry *h = &history[i];
        json_object *o = json_object_new_object();
        json_object_object_add(o, "meta_id", json_object_new_string(h->meta_id));
        json_object_object_add(o, "type", json_object_new_string(h->type));
        json_object_object_add(o, "name", json_object_new_string(h->name));
        json_object_object_add(o, "poster", json_object_new_string(h->poster));
        json_object_object_add(o, "background", json_object_new_string(h->background));
        json_object_object_add(o, "ep_id", json_object_new_string(h->ep_id));
        json_object_object_add(o, "season", json_object_new_int(h->season));
        json_object_object_add(o, "episode", json_object_new_int(h->episode));
        json_object_object_add(o, "ep_title", json_object_new_string(h->ep_title));
        json_object_object_add(o, "url", json_object_new_string(h->url));
        json_object_object_add(o, "when", json_object_new_int64(h->when));
        json_object_array_add(arr, o);
    }
    json_object_to_file_ext(path, arr, JSON_C_TO_STRING_PRETTY);
    json_object_put(arr);
}

static void history_load(void)
{
    char path[PATH_MAX];
    state_file(path, sizeof(path), "history.json");
    if (access(path, R_OK) != 0) return;
    json_object *arr = json_object_from_file(path);
    if (!arr || !json_object_is_type(arr, json_type_array)) {
        if (arr) json_object_put(arr);
        return;
    }
    size_t n = json_object_array_length(arr);
    for (size_t i = 0; i < n && history_n < MAX_HISTORY; i++) {
        json_object *o = json_object_array_get_idx(arr, i), *v;
        HistoryEntry *h = &history[history_n++];
        memset(h, 0, sizeof(*h));
#define S(f) if (json_object_object_get_ex(o, #f, &v)) copy_text(h->f, sizeof(h->f), json_object_get_string(v))
        S(meta_id); S(type); S(name); S(poster); S(background); S(ep_id); S(ep_title); S(url);
#undef S
        if (json_object_object_get_ex(o, "season", &v)) h->season = json_object_get_int(v);
        if (json_object_object_get_ex(o, "episode", &v)) h->episode = json_object_get_int(v);
        if (json_object_object_get_ex(o, "when", &v)) h->when = (long)json_object_get_int64(v);
    }
    json_object_put(arr);
}

static void history_push(const HistoryEntry *e)
{
    int found = -1;
    for (int i = 0; i < history_n; i++)
        if (strcmp(history[i].meta_id, e->meta_id) == 0) found = i;
    if (found < 0) {
        if (history_n == MAX_HISTORY) history_n--;
        found = history_n++;
    }
    for (int i = found; i > 0; i--) history[i] = history[i - 1];
    history[0] = *e;
    history[0].when = (long)time(NULL);
    history_save();
}

int history_count(void) { return history_n; }
const HistoryEntry *history_get(int idx) { return idx >= 0 && idx < history_n ? &history[idx] : NULL; }

void history_remove(int idx)
{
    if (idx < 0 || idx >= history_n) return;
    for (int i = idx; i < history_n - 1; i++) history[i] = history[i + 1];
    history_n--;
    history_save();
}

static void session_close_helper(void)
{
    if (sess.in_fd >= 0) close(sess.in_fd);
    if (sess.out_fd >= 0) close(sess.out_fd);
    sess.in_fd = sess.out_fd = -1;
    if (sess.helper > 0) {
        kill(sess.helper, SIGTERM);
        reap_later(sess.helper);
    }
    sess.helper = 0;
    sess.buf_len = 0;
}

static bool start_helper(int port)
{
    char helper[PATH_MAX], file_arg[16], port_arg[16];
    helper_path(helper, sizeof(helper));
    snprintf(file_arg, sizeof(file_arg), "%d", sess.file_idx);
    snprintf(port_arg, sizeof(port_arg), "%d", port);
    char *argv[10];
    int a = 0;
    argv[a++] = helper;
    argv[a++] = "stream";
    argv[a++] = sess.magnet;
    if (sess.file_idx >= 0) {
        argv[a++] = "--file";
        argv[a++] = file_arg;
    }
    argv[a++] = "--port";
    argv[a++] = port_arg;
    argv[a] = NULL;
    sess.helper = spawn(argv, &sess.out_fd, &sess.in_fd);
    return sess.helper > 0;
}

bool play_torrent(const char *magnet, int file_idx, const char *title, const HistoryEntry *hist)
{
    stream_cancel();
    memset(&status, 0, sizeof(status));
    copy_text(sess.magnet, sizeof(sess.magnet), magnet);
    sess.file_idx = file_idx;
    sess.retried = false;
    sess.has_hist = hist != NULL;
    if (hist) sess.hist = *hist;
    sess.next_label[0] = '\0';
    serial++;
    copy_text(status.title, sizeof(status.title), title);
    status.started = SDL_GetTicks();
    char key[URL_LEN * 2 + 16];
    snprintf(key, sizeof(key), "%s#%d", magnet, file_idx);
    int port = 20000 + (int)(hash_string(key) % 9000);
    if (!start_helper(port)) {
        copy_text(status.error, sizeof(status.error), "cwatch-stream not found; run make");
        return false;
    }
    status.state = ST_CONNECTING;
    return true;
}

void stream_cancel(void)
{
    if (sess.mpv > 0) {
        kill(-sess.mpv, SIGTERM);
        reap_later(sess.mpv);
        sess.mpv = 0;
    }
    session_close_helper();
    status.state = ST_IDLE;
}

const StreamStatus *stream_status(void)
{
    return &status;
}

unsigned stream_serial(void)
{
    return serial;
}

const HistoryEntry *stream_hist(void)
{
    return sess.has_hist ? &sess.hist : NULL;
}

void stream_set_next(const char *label)
{
    copy_text(sess.next_label, sizeof(sess.next_label), label);
}

bool stream_take_next(HistoryEntry *out, char *hash, size_t hash_len)
{
    if (!sess.want_next) return false;
    sess.want_next = false;
    if (!sess.has_hist) return false;
    *out = sess.hist;
    const char *b = strstr(sess.magnet, "btih:");
    copy_text(hash, hash_len, b ? b + 5 : "");
    char *amp = strchr(hash, '&');
    if (amp) *amp = '\0';
    return true;
}

static void handle_stream_line(const char *line)
{
    if (getenv("CWATCH_DEBUG")) fprintf(stderr, "stream: %s\n", line);
    if (strncmp(line, "URL ", 4) == 0 && sess.mpv <= 0) {
        copy_text(status.url, sizeof(status.url), line + 4);
        char key[32];
        track_key_for(sess.has_hist ? sess.hist.name : status.title, key, sizeof(key));
        MpvCtx ctx = {"stream", sess.next_label, key, sess.has_hist ? (sess.hist.ep_id[0] ? sess.hist.ep_id : sess.hist.meta_id) : ""};
        sess.mpv = launch_mpv(status.url, status.title, &ctx, true);
        status.state = ST_PLAYING;
        if (sess.has_hist) {
            copy_text(sess.hist.url, sizeof(sess.hist.url), status.url);
            history_push(&sess.hist);
        }
    } else if (strncmp(line, "STAT ", 5) == 0) {
        sscanf(line + 5, "%lf %lf %d %d", &status.progress, &status.rate, &status.peers, &status.seeds);
    } else if (strcmp(line, "DONE") == 0) {
        status.done = true;
        status.progress = 1.0;
    } else if (strncmp(line, "ERROR ", 6) == 0) {
        copy_text(status.error, sizeof(status.error), line + 6);
    }
}

static void poll_stream(void)
{
    if (status.state == ST_IDLE) return;
    char line[2048];
    bool eof = false;
    while (sess.out_fd >= 0 &&
           read_line(sess.out_fd, sess.buf, sizeof(sess.buf), &sess.buf_len, line, sizeof(line), &eof))
        handle_stream_line(line);

    if (eof && sess.mpv <= 0) {
        session_close_helper();
        if (!sess.retried && strstr(status.error, "bind")) {
            sess.retried = true;
            status.error[0] = '\0';
            start_helper(0);
            return;
        }
        if (!status.error[0]) copy_text(status.error, sizeof(status.error), "stream helper stopped");
        status.state = ST_IDLE;
        return;
    }
    if (sess.mpv > 0) {
        int st;
        pid_t r = waitpid(sess.mpv, &st, WNOHANG);
        if (r == sess.mpv || (r < 0 && errno == ECHILD)) {
            if (getenv("CWATCH_DEBUG")) fprintf(stderr, "mpv exited status=%d\n", r == sess.mpv ? st : -1);
            if (r == sess.mpv && WIFEXITED(st) && WEXITSTATUS(st) == 42) sess.want_next = true;
            sess.mpv = 0;
            session_close_helper();
            status.state = ST_IDLE;
        }
    }
}

static void downloads_save(void)
{
    char path[PATH_MAX];
    state_file(path, sizeof(path), "downloads.json");
    json_object *arr = json_object_new_array();
    for (int i = 0; i < download_n; i++) {
        Download *d = &downloads[i];
        json_object *o = json_object_new_object();
        json_object_object_add(o, "title", json_object_new_string(d->title));
        json_object_object_add(o, "magnet", json_object_new_string(d->magnet));
        json_object_object_add(o, "dest", json_object_new_string(d->dest));
        json_object_object_add(o, "file", json_object_new_int(d->file_idx));
        json_object_object_add(o, "state", json_object_new_int(d->state));
        json_object_object_add(o, "progress", json_object_new_double(d->progress));
        json_object_array_add(arr, o);
    }
    json_object_to_file_ext(path, arr, JSON_C_TO_STRING_PRETTY);
    json_object_put(arr);
}

static bool download_spawn(Download *d)
{
    char helper[PATH_MAX], file_arg[16];
    helper_path(helper, sizeof(helper));
    snprintf(file_arg, sizeof(file_arg), "%d", d->file_idx);
    char *argv[10];
    int a = 0;
    argv[a++] = helper;
    argv[a++] = "download";
    argv[a++] = d->magnet;
    argv[a++] = "--dest";
    argv[a++] = d->dest;
    if (d->file_idx >= 0) {
        argv[a++] = "--file";
        argv[a++] = file_arg;
    }
    argv[a] = NULL;
    mkdir_p(d->dest);
    d->pid = spawn(argv, &d->out_fd, &d->in_fd);
    d->buf_len = 0;
    d->state = d->pid > 0 ? DL_ACTIVE : DL_FAILED;
    if (d->pid <= 0) copy_text(d->error, sizeof(d->error), "cwatch-stream not found");
    return d->pid > 0;
}

bool download_start(const char *magnet, int file_idx, const char *dest, const char *title)
{
    for (int i = 0; i < download_n; i++) {
        Download *d = &downloads[i];
        if (strcmp(d->magnet, magnet) == 0 && d->file_idx == file_idx && d->state == DL_ACTIVE)
            return true;
    }
    if (download_n >= MAX_DOWNLOADS) {
        int drop = -1;
        for (int i = 0; i < download_n && drop < 0; i++)
            if (downloads[i].state != DL_ACTIVE) drop = i;
        if (drop < 0) return false;
        download_remove(drop);
    }
    Download *d = &downloads[download_n++];
    memset(d, 0, sizeof(*d));
    d->out_fd = d->in_fd = -1;
    copy_text(d->title, sizeof(d->title), title);
    copy_text(d->magnet, sizeof(d->magnet), magnet);
    copy_text(d->dest, sizeof(d->dest), dest);
    d->file_idx = file_idx;
    bool ok = download_spawn(d);
    downloads_save();
    return ok;
}

static void download_stop_proc(Download *d)
{
    if (d->out_fd >= 0) close(d->out_fd);
    if (d->in_fd >= 0) close(d->in_fd);
    d->out_fd = d->in_fd = -1;
    if (d->pid > 0) {
        kill(d->pid, SIGTERM);
        reap_later(d->pid);
    }
    d->pid = 0;
}

void download_cancel(int idx)
{
    if (idx < 0 || idx >= download_n) return;
    Download *d = &downloads[idx];
    if (d->state == DL_ACTIVE) {
        download_stop_proc(d);
        d->state = DL_STOPPED;
    } else if (d->state != DL_DONE) {
        download_spawn(d);
    }
    downloads_save();
}

void download_remove(int idx)
{
    if (idx < 0 || idx >= download_n) return;
    download_stop_proc(&downloads[idx]);
    for (int i = idx; i < download_n - 1; i++) downloads[i] = downloads[i + 1];
    download_n--;
    downloads_save();
}

int download_count(void) { return download_n; }
const Download *download_get(int idx) { return idx >= 0 && idx < download_n ? &downloads[idx] : NULL; }

int downloads_active(void)
{
    int n = 0;
    for (int i = 0; i < download_n; i++)
        if (downloads[i].state == DL_ACTIVE) n++;
    return n;
}

bool downloads_changed(void)
{
    bool c = dl_changed;
    dl_changed = false;
    return c;
}

static void poll_downloads(void)
{
    for (int i = 0; i < download_n; i++) {
        Download *d = &downloads[i];
        if (d->state != DL_ACTIVE || d->out_fd < 0) continue;
        char line[1024];
        bool eof = false;
        while (read_line(d->out_fd, d->buf, sizeof(d->buf), &d->buf_len, line, sizeof(line), &eof)) {
            if (strncmp(line, "STAT ", 5) == 0) {
                int seeds = 0;
                sscanf(line + 5, "%lf %lf %d %d", &d->progress, &d->rate, &d->peers, &seeds);
            } else if (strcmp(line, "DONE") == 0) {
                d->state = DL_DONE;
                d->progress = 1.0;
                dl_changed = true;
            } else if (strncmp(line, "ERROR ", 6) == 0) {
                copy_text(d->error, sizeof(d->error), line + 6);
            }
        }
        if (eof || d->state == DL_DONE) {
            download_stop_proc(d);
            if (d->state == DL_ACTIVE) d->state = DL_FAILED;
            downloads_save();
        }
    }
}

void playback_init(void)
{
    signal(SIGPIPE, SIG_IGN);
    write_script();
    history_load();
    char path[PATH_MAX];
    state_file(path, sizeof(path), "downloads.json");
    if (access(path, R_OK) != 0) return;
    json_object *arr = json_object_from_file(path);
    if (!arr || !json_object_is_type(arr, json_type_array)) {
        if (arr) json_object_put(arr);
        return;
    }
    size_t n = json_object_array_length(arr);
    for (size_t i = 0; i < n && download_n < MAX_DOWNLOADS; i++) {
        json_object *o = json_object_array_get_idx(arr, i), *v;
        Download *d = &downloads[download_n++];
        memset(d, 0, sizeof(*d));
        d->out_fd = d->in_fd = -1;
#define S(f, k) if (json_object_object_get_ex(o, k, &v)) copy_text(d->f, sizeof(d->f), json_object_get_string(v))
        S(title, "title"); S(magnet, "magnet"); S(dest, "dest");
#undef S
        d->file_idx = json_object_object_get_ex(o, "file", &v) ? json_object_get_int(v) : -1;
        d->state = json_object_object_get_ex(o, "state", &v) ? json_object_get_int(v) : DL_STOPPED;
        d->progress = json_object_object_get_ex(o, "progress", &v) ? json_object_get_double(v) : 0;
        if (d->state == DL_ACTIVE) download_spawn(d);
    }
    json_object_put(arr);
}

void playback_shutdown(void)
{
    if (sess.helper > 0 || sess.in_fd >= 0) session_close_helper();
    for (int i = 0; i < download_n; i++) {
        bool active = downloads[i].state == DL_ACTIVE;
        download_stop_proc(&downloads[i]);
        if (active) downloads[i].state = DL_ACTIVE;
    }
    downloads_save();
    for (int i = 0; i < MAX_REAP; i++)
        if (reap[i] > 0 && waitpid(reap[i], NULL, WNOHANG) == 0) {
            kill(reap[i], 0);
        }
}

void playback_poll(void)
{
    poll_stream();
    poll_downloads();
    for (int i = 0; i < MAX_REAP; i++) {
        if (reap[i] <= 0) continue;
        pid_t r = waitpid(reap[i], NULL, WNOHANG);
        if (r == reap[i] || (r < 0 && errno == ECHILD)) reap[i] = 0;
    }
}
