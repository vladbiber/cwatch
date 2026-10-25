#include "net.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_THREADS 8

typedef struct {
    Job *head;
    Job *tail;
} Queue;

static Queue hi_q, lo_q, done_q;
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cond = PTHREAD_COND_INITIALIZER;
static pthread_t threads[MAX_THREADS];
static int thread_count;
static bool quitting;
static int pending;

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} Buffer;

static void q_push(Queue *q, Job *job)
{
    job->next = NULL;
    if (q->tail) q->tail->next = job;
    else q->head = job;
    q->tail = job;
}

static Job *q_pop(Queue *q)
{
    Job *job = q->head;
    if (!job) return NULL;
    q->head = job->next;
    if (!q->head) q->tail = NULL;
    job->next = NULL;
    return job;
}

static size_t on_data(char *ptr, size_t size, size_t nmemb, void *user)
{
    Buffer *b = user;
    size_t n = size * nmemb;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 65536;
        while (cap < b->len + n + 1) cap *= 2;
        if (cap > 64u * 1024 * 1024) return 0;
        char *nb = realloc(b->buf, cap);
        if (!nb) return 0;
        b->buf = nb;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, ptr, n);
    b->len += n;
    b->buf[b->len] = '\0';
    return n;
}

static bool http_get(const char *url, char **data, size_t *len)
{
    CURL *c = curl_easy_init();
    if (!c) return false;
    Buffer b = {0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Linux x86_64) cwatch/2.0");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK || code < 200 || code >= 300 || b.len == 0) {
        free(b.buf);
        return false;
    }
    *data = b.buf;
    *len = b.len;
    return true;
}

static bool read_file(const char *path, char **data, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n <= 0) {
        fclose(fp);
        return false;
    }
    char *buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf);
        fclose(fp);
        return false;
    }
    buf[n] = '\0';
    fclose(fp);
    *data = buf;
    *len = (size_t)n;
    return true;
}

static void write_file(const char *path, const char *data, size_t len)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.%lx.tmp", path, (unsigned long)pthread_self());
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return;
    bool ok = fwrite(data, 1, len, fp) == len;
    fclose(fp);
    if (ok) rename(tmp, path);
    else unlink(tmp);
}

bool http_cached(const char *url, int ttl, char **data, size_t *len, char *path_out, size_t path_len)
{
    char dir[PATH_MAX], path[PATH_MAX], name[64];
    cache_dir(dir, sizeof(dir), "http");
    snprintf(name, sizeof(name), "%016lx", hash_string(url));
    path_join(path, sizeof(path), dir, name);
    if (path_out) copy_text(path_out, path_len, path);

    struct stat st;
    bool have = stat(path, &st) == 0 && st.st_size > 0;
    bool fresh = have && (ttl < 0 || (ttl > 0 && time(NULL) - st.st_mtime < ttl));
    if (fresh && read_file(path, data, len)) return true;

    if (http_get(url, data, len)) {
        if (ttl != 0) write_file(path, *data, *len);
        return true;
    }
    return have && read_file(path, data, len);
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&q_lock);
        while (!quitting && !hi_q.head && !lo_q.head)
            pthread_cond_wait(&q_cond, &q_lock);
        if (quitting) {
            pthread_mutex_unlock(&q_lock);
            return NULL;
        }
        Job *job = q_pop(&hi_q);
        if (!job) job = q_pop(&lo_q);
        pthread_mutex_unlock(&q_lock);

        if (job->url[0])
            job->ok = http_cached(job->url, job->ttl, &job->data, &job->len, NULL, 0);
        else
            job->ok = true;
        if (job->parse) job->parse(job);

        pthread_mutex_lock(&q_lock);
        q_push(&done_q, job);
        pthread_mutex_unlock(&q_lock);
    }
}

void net_init(int count)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (count > MAX_THREADS) count = MAX_THREADS;
    for (int i = 0; i < count; i++)
        if (pthread_create(&threads[thread_count], NULL, worker, NULL) == 0)
            thread_count++;
}

void net_shutdown(void)
{
    pthread_mutex_lock(&q_lock);
    quitting = true;
    pthread_cond_broadcast(&q_cond);
    pthread_mutex_unlock(&q_lock);
    for (int i = 0; i < thread_count; i++)
        pthread_detach(threads[i]);
}

Job *job_new(const char *url, int ttl, JobFn parse, JobFn apply)
{
    Job *job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    copy_text(job->url, sizeof(job->url), url);
    job->ttl = ttl;
    job->parse = parse;
    job->apply = apply;
    return job;
}

void net_submit(Job *job)
{
    if (!job) return;
    pthread_mutex_lock(&q_lock);
    q_push(job->low_priority ? &lo_q : &hi_q, job);
    pending++;
    pthread_cond_signal(&q_cond);
    pthread_mutex_unlock(&q_lock);
}

void net_poll(void)
{
    for (int i = 0; i < 64; i++) {
        pthread_mutex_lock(&q_lock);
        Job *job = q_pop(&done_q);
        if (job) pending--;
        pthread_mutex_unlock(&q_lock);
        if (!job) return;
        if (job->apply) job->apply(job);
        free(job->data);
        free(job);
    }
}

int net_pending(void)
{
    pthread_mutex_lock(&q_lock);
    int n = pending;
    pthread_mutex_unlock(&q_lock);
    return n;
}
