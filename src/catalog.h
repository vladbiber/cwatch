#ifndef CWATCH_CATALOG_H
#define CWATCH_CATALOG_H

#include "common.h"

typedef struct {
    Meta *items;
    int count;
} MetaList;

typedef struct {
    Source *items;
    int count;
} SourceList;

void catalog_url(char *out, size_t len, const char *base, const char *type,
                 const char *catalog, const char *genre, const char *search);
void meta_url(char *out, size_t len, const char *type, const char *id);
void streams_url(char *out, size_t len, const char *type, const char *id);
void nyaa_url(char *out, size_t len, const char *query);

MetaList *parse_meta_list(const char *json);
MetaFull *parse_meta_full(const char *json);
SourceList *parse_torrentio(const char *json);
SourceList *parse_nyaa(const char *xml);

void meta_list_free(MetaList *l);
void meta_full_free(MetaFull *m);
void source_list_free(SourceList *l);
bool meta_is_series(const Meta *m);

#endif
