// cwatch-stream: torrent to local HTTP streamer / downloader for cwatch
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace lt = libtorrent;
namespace fs = std::filesystem;
using i64 = std::int64_t;
using clk = std::chrono::steady_clock;

static const char* TRACKERS[] = {
    "udp://tracker.opentrackr.org:1337/announce", "udp://open.stealth.si:80/announce",
    "udp://tracker.torrent.eu.org:451/announce", "udp://open.demonii.com:1337/announce",
    "udp://exodus.desync.com:6969/announce"};
static const i64 READAHEAD = 20 << 20;
static const i64 EDGE = 2 << 20;

static std::atomic<bool> g_quit{false};
static std::atomic<int> g_active{0};

struct Ctx {
    lt::torrent_handle h;
    int file = -1, plen = 0, first = 0, last = 0, ahead = 1;
    i64 foff = 0, fsize = 0;
    std::string path, ctype;
    std::mutex m;
    std::condition_variable cv;
    std::map<int, int> want;
    std::set<int> fds;
};
static Ctx C;

static void on_sig(int) { g_quit = true; }

[[noreturn]] static void die(std::string const& msg) {
    printf("ERROR %s\n", msg.c_str());
    fflush(stdout);
    _exit(1);
}

static std::string clean(std::string s) {
    for (char& c : s)
        if (c == '\n' || c == '\r') c = ' ';
    return s;
}

static std::string urlenc(std::string const& s) {
    static const char* hx = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += char(c);
        else { o += '%'; o += hx[c >> 4]; o += hx[c & 15]; }
    }
    return o;
}

static std::string lower(std::string s) {
    for (char& c : s) c = char(tolower((unsigned char)c));
    return s;
}

static std::string ext_of(std::string const& p) {
    auto d = p.rfind('.');
    return d == std::string::npos ? "" : lower(p.substr(d + 1));
}

static bool is_video(std::string const& p) {
    static const std::set<std::string> v = {"mkv", "mp4", "avi", "webm", "mov", "m4v", "ts"};
    return v.count(ext_of(p)) > 0;
}

static std::string mime(std::string const& p) {
    static const std::map<std::string, std::string> m = {
        {"mkv", "video/x-matroska"}, {"mp4", "video/mp4"}, {"m4v", "video/mp4"},
        {"webm", "video/webm"}, {"avi", "video/x-msvideo"}, {"mov", "video/quicktime"},
        {"ts", "video/mp2t"}, {"mp3", "audio/mpeg"}, {"flac", "audio/flac"}};
    auto it = m.find(ext_of(p));
    return it == m.end() ? "application/octet-stream" : it->second;
}

static std::string hexhash(lt::sha1_hash const& h) {
    static const char* hx = "0123456789abcdef";
    std::string o;
    for (auto c : h) { o += hx[(unsigned char)c >> 4]; o += hx[(unsigned char)c & 15]; }
    return o;
}

static bool is_hex40(std::string const& s) {
    return s.size() == 40 && std::all_of(s.begin(), s.end(), [](char c) { return isxdigit((unsigned char)c); });
}

static lt::add_torrent_params parse_source(std::string const& src) {
    lt::error_code ec;
    if (is_hex40(src) || src.rfind("magnet:", 0) == 0) {
        auto p = lt::parse_magnet_uri(is_hex40(src) ? "magnet:?xt=urn:btih:" + src : src, ec);
        if (ec) die("bad magnet: " + ec.message());
        if (is_hex40(src))
            for (auto t : TRACKERS) p.trackers.push_back(t);
        return p;
    }
    lt::add_torrent_params p;
    auto ti = std::make_shared<lt::torrent_info>(src, ec);
    if (ec) die("bad torrent file: " + ec.message());
    p.ti = ti;
    return p;
}

static void watch_stdin() {
    struct stat st;
    if (fstat(0, &st) != 0 || !(S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode))) return;
    std::thread([] {
        char b[256];
        for (;;) {
            ssize_t n = read(0, b, sizeof b);
            if (n == 0 || (n < 0 && errno != EINTR)) { g_quit = true; return; }
        }
    }).detach();
}

static void print_files(lt::torrent_info const& ti, int only) {
    auto const& st = ti.files();
    for (int i = 0; i < st.num_files(); ++i) {
        lt::file_index_t fi(i);
        if (st.pad_file_at(fi) || (only >= 0 && i != only)) continue;
        printf("FILE %d %lld %s\n", i, (long long)st.file_size(fi), clean(st.file_path(fi)).c_str());
    }
}

// HTTP

static bool send_all(int fd, const char* p, size_t n) {
    while (n) {
        ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r;
        n -= size_t(r);
    }
    return true;
}

static bool hung_up(int fd) {
    pollfd p{fd, POLLRDHUP, 0};
    return poll(&p, 1, 0) > 0 && (p.revents & (POLLRDHUP | POLLHUP | POLLERR));
}

static bool have(int p) {
    try { return C.h.have_piece(lt::piece_index_t(p)); } catch (...) { return false; }
}

static void release(int p) {
    auto it = C.want.find(p);
    if (it == C.want.end()) return;
    if (--it->second == 0) {
        C.want.erase(it);
        if (p != C.first && p != C.last) C.h.reset_piece_deadline(lt::piece_index_t(p));
    }
}

static void update_window(std::set<int>& mine, int cur) {
    int end = std::min(C.last, cur + C.ahead);
    std::lock_guard<std::mutex> lk(C.m);
    for (auto it = mine.begin(); it != mine.end();) {
        if (*it < cur || *it > end) { release(*it); it = mine.erase(it); }
        else ++it;
    }
    for (int p = cur; p <= end; ++p) {
        if (mine.insert(p).second) C.want[p]++;
        C.h.set_piece_deadline(lt::piece_index_t(p), (p - cur) * 150);
    }
}

static bool wait_piece(int fd, int p) {
    std::unique_lock<std::mutex> lk(C.m);
    for (;;) {
        if (g_quit) return false;
        lk.unlock();
        bool ok = have(p);
        bool gone = !ok && hung_up(fd);
        lk.lock();
        if (ok) return true;
        if (gone) return false;
        C.cv.wait_for(lk, std::chrono::milliseconds(500));
    }
}

static bool parse_range(std::string v, i64 size, i64& a, i64& b) {
    v.erase(std::remove(v.begin(), v.end(), ' '), v.end());
    if (lower(v).rfind("bytes=", 0) != 0) return false;
    v = v.substr(6);
    auto c = v.find(',');
    if (c != std::string::npos) v = v.substr(0, c);
    auto d = v.find('-');
    if (d == std::string::npos) return false;
    std::string s = v.substr(0, d), e = v.substr(d + 1);
    char* endp;
    if (s.empty()) {
        if (e.empty()) return false;
        i64 n = strtoll(e.c_str(), &endp, 10);
        if (*endp || n <= 0) return false;
        a = std::max<i64>(0, size - n);
        b = size - 1;
    } else {
        a = strtoll(s.c_str(), &endp, 10);
        if (*endp || a < 0) return false;
        b = size - 1;
        if (!e.empty()) {
            i64 x = strtoll(e.c_str(), &endp, 10);
            if (*endp || x < a) return false;
            b = std::min(b, x);
        }
    }
    return a < size;
}

static void serve(int fd) {
    timeval tv{15, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    std::string req;
    char rb[4096];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
        ssize_t n = recv(fd, rb, sizeof rb, 0);
        if (n <= 0) break;
        req.append(rb, size_t(n));
    }
    auto hend = req.find("\r\n\r\n");
    std::string method = req.substr(0, req.find(' '));
    std::string range;
    bool has_range = false;
    for (size_t p = req.find("\r\n"); hend != std::string::npos && p < hend;) {
        size_t q = req.find("\r\n", p + 2);
        std::string line = req.substr(p + 2, q - p - 2);
        auto c = line.find(':');
        if (c != std::string::npos && lower(line.substr(0, c)) == "range") {
            range = line.substr(c + 1);
            has_range = true;
        }
        p = q;
    }
    i64 size = C.fsize, a = 0, b = size - 1;
    std::string hdr;
    bool body = false;
    if (hend == std::string::npos) {
        hdr = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    } else if (method != "GET" && method != "HEAD") {
        hdr = "HTTP/1.1 405 Method Not Allowed\r\nAllow: GET, HEAD\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    } else if (has_range && !parse_range(range, size, a, b)) {
        hdr = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" + std::to_string(size) +
              "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    } else {
        hdr = std::string(has_range ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n") +
              "Content-Type: " + C.ctype + "\r\nAccept-Ranges: bytes\r\nContent-Length: " +
              std::to_string(size > 0 ? b - a + 1 : 0) + "\r\n";
        if (has_range)
            hdr += "Content-Range: bytes " + std::to_string(a) + "-" + std::to_string(b) + "/" + std::to_string(size) + "\r\n";
        hdr += "Connection: close\r\n\r\n";
        body = method == "GET" && size > 0;
    }
    std::set<int> mine;
    if (send_all(fd, hdr.data(), hdr.size()) && body) {
        std::vector<char> buf(1 << 18);
        int rfd = -1, curp = -1;
        bool ready = false;
        i64 pos = a;
        while (pos <= b && !g_quit) {
            int p = int((C.foff + pos) / C.plen);
            if (p != curp) { curp = p; ready = false; update_window(mine, p); }
            if (!ready && !(ready = wait_piece(fd, p))) break;
            i64 pend = i64(p + 1) * C.plen - C.foff;
            i64 n = std::min({i64(buf.size()), pend - pos, b + 1 - pos});
            if (rfd < 0 && (rfd = open(C.path.c_str(), O_RDONLY | O_CLOEXEC)) < 0) break;
            ssize_t r = pread(rfd, buf.data(), size_t(n), pos);
            if (r <= 0 || !send_all(fd, buf.data(), size_t(r))) break;
            pos += r;
        }
        if (rfd >= 0) close(rfd);
    }
    {
        std::lock_guard<std::mutex> lk(C.m);
        for (int p : mine) release(p);
        C.fds.erase(fd);
    }
    shutdown(fd, SHUT_WR);
    close(fd);
    g_active--;
}

static int start_http(int port, int& bound) {
    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(uint16_t(port));
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(lfd, (sockaddr*)&sa, sizeof sa) < 0 || listen(lfd, 16) < 0) return -1;
    socklen_t sl = sizeof sa;
    getsockname(lfd, (sockaddr*)&sa, &sl);
    bound = ntohs(sa.sin_port);
    std::thread([lfd] {
        for (;;) {
            int c = accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
            if (c < 0) {
                if (g_quit) return;
                if (errno == EINTR || errno == ECONNABORTED) continue;
                return;
            }
            {
                std::lock_guard<std::mutex> lk(C.m);
                C.fds.insert(c);
            }
            g_active++;
            std::thread(serve, c).detach();
        }
    }).detach();
    return lfd;
}

// torrent

enum Mode { FILES, STREAM, DOWNLOAD };

static int pick_file(lt::file_storage const& st) {
    int best = -1, any = -1;
    for (int i = 0; i < st.num_files(); ++i) {
        lt::file_index_t fi(i);
        if (st.pad_file_at(fi)) continue;
        if (any < 0 || st.file_size(fi) > st.file_size(lt::file_index_t(any))) any = i;
        if (is_video(st.file_path(fi)) && (best < 0 || st.file_size(fi) > st.file_size(lt::file_index_t(best)))) best = i;
    }
    return best >= 0 ? best : any;
}

static void prioritize_edges() {
    i64 head = std::min(C.fsize, std::max<i64>(EDGE, 1));
    int hp = int((C.foff + head - 1) / C.plen);
    int tp = int((C.foff + std::max<i64>(0, C.fsize - EDGE)) / C.plen);
    for (int p = C.first; p <= hp; ++p) C.h.set_piece_deadline(lt::piece_index_t(p), (p - C.first) * 100);
    for (int p = std::max(tp, hp + 1); p <= C.last; ++p) C.h.set_piece_deadline(lt::piece_index_t(p), 400 + (p - tp) * 100);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa{};
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    if (argc < 3) {
        fprintf(stderr, "usage: cwatch-stream files <source>\n"
                        "       cwatch-stream stream <source> [--file N] [--port P] [--keep]\n"
                        "       cwatch-stream download <source> --dest DIR [--file N]\n");
        return 2;
    }
    std::string cmd = argv[1], src = argv[2], dest;
    Mode mode = cmd == "files" ? FILES : cmd == "stream" ? STREAM : cmd == "download" ? DOWNLOAD : Mode(-1);
    if (mode == Mode(-1)) die("unknown command " + cmd);
    int want_file = -1, port = 0;
    bool keep = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--file" && i + 1 < argc) want_file = atoi(argv[++i]);
        else if (a == "--port" && i + 1 < argc) port = atoi(argv[++i]);
        else if (a == "--dest" && i + 1 < argc) dest = argv[++i];
        else if (a == "--keep") keep = true;
        else die("bad argument " + a);
    }
    if (mode == DOWNLOAD && dest.empty()) die("--dest required");

    auto atp = parse_source(src);
    if (mode == FILES && atp.ti) {
        printf("META %s\n", clean(atp.ti->name()).c_str());
        print_files(*atp.ti, -1);
        return 0;
    }
    watch_stdin();

    std::string ih = hexhash(atp.ti ? atp.ti->info_hashes().get_best() : atp.info_hashes.get_best());
    std::string save;
    bool temp = mode != DOWNLOAD;
    if (temp) {
        const char* home = getenv("HOME");
        save = std::string(home ? home : "/tmp") + "/.cache/cwatch/stream/" + ih;
    } else {
        save = fs::absolute(dest).string();
    }
    std::error_code fec;
    fs::create_directories(save, fec);
    if (fec) die("cannot create " + save + ": " + fec.message());
    atp.save_path = save;
    if (mode == STREAM) atp.flags |= lt::torrent_flags::sequential_download;

    lt::settings_pack sp;
    sp.set_int(lt::settings_pack::alert_mask, lt::alert_category::error | lt::alert_category::status |
                                                  lt::alert_category::storage | lt::alert_category::piece_progress);
    sp.set_str(lt::settings_pack::listen_interfaces, "0.0.0.0:0,[::]:0");
    sp.set_str(lt::settings_pack::user_agent, "cwatch-stream/1.0");
    sp.set_int(lt::settings_pack::stop_tracker_timeout, 1);
    auto ses = std::make_unique<lt::session>(lt::session_params(sp));
    lt::error_code ec;
    C.h = ses->add_torrent(std::move(atp), ec);
    if (ec) {
        if (temp && !keep) fs::remove_all(save, fec);
        die("add torrent: " + ec.message());
    }

    std::string err;
    int lfd = -1, rc = 0;
    bool meta = false, done = false, said = false;
    std::vector<int> sel;
    i64 sel_total = 0;
    auto t0 = clk::now(), last_stat = t0 - std::chrono::seconds(1);
    int meta_timeout = mode == FILES ? 60 : 120;

    auto on_meta = [&]() {
        auto ti = C.h.torrent_file();
        if (!ti) return;
        meta = true;
        auto const& st = ti->files();
        printf("META %s\n", clean(ti->name()).c_str());
        if (mode == FILES) { print_files(*ti, -1); g_quit = true; return; }
        if (want_file >= 0 && (want_file >= st.num_files() || st.pad_file_at(lt::file_index_t(want_file)))) {
            err = "no such file index " + std::to_string(want_file);
            return;
        }
        if (mode == STREAM) {
            if (want_file < 0) want_file = pick_file(st);
            if (want_file < 0) { err = "torrent has no files"; return; }
        }
        std::vector<lt::download_priority_t> pr(size_t(st.num_files()),
                                                want_file < 0 ? lt::default_priority : lt::dont_download);
        for (int i = 0; i < st.num_files(); ++i) {
            lt::file_index_t fi(i);
            if (st.pad_file_at(fi)) pr[size_t(i)] = lt::dont_download;
            else if (want_file < 0 || i == want_file) { sel.push_back(i); sel_total += st.file_size(fi); }
        }
        if (want_file >= 0) pr[size_t(want_file)] = lt::default_priority;
        C.h.prioritize_files(pr);
        print_files(*ti, want_file);
        if (mode != STREAM) return;
        lt::file_index_t fi(want_file);
        C.file = want_file;
        C.plen = ti->piece_length();
        C.foff = st.file_offset(fi);
        C.fsize = st.file_size(fi);
        C.path = st.file_path(fi, save);
        C.ctype = mime(C.path);
        C.first = int(C.foff / C.plen);
        C.last = int((C.foff + std::max<i64>(C.fsize, 1) - 1) / C.plen);
        C.ahead = int(std::max<i64>(1, READAHEAD / C.plen));
        prioritize_edges();
        int bound = 0;
        lfd = start_http(port, bound);
        if (lfd < 0) { err = "cannot bind 127.0.0.1:" + std::to_string(port) + ": " + strerror(errno); return; }
        std::string fname = std::string(st.file_name(fi));
        printf("URL http://127.0.0.1:%d/%s\n", bound, urlenc(fname).c_str());
    };

    if (C.h.status(lt::status_flags_t{}).has_metadata) on_meta();

    while (!g_quit && err.empty()) {
        ses->wait_for_alert(std::chrono::milliseconds(250));
        std::vector<lt::alert*> alerts;
        ses->pop_alerts(&alerts);
        for (auto* a : alerts) {
            if (lt::alert_cast<lt::metadata_received_alert>(a)) { if (!meta) on_meta(); }
            else if (lt::alert_cast<lt::piece_finished_alert>(a)) { std::lock_guard<std::mutex> lk(C.m); C.cv.notify_all(); }
            else if (auto* e = lt::alert_cast<lt::torrent_error_alert>(a)) err = clean(e->message());
            else if (auto* e = lt::alert_cast<lt::file_error_alert>(a)) err = clean(e->message());
            if (!err.empty() || g_quit) break;
        }
        if (!err.empty() || g_quit) break;
        auto now = clk::now();
        if (!meta && now - t0 > std::chrono::seconds(meta_timeout)) { err = "metadata timeout"; break; }
        if (mode == FILES || now - last_stat < std::chrono::seconds(1)) continue;
        last_stat = now;
        auto s = C.h.status(lt::status_flags_t{});
        double prog = 0;
        if (meta && sel_total > 0) {
            auto fp = C.h.file_progress();
            i64 got = 0;
            for (int i : sel) got += fp[size_t(i)];
            prog = double(got) / double(sel_total);
            done = got >= sel_total;
        } else if (meta && !sel.empty()) {
            done = true;
        }
        printf("STAT %.4f %d %d %d\n", prog, s.download_payload_rate, s.num_peers, s.num_seeds);
        if (done && !said) { said = true; printf("DONE\n"); }
        if (done && mode == DOWNLOAD) break;
    }

    if (!err.empty()) { printf("ERROR %s\n", err.c_str()); rc = 1; }
    g_quit = true;
    if (lfd >= 0) { shutdown(lfd, SHUT_RDWR); close(lfd); }
    {
        std::lock_guard<std::mutex> lk(C.m);
        for (int fd : C.fds) shutdown(fd, SHUT_RDWR);
        C.cv.notify_all();
    }
    for (int i = 0; i < 40 && g_active > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ses.reset();
    if (temp && !keep) fs::remove_all(save, fec);
    if (mode == DOWNLOAD && done && want_file >= 0) fs::remove(fs::path(save) / ("." + ih + ".parts"), fec);
    fflush(stdout);
    _exit(rc);
}
