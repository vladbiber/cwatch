#ifndef CWATCH_NET_H
#define CWATCH_NET_H

#include "common.h"

typedef struct Job Job;
typedef void (*JobFn)(Job *job);

struct Job {
    char url[URL_LEN];
    int ttl;
    bool low_priority;
    char *data;
    size_t len;
    bool ok;
    void *result;
    int tag;
    unsigned gen;
    char text[PATH_MAX];
    JobFn parse;
    JobFn apply;
    Job *next;
};

void net_init(int threads);
void net_shutdown(void);
Job *job_new(const char *url, int ttl, JobFn parse, JobFn apply);
void net_submit(Job *job);
void net_poll(void);
int net_pending(void);
bool http_cached(const char *url, int ttl, char **data, size_t *len, char *path_out, size_t path_len);

#endif
