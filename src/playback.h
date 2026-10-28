#ifndef CWATCH_PLAYBACK_H
#define CWATCH_PLAYBACK_H

#include "common.h"

#define MAX_DOWNLOADS 64

enum { DL_ACTIVE, DL_DONE, DL_FAILED, DL_STOPPED };
enum { ST_IDLE, ST_CONNECTING, ST_PLAYING };

typedef struct {
    char title[TITLE_LEN];
    char magnet[URL_LEN * 2];
    char dest[PATH_MAX];
    int file_idx;
    int state;
    pid_t pid;
    int out_fd;
    int in_fd;
    char buf[1024];
    size_t buf_len;
    double progress;
    double rate;
    int peers;
    char error[TITLE_LEN];
} Download;

typedef struct {
    int state;
    char title[TITLE_LEN];
    char url[URL_LEN];
    char error[TITLE_LEN];
    double progress;
    double rate;
    int peers;
    int seeds;
    Uint32 started;
    bool done;
} StreamStatus;

typedef struct {
    char meta_id[64];
    char type[16];
    char name[TITLE_LEN];
    char poster[URL_LEN];
    char background[URL_LEN];
    char ep_id[96];
    int season;
    int episode;
    char ep_title[TITLE_LEN];
    char url[URL_LEN];
    long when;
} HistoryEntry;

void playback_init(void);
void playback_shutdown(void);
void playback_poll(void);
int discover_players(Player *players);
char *helper_capture(const char *mode, const char *source, int timeout_s);

void play_file(const char *path, const Player *player, const char *title);
void play_playlist(char paths[][PATH_MAX], char titles[][TITLE_LEN], int n, const Player *player, const char *show);
void track_key_for(const char *show, char *out, size_t len);
void stream_set_next(const char *label);
unsigned stream_serial(void);
const HistoryEntry *stream_hist(void);
bool stream_take_next(HistoryEntry *out, char *hash, size_t hash_len);
bool play_torrent(const char *magnet, int file_idx, const char *title, const HistoryEntry *hist);
void stream_cancel(void);
const StreamStatus *stream_status(void);

bool download_start(const char *magnet, int file_idx, const char *dest, const char *title);
void download_cancel(int idx);
void download_remove(int idx);
int download_count(void);
const Download *download_get(int idx);
int downloads_active(void);
bool downloads_changed(void);

int history_count(void);
const HistoryEntry *history_get(int idx);
void history_remove(int idx);

#endif
