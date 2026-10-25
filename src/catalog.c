#include "catalog.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void catalog_url(char *out, size_t len, const char *base, const char *type,
                 const char *catalog, const char *genre, const char *search)
{
    char extra[300] = "";
    char enc[256];
    if (search && search[0]) {
        url_encode(search, enc, sizeof(enc));
        snprintf(extra, sizeof(extra), "/search=%s", enc);
    } else if (genre && genre[0]) {
        url_encode(genre, enc, sizeof(enc));
        snprintf(extra, sizeof(extra), "/genre=%s", enc);
    }
    snprintf(out, len, "%s/catalog/%s/%s%s.json", base, type, catalog, extra);
}

void meta_url(char *out, size_t len, const char *type, const char *id)
{
    bool kitsu = strncmp(id, "kitsu:", 6) == 0;
    const char *t = strcmp(type, "movie") == 0 ? "movie" : "series";
    if (kitsu && strcmp(type, "anime") == 0) t = "series";
    snprintf(out, len, "%s/meta/%s/%s.json", kitsu ? KITSU : CINEMETA, t, id);
}

void streams_url(char *out, size_t len, const char *type, const char *id)
{
    snprintf(out, len, "%s/stream/%s/%s.json", TORRENTIO, type, id);
}

void nyaa_url(char *out, size_t len, const char *query)
{
    char enc[512];
    url_encode(query, enc, sizeof(enc));
    snprintf(out, len, "https://nyaa.si/?page=rss&q=%s&c=1_0&f=0", enc);
}

bool meta_is_series(const Meta *m)
{
    return strcmp(m->type, "movie") != 0;
}

static const char *jstr(json_object *o, const char *key)
{
    json_object *v;
    if (!o || !json_object_object_get_ex(o, key, &v)) return "";
    if (json_object_is_type(v, json_type_string)) return json_object_get_string(v);
    if (json_object_is_type(v, json_type_int) || json_object_is_type(v, json_type_double))
        return json_object_get_string(v);
    return "";
}

static int jint(json_object *o, const char *key)
{
    json_object *v;
    if (!o || !json_object_object_get_ex(o, key, &v)) return 0;
    return json_object_get_int(v);
}

static void join_genres(json_object *o, char *out, size_t len)
{
    json_object *arr;
    out[0] = '\0';
    if (!json_object_object_get_ex(o, "genres", &arr) &&
        !json_object_object_get_ex(o, "genre", &arr))
        return;
    if (!json_object_is_type(arr, json_type_array)) return;
    size_t n = json_object_array_length(arr), used = 0;
    for (size_t i = 0; i < n && i < 3; i++) {
        const char *g = json_object_get_string(json_object_array_get_idx(arr, i));
        int w = snprintf(out + used, len - used, "%s%s", used ? "  ·  " : "", g ? g : "");
        if (w < 0 || (size_t)w >= len - used) break;
        used += (size_t)w;
    }
}

static void fill_meta(json_object *o, Meta *m)
{
    memset(m, 0, sizeof(*m));
    m->local_col = -1;
    m->local_item = -1;
    m->hist = -1;
    m->progress = -1;
    copy_text(m->id, sizeof(m->id), jstr(o, "id"));
    if (!m->id[0]) copy_text(m->id, sizeof(m->id), jstr(o, "imdb_id"));
    copy_text(m->type, sizeof(m->type), jstr(o, "type"));
    copy_text(m->name, sizeof(m->name), jstr(o, "name"));
    copy_text(m->poster, sizeof(m->poster), jstr(o, "poster"));
    copy_text(m->background, sizeof(m->background), jstr(o, "background"));
    copy_text(m->logo, sizeof(m->logo), jstr(o, "logo"));
    copy_text(m->year, sizeof(m->year), jstr(o, "releaseInfo"));
    if (!m->year[0]) copy_text(m->year, sizeof(m->year), jstr(o, "year"));
    copy_text(m->rating, sizeof(m->rating), jstr(o, "imdbRating"));
    copy_text(m->runtime, sizeof(m->runtime), jstr(o, "runtime"));
    copy_text(m->desc, sizeof(m->desc), jstr(o, "description"));
    join_genres(o, m->genres, sizeof(m->genres));
    if (strcmp(m->type, "anime") == 0) copy_text(m->type, sizeof(m->type), "series");
    if (!m->background[0] && strncmp(m->id, "tt", 2) == 0)
        snprintf(m->background, sizeof(m->background),
                 "https://images.metahub.space/background/medium/%s/img", m->id);
}

MetaList *parse_meta_list(const char *json)
{
    if (!json) return NULL;
    json_object *root = json_tokener_parse(json);
    if (!root) return NULL;
    json_object *arr;
    MetaList *l = calloc(1, sizeof(*l));
    if (l && json_object_object_get_ex(root, "metas", &arr) &&
        json_object_is_type(arr, json_type_array)) {
        size_t n = json_object_array_length(arr);
        l->items = calloc(n ? n : 1, sizeof(Meta));
        for (size_t i = 0; l->items && i < n; i++) {
            Meta *m = &l->items[l->count];
            fill_meta(json_object_array_get_idx(arr, i), m);
            if (m->id[0] && m->name[0]) l->count++;
        }
    }
    json_object_put(root);
    return l;
}

static int cmp_episode(const void *a, const void *b)
{
    const Episode *x = a, *y = b;
    int sa = x->season == 0 ? 9999 : x->season;
    int sb = y->season == 0 ? 9999 : y->season;
    if (sa != sb) return sa - sb;
    return x->episode - y->episode;
}

MetaFull *parse_meta_full(const char *json)
{
    if (!json) return NULL;
    json_object *root = json_tokener_parse(json);
    if (!root) return NULL;
    json_object *meta, *vids;
    MetaFull *mf = NULL;
    if (json_object_object_get_ex(root, "meta", &meta) && (mf = calloc(1, sizeof(*mf)))) {
        fill_meta(meta, &mf->meta);
        if (json_object_object_get_ex(meta, "videos", &vids) &&
            json_object_is_type(vids, json_type_array)) {
            size_t n = json_object_array_length(vids);
            mf->eps = calloc(n ? n : 1, sizeof(Episode));
            for (size_t i = 0; mf->eps && i < n; i++) {
                json_object *v = json_object_array_get_idx(vids, i);
                Episode *e = &mf->eps[mf->ep_count];
                copy_text(e->id, sizeof(e->id), jstr(v, "id"));
                e->season = jint(v, "season");
                e->episode = jint(v, "episode");
                if (!e->episode) e->episode = jint(v, "number");
                copy_text(e->title, sizeof(e->title), jstr(v, "title"));
                if (!e->title[0]) copy_text(e->title, sizeof(e->title), jstr(v, "name"));
                copy_text(e->thumb, sizeof(e->thumb), jstr(v, "thumbnail"));
                copy_text(e->overview, sizeof(e->overview), jstr(v, "overview"));
                if (!e->overview[0]) copy_text(e->overview, sizeof(e->overview), jstr(v, "description"));
                copy_text(e->released, sizeof(e->released), jstr(v, "released"));
                e->local_item = -1;
                if (e->id[0] && e->episode > 0) mf->ep_count++;
            }
            qsort(mf->eps, (size_t)mf->ep_count, sizeof(Episode), cmp_episode);
        }
        if (mf->ep_count > 0) copy_text(mf->meta.type, sizeof(mf->meta.type), "series");
        else if (!mf->meta.type[0] || strcmp(mf->meta.type, "series") == 0)
            copy_text(mf->meta.type, sizeof(mf->meta.type), "movie");
    }
    json_object_put(root);
    return mf;
}

static const char *after_marker(const char *s, const char *marker)
{
    const char *p = strstr(s, marker);
    if (!p) return NULL;
    p += strlen(marker);
    while (*p == ' ') p++;
    return p;
}

static void copy_until(char *out, size_t len, const char *src, const char *stops)
{
    size_t i = 0;
    while (src && src[i] && !strchr(stops, src[i]) && i + 1 < len) {
        out[i] = src[i];
        i++;
    }
    out[i] = '\0';
    trim_line(out);
}

SourceList *parse_torrentio(const char *json)
{
    if (!json) return NULL;
    json_object *root = json_tokener_parse(json);
    if (!root) return NULL;
    json_object *arr;
    SourceList *l = calloc(1, sizeof(*l));
    if (l && json_object_object_get_ex(root, "streams", &arr) &&
        json_object_is_type(arr, json_type_array)) {
        size_t n = json_object_array_length(arr);
        l->items = calloc(n ? n : 1, sizeof(Source));
        for (size_t i = 0; l->items && i < n; i++) {
            json_object *o = json_object_array_get_idx(arr, i);
            Source *s = &l->items[l->count];
            memset(s, 0, sizeof(*s));
            copy_text(s->hash, sizeof(s->hash), jstr(o, "infoHash"));
            if (!s->hash[0]) continue;
            json_object *fi, *bh;
            s->file_idx = json_object_object_get_ex(o, "fileIdx", &fi) ? json_object_get_int(fi) : -1;
            const char *name = jstr(o, "name");
            const char *nl = strchr(name, '\n');
            copy_until(s->quality, sizeof(s->quality), nl ? nl + 1 : name, "\n");
            const char *title = jstr(o, "title");
            copy_until(s->title, sizeof(s->title), title, "\n");
            if (json_object_object_get_ex(o, "behaviorHints", &bh))
                copy_text(s->file, sizeof(s->file), jstr(bh, "filename"));
            const char *p;
            if ((p = after_marker(title, "\xF0\x9F\x91\xA4"))) s->seeds = atoi(p);
            if ((p = after_marker(title, "\xF0\x9F\x92\xBE"))) copy_until(s->size, sizeof(s->size), p, "\n\xE2\xF0");
            if ((p = after_marker(title, "\xE2\x9A\x99\xEF\xB8\x8F"))) copy_until(s->provider, sizeof(s->provider), p, "\n");
            l->count++;
        }
    }
    json_object_put(root);
    return l;
}

static bool xml_tag(const char *item, const char *end, const char *tag, char *out, size_t len)
{
    char open[64], close[64];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    const char *a = strstr(item, open);
    if (!a || a > end) return false;
    a += strlen(open);
    const char *b = strstr(a, close);
    if (!b || b > end) return false;
    size_t n = (size_t)(b - a);
    if (n >= len) n = len - 1;
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < len; i++) {
        if (a[i] == '&') {
            const char *ent = a + i;
            char c = 0;
            int skip = 0;
            if (!strncmp(ent, "&amp;", 5)) { c = '&'; skip = 4; }
            else if (!strncmp(ent, "&lt;", 4)) { c = '<'; skip = 3; }
            else if (!strncmp(ent, "&gt;", 4)) { c = '>'; skip = 3; }
            else if (!strncmp(ent, "&#34;", 5) || !strncmp(ent, "&quot;", 6)) { c = '"'; skip = ent[2] == '3' ? 4 : 5; }
            else if (!strncmp(ent, "&#39;", 5)) { c = '\''; skip = 4; }
            if (c) {
                out[o++] = c;
                i += (size_t)skip;
                continue;
            }
        }
        out[o++] = a[i];
    }
    out[o] = '\0';
    return true;
}

SourceList *parse_nyaa(const char *xml)
{
    if (!xml) return NULL;
    SourceList *l = calloc(1, sizeof(*l));
    if (!l) return NULL;
    int cap = 0;
    for (const char *p = strstr(xml, "<item>"); p; p = strstr(p + 6, "<item>")) cap++;
    l->items = calloc(cap ? (size_t)cap : 1, sizeof(Source));
    if (!l->items) return l;

    for (const char *p = strstr(xml, "<item>"); p; p = strstr(p + 6, "<item>")) {
        const char *end = strstr(p, "</item>");
        if (!end) break;
        Source *s = &l->items[l->count];
        memset(s, 0, sizeof(*s));
        char buf[64];
        xml_tag(p, end, "title", s->title, sizeof(s->title));
        xml_tag(p, end, "link", s->torrent_url, sizeof(s->torrent_url));
        xml_tag(p, end, "nyaa:infoHash", s->hash, sizeof(s->hash));
        xml_tag(p, end, "nyaa:size", s->size, sizeof(s->size));
        if (xml_tag(p, end, "nyaa:seeders", buf, sizeof(buf))) s->seeds = atoi(buf);
        if (xml_tag(p, end, "nyaa:trusted", buf, sizeof(buf))) s->trusted = strcmp(buf, "Yes") == 0;
        copy_text(s->provider, sizeof(s->provider), "Nyaa");
        s->file_idx = -1;
        const char *q = strstr(s->title, "2160p") ? "4K" : strstr(s->title, "1080p") ? "1080p"
                      : strstr(s->title, "720p") ? "720p" : strstr(s->title, "480p") ? "480p" : "";
        copy_text(s->quality, sizeof(s->quality), q);
        if (s->hash[0]) l->count++;
        p = end;
    }
    return l;
}

void meta_list_free(MetaList *l)
{
    if (!l) return;
    free(l->items);
    free(l);
}

void meta_full_free(MetaFull *m)
{
    if (!m) return;
    free(m->eps);
    free(m);
}

void source_list_free(SourceList *l)
{
    if (!l) return;
    free(l->items);
    free(l);
}
