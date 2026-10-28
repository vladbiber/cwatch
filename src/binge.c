#include "app.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static App *G;
static unsigned seen_serial;
static HistoryEntry cur;
static Episode next;
static bool next_ok;
static unsigned gen;

static void parse_full(Job *job)
{
    job->result = job->ok ? parse_meta_full(job->data) : NULL;
}

static void apply_next(Job *job)
{
    MetaFull *mf = job->result;
    if (job->gen != gen || !mf) {
        meta_full_free(mf);
        return;
    }
    int at = -1;
    for (int i = 0; i < mf->ep_count; i++)
        if (strcmp(mf->eps[i].id, cur.ep_id) == 0) at = i;
    for (int i = at + 1; at >= 0 && i < mf->ep_count; i++) {
        Episode *e = &mf->eps[i];
        if (e->season == 0 && cur.season != 0) continue;
        next = *e;
        next_ok = true;
        char label[TITLE_LEN];
        snprintf(label, sizeof(label), "S%d:E%d  %s", e->season, e->episode, e->title);
        stream_set_next(label);
        break;
    }
    meta_full_free(mf);
}

static void prepare(void)
{
    const HistoryEntry *h = stream_hist();
    next_ok = false;
    gen++;
    if (!h || !h->ep_id[0] || !h->meta_id[0]) return;
    cur = *h;
    char url[URL_LEN];
    meta_url(url, sizeof(url), "series", h->meta_id);
    Job *job = job_new(url, 6 * 3600, parse_full, apply_next);
    if (!job) return;
    job->gen = gen;
    net_submit(job);
}

static void apply_streams(Job *job)
{
    SourceList *l = job->ok ? parse_torrentio(job->data) : NULL;
    if (job->gen != gen || !G) {
        source_list_free(l);
        return;
    }
    HistoryEntry h = cur;
    copy_text(h.ep_id, sizeof(h.ep_id), next.id);
    copy_text(h.ep_title, sizeof(h.ep_title), next.title);
    h.season = next.season;
    h.episode = next.episode;
    char play[TITLE_LEN], heading[TITLE_LEN * 2], q[TITLE_LEN];
    snprintf(play, sizeof(play), "%s  S%d:E%d", h.name, h.season, h.episode);
    const Source *pick = NULL;
    for (int i = 0; l && i < l->count && !pick; i++)
        if (strcasecmp(l->items[i].hash, job->text) == 0) pick = &l->items[i];
    for (int i = 0; l && i < l->count && !pick; i++)
        if (l->items[i].seeds > 0) pick = &l->items[i];
    if (pick) {
        app_toast(G, "Next: %s", play);
        start_source(G, pick, false, h.name, play, h.episode, &h);
    } else {
        snprintf(heading, sizeof(heading), "%s  ·  S%d:E%d  %s", h.name, h.season, h.episode, h.ep_title);
        snprintf(q, sizeof(q), "%s %02d", h.name, h.episode);
        sources_open(G, "series", h.ep_id, heading, h.name, play, q, h.season, h.episode, &h);
    }
    source_list_free(l);
}

void binge_tick(App *a)
{
    G = a;
    if (stream_serial() != seen_serial) {
        seen_serial = stream_serial();
        prepare();
    }
    HistoryEntry h;
    char hash[64];
    if (!stream_take_next(&h, hash, sizeof(hash))) return;
    if (!next_ok || strcmp(h.ep_id, cur.ep_id) != 0) {
        app_toast(a, "No next episode found");
        return;
    }
    char url[URL_LEN];
    streams_url(url, sizeof(url), "series", next.id);
    Job *job = job_new(url, 1800, NULL, apply_streams);
    if (!job) return;
    job->gen = gen;
    copy_text(job->text, sizeof(job->text), hash);
    net_submit(job);
}
