#include "common.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

int win_w = 1280;
int win_h = 760;
unsigned long frame_no = 0;

void copy_text(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return;
    if (!src) src = "";
    size_t len = strnlen(src, dst_len - 1);
    memmove(dst, src, len);
    dst[len] = '\0';
}

bool path_join(char *out, size_t out_len, const char *left, const char *right)
{
    if (!out || out_len == 0) return false;
    if (!left) left = "";
    if (!right) right = "";

    int written;
    size_t left_len = strlen(left);
    if (left_len > 0 && left[left_len - 1] == '/')
        written = snprintf(out, out_len, "%s%s", left, right);
    else
        written = snprintf(out, out_len, "%s/%s", left, right);

    if (written < 0 || (size_t)written >= out_len) {
        out[0] = '\0';
        return false;
    }
    return true;
}

int natural_compare(const char *a, const char *b)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;

    while (*pa || *pb) {
        if (isdigit(*pa) && isdigit(*pb)) {
            while (*pa == '0') pa++;
            while (*pb == '0') pb++;

            const unsigned char *na = pa;
            const unsigned char *nb = pb;
            while (isdigit(*na)) na++;
            while (isdigit(*nb)) nb++;

            int len_a = (int)(na - pa);
            int len_b = (int)(nb - pb);
            if (len_a != len_b) return len_a - len_b;

            int cmp = strncmp((const char *)pa, (const char *)pb, (size_t)len_a);
            if (cmp != 0) return cmp;

            pa = na;
            pb = nb;
            continue;
        }

        int ca = *pa ? tolower(*pa) : -1;
        int cb = *pb ? tolower(*pb) : -1;
        if (ca != cb) return ca - cb;
        if (*pa) pa++;
        if (*pb) pb++;
    }

    return 0;
}

bool command_exists(const char *cmd)
{
    if (strchr(cmd, '/')) return access(cmd, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path) return false;
    char copy[4096];
    copy_text(copy, sizeof(copy), path);
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        char full[PATH_MAX];
        if (!path_join(full, sizeof(full), dir, cmd))
            continue;
        if (access(full, X_OK) == 0) return true;
    }
    return false;
}

unsigned long hash_string(const char *s)
{
    unsigned long hash = 1469598103934665603UL;
    while (*s) {
        hash ^= (unsigned char)*s++;
        hash *= 1099511628211UL;
    }
    return hash;
}

void expand_path(const char *in, char *out, size_t out_len)
{
    if (!in || in[0] == '\0') {
        out[0] = '\0';
        return;
    }
    const char *home = getenv("HOME");
    if (in[0] == '~' && (in[1] == '/' || in[1] == '\0') && home)
        snprintf(out, out_len, "%s%s", home, in + 1);
    else
        copy_text(out, out_len, in);
}

const char *basename_of(const char *path)
{
    const char *base = strrchr(path, '/');
    if (base && base[1]) return base + 1;
    return path;
}

void prettify_name(const char *in, char *out, size_t out_len)
{
    copy_text(out, out_len, in);
    for (char *p = out; *p; p++)
        if (*p == '_' || *p == '.') *p = ' ';
}

void trim_line(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                       line[len - 1] == ' ' || line[len - 1] == '\t'))
        line[--len] = '\0';
}

void mkdir_p(const char *path)
{
    char tmp[PATH_MAX];
    copy_text(tmp, sizeof(tmp), path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(tmp, 0755);
        *p = '/';
    }
    mkdir(tmp, 0755);
}

static void xdg_dir(char *out, size_t out_len, const char *env, const char *fallback)
{
    const char *base = getenv(env);
    const char *home = getenv("HOME");
    if (base && base[0])
        snprintf(out, out_len, "%s/cwatch", base);
    else if (home && home[0])
        snprintf(out, out_len, "%s/%s/cwatch", home, fallback);
    else
        snprintf(out, out_len, ".cwatch");
    mkdir_p(out);
}

void state_dir(char *out, size_t out_len)
{
    xdg_dir(out, out_len, "XDG_STATE_HOME", ".local/state");
}

void state_file(char *out, size_t out_len, const char *name)
{
    char dir[PATH_MAX];
    state_dir(dir, sizeof(dir));
    path_join(out, out_len, dir, name);
}

void cache_dir(char *out, size_t out_len, const char *sub)
{
    char base[PATH_MAX];
    xdg_dir(base, sizeof(base), "XDG_CACHE_HOME", ".cache");
    path_join(out, out_len, base, sub);
    mkdir_p(out);
}

void watch_later_dir(char *out, size_t out_len)
{
    char dir[PATH_MAX];
    state_dir(dir, sizeof(dir));
    path_join(out, out_len, dir, "watch_later");
    mkdir_p(out);
}

void url_encode(const char *in, char *out, size_t out_len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < out_len; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 15];
        }
    }
    out[o] = '\0';
}

void format_clock(double seconds, char *out, size_t out_len)
{
    int total = seconds > 0.0 ? (int)(seconds + 0.5) : 0;
    int hh = total / 3600;
    int mm = (total % 3600) / 60;
    int ss = total % 60;
    if (hh > 0)
        snprintf(out, out_len, "%d:%02d:%02d", hh, mm, ss);
    else
        snprintf(out, out_len, "%02d:%02d", mm, ss);
}

void format_bytes(double bytes, char *out, size_t out_len)
{
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int u = 0;
    while (bytes >= 1024.0 && u < 4) {
        bytes /= 1024.0;
        u++;
    }
    snprintf(out, out_len, u >= 2 ? "%.1f %s" : "%.0f %s", bytes, units[u]);
}

static bool is_noise_word(const char *w)
{
    static const char *noise[] = {
        "1080p", "720p", "480p", "2160p", "4k", "bluray", "bdrip", "brrip", "web", "webrip",
        "web-dl", "hdtv", "x264", "x265", "h264", "h265", "hevc", "av1", "aac", "opus",
        "flac", "dual", "multi", "complete", "batch", "remux", "10bit", "dub", "sub", NULL
    };
    for (int i = 0; noise[i]; i++)
        if (strcasecmp(w, noise[i]) == 0) return true;
    if ((w[0] == 's' || w[0] == 'S') && isdigit((unsigned char)w[1])) return true;
    return false;
}

void clean_title(const char *in, char *out, size_t out_len)
{
    char buf[TITLE_LEN * 2];
    size_t o = 0;
    int depth = 0;
    for (const char *p = in; *p && o + 1 < sizeof(buf); p++) {
        if (*p == '[' || *p == '(' || *p == '{') { depth++; continue; }
        if (*p == ']' || *p == ')' || *p == '}') { if (depth) depth--; continue; }
        if (depth) continue;
        buf[o++] = (*p == '.' || *p == '_') ? ' ' : *p;
    }
    buf[o] = '\0';

    out[0] = '\0';
    size_t len = 0;
    char *save = NULL;
    for (char *w = strtok_r(buf, " ", &save); w; w = strtok_r(NULL, " ", &save)) {
        if (is_noise_word(w) || strcmp(w, "-") == 0) {
            if (len > 0) break;
            continue;
        }
        int n = snprintf(out + len, out_len - len, "%s%s", len ? " " : "", w);
        if (n < 0 || (size_t)n >= out_len - len) break;
        len += (size_t)n;
    }
}

bool parse_episode_tag(const char *name, int *season, int *episode)
{
    for (const char *p = name; *p; p++) {
        if ((p[0] == 'S' || p[0] == 's') && isdigit((unsigned char)p[1])) {
            char *end;
            long s = strtol(p + 1, &end, 10);
            if ((*end == 'E' || *end == 'e') && isdigit((unsigned char)end[1])) {
                *season = (int)s;
                *episode = (int)strtol(end + 1, NULL, 10);
                return true;
            }
        }
    }
    const char *dash = strstr(name, " - ");
    if (dash && isdigit((unsigned char)dash[3])) {
        *season = 1;
        *episode = atoi(dash + 3);
        return *episode > 0;
    }
    return false;
}

void make_magnet(const char *hash, const char *name, char *out, size_t out_len)
{
    static const char *trackers[] = {
        "udp://tracker.opentrackr.org:1337/announce",
        "udp://open.stealth.si:80/announce",
        "udp://tracker.torrent.eu.org:451/announce",
        "udp://open.demonii.com:1337/announce",
        "udp://exodus.desync.com:6969/announce",
        "http://nyaa.tracker.wf:7777/announce",
        NULL
    };
    char dn[512];
    url_encode(name ? name : "", dn, sizeof(dn));
    int n = snprintf(out, out_len, "magnet:?xt=urn:btih:%s&dn=%s", hash, dn);
    for (int i = 0; trackers[i] && n > 0 && (size_t)n < out_len; i++) {
        char enc[256];
        url_encode(trackers[i], enc, sizeof(enc));
        n += snprintf(out + n, out_len - (size_t)n, "&tr=%s", enc);
    }
}
