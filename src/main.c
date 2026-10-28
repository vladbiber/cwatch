#include "app.h"
#include "images.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void text_append(char *buf, size_t cap, const char *txt)
{
    if (strlen(buf) + strlen(txt) < cap) strcat(buf, txt);
}

static void backspace(char *buf)
{
    size_t n = strlen(buf);
    while (n > 0 && ((unsigned char)buf[n - 1] & 0xC0) == 0x80) n--;
    if (n > 0) n--;
    buf[n] = '\0';
}

static bool search_typing(App *a)
{
    return a->tab == TAB_SEARCH && a->focus == FOCUS_HERO && !a->det.open && !a->src.open && !a->files.open;
}

static void on_text(App *a, const char *txt)
{
    if (a->files.open) return;
    if (a->src.open) {
        sources_text(a, txt);
        return;
    }
    if (a->det.open) return;
    if (a->input_root_active) {
        text_append(a->input_root, sizeof(a->input_root), txt);
    } else if (search_typing(a)) {
        text_append(a->query, sizeof(a->query), txt);
        a->query_dirty = true;
        a->query_changed = SDL_GetTicks();
    } else if (a->tab == TAB_NYAA && a->typing) {
        text_append(a->nyaa_query, sizeof(a->nyaa_query), txt);
    }
}

static bool typing_now(App *a)
{
    if (a->files.open) return false;
    if (a->src.open) return a->src.typing;
    if (a->det.open) return false;
    return a->input_root_active || search_typing(a) || (a->tab == TAB_NYAA && a->typing);
}

static void go_back(App *a)
{
    if (a->focus >= 0 || (a->focus == FOCUS_HERO && a->tab != TAB_HOME)) {
        if (a->focus >= 0) a->focus = FOCUS_HERO;
        else set_tab(a, TAB_HOME);
        return;
    }
    if (a->tab != TAB_HOME) {
        set_tab(a, TAB_HOME);
        return;
    }
    a->running = false;
}

static void on_key(App *a, SDL_Keycode key, Uint16 mod)
{
    if ((mod & KMOD_CTRL) && key == SDLK_q) {
        a->running = false;
        return;
    }
    const StreamStatus *st = stream_status();
    if (st->state == ST_CONNECTING) {
        if (key == SDLK_ESCAPE || key == SDLK_BACKSPACE) stream_cancel();
        return;
    }
    if (a->files.open) {
        files_key(a, key);
        return;
    }
    if (a->src.open) {
        sources_key(a, key);
        return;
    }
    if (a->det.open) {
        if (!detail_key(a, key) && key == SDLK_p && a->player_count > 0)
            a->player_idx = (a->player_idx + 1) % a->player_count;
        return;
    }
    if (a->input_root_active) {
        if (key == SDLK_ESCAPE) a->input_root_active = false;
        else if (key == SDLK_BACKSPACE) backspace(a->input_root);
        else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            a->input_root_active = false;
            if (library_add_root(a->lib, a->input_root)) {
                app_toast(a, "Added %s", a->input_root);
                browse_build(a);
            } else {
                app_toast(a, "Not a folder: %s", a->input_root);
            }
        }
        return;
    }
    if (search_typing(a)) {
        if (key == SDLK_BACKSPACE) {
            backspace(a->query);
            a->query_dirty = true;
            a->query_changed = SDL_GetTicks();
            return;
        }
        if (key == SDLK_ESCAPE) {
            if (a->query[0]) {
                a->query[0] = '\0';
                browse_build(a);
            } else {
                set_tab(a, TAB_HOME);
            }
            return;
        }
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            if (a->query_dirty) {
                a->query_dirty = false;
                browse_build(a);
            }
            if (a->row_count > 0) a->focus = 0;
            return;
        }
        if (key == SDLK_DOWN) {
            if (a->row_count > 0) a->focus = 0;
            return;
        }
        if (key == SDLK_UP) {
            a->focus = FOCUS_NAV;
            return;
        }
        return;
    }
    if (a->tab == TAB_NYAA && a->focus != FOCUS_NAV) {
        if (!a->typing && key == SDLK_ESCAPE) {
            set_tab(a, TAB_HOME);
            return;
        }
        if (!a->typing && key == SDLK_UP && a->nyaa_sel == 0 && a->nyaa) {
            a->focus = FOCUS_NAV;
            return;
        }
        if (nyaa_key(a, key)) return;
    } else if (a->tab == TAB_DOWNLOADS && a->focus != FOCUS_NAV) {
        if (key == SDLK_ESCAPE) {
            set_tab(a, TAB_HOME);
            return;
        }
        if (downloads_key(a, key)) return;
    } else if (a->tab != TAB_NYAA && a->tab != TAB_DOWNLOADS) {
        if (browse_key(a, key)) return;
    } else if (a->focus == FOCUS_NAV) {
        if (key == SDLK_DOWN) {
            a->focus = FOCUS_HERO;
            if (a->tab == TAB_NYAA && !a->nyaa) a->typing = true;
            return;
        }
        if (browse_key(a, key)) return;
    }

    switch (key) {
    case SDLK_ESCAPE:
        go_back(a);
        break;
    case SDLK_SLASH:
        set_tab(a, TAB_SEARCH);
        a->query[0] = '\0';
        break;
    case SDLK_p:
        a->player_idx = (a->player_idx + 1) % a->player_count;
        app_toast(a, "Player: %s", a->players[a->player_idx].name);
        break;
    case SDLK_r:
        library_scan(a->lib);
        library_request_matches(a->lib);
        library_request_durations(a->lib);
        browse_refresh_local(a);
        app_toast(a, "Library rescanned");
        break;
    case SDLK_d:
        if (a->tab == TAB_LIBRARY || a->tab == TAB_HOME) {
            a->input_root_active = true;
            a->input_root[0] = '\0';
        }
        break;
    default:
        break;
    }
}

static void on_click(App *a, int x, int y)
{
    if (stream_status()->state == ST_CONNECTING) return;
    if (a->files.open) {
        files_click(a, x, y);
        return;
    }
    if (a->src.open) {
        sources_click(a, x, y);
        return;
    }
    if (a->det.open) {
        if (y < NAV_H && x < 300) detail_close(a);
        else detail_click(a, x, y);
        return;
    }
    int t = nav_hit(a, x, y);
    if (t >= 0) {
        set_tab(a, t);
        return;
    }
    if (a->tab == TAB_NYAA) nyaa_click(a, x, y);
    else if (a->tab == TAB_DOWNLOADS) downloads_click(a, x, y);
    else browse_click(a, x, y);
}

static void on_wheel(App *a, int dx, int dy)
{
    if (a->files.open) files_wheel(a, dy);
    else if (a->src.open) sources_wheel(a, dy);
    else if (a->det.open) detail_wheel(a, dy);
    else if (a->tab == TAB_NYAA) nyaa_wheel(a, dy);
    else if (a->tab == TAB_DOWNLOADS) downloads_key(a, dy < 0 ? SDLK_DOWN : SDLK_UP);
    else browse_wheel(a, dx, dy);
}

static void debug_keys(App *a, Uint32 start)
{
    static const char *keys;
    static int step;
    if (!keys) keys = getenv("CWATCH_KEYS");
    if (!keys || !keys[0]) return;
    Uint32 due = start + 2500 + (Uint32)step * 900;
    if (SDL_GetTicks() < due) return;
    const char *p = keys;
    for (int i = 0; i < step && p; i++) {
        p = strchr(p, ',');
        if (p) p++;
    }
    if (!p || !*p) return;
    char name[32];
    size_t n = strcspn(p, ",");
    if (n >= sizeof(name)) n = sizeof(name) - 1;
    memcpy(name, p, n);
    name[n] = '\0';
    step++;
    if (strncmp(name, "text:", 5) == 0) {
        on_text(a, name + 5);
        return;
    }
    SDL_Keycode k = SDL_GetKeyFromName(name);
    if (k != SDLK_UNKNOWN) on_key(a, k, 0);
}

static void debug_shot(App *a, Uint32 start)
{
    static const char *path;
    static Uint32 after;
    if (!path) {
        path = getenv("CWATCH_SHOT");
        const char *ms = getenv("CWATCH_SHOT_AFTER");
        after = ms ? (Uint32)atoi(ms) : 8000;
        if (!path) path = "";
    }
    if (!path[0] || SDL_GetTicks() - start < after) return;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, win_w, win_h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (s && SDL_RenderReadPixels(a->r, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0)
        SDL_SaveBMP(s, path);
    if (s) SDL_FreeSurface(s);
    a->running = false;
}

static void render(App *a)
{
    if (a->det.open) {
        detail_render(a);
    } else {
        if (a->tab == TAB_NYAA) nyaa_render(a);
        else if (a->tab == TAB_DOWNLOADS) downloads_render(a);
        else browse_render(a);
        nav_render(a);
    }
    if (a->src.open) sources_render(a);
    if (a->files.open) files_render(a);
    if (a->input_root_active) {
        int w = 700, h = 56, x = (win_w - w) / 2, y = win_h - 140;
        fill_round(a->r, x - 4, y - 4, w + 8, h + 8, 10, (Color){0, 0, 0, 200});
        fill_round(a->r, x, y, w, h, 8, (Color){45, 45, 45, 255});
        char q[PATH_MAX + 32];
        snprintf(q, sizeof(q), "Add folder: %s|", a->input_root);
        draw_text_fit(a->r, a->ui.md, q, x + 18, y + 15, w - 36, C_WHITE);
    }
    status_render(a);
}

int main(int argc, char **argv)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (TTF_Init() != 0) {
        fprintf(stderr, "TTF_Init: %s\n", TTF_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_Window *window = SDL_CreateWindow(APP_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          win_w, win_h, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return 1;
    }

    App *a = calloc(1, sizeof(*a));
    Library *lib = calloc(1, sizeof(*lib));
    if (!a || !lib) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }
    a->r = renderer;
    a->lib = lib;
    a->running = true;
    if (!gfx_init(renderer, &a->ui)) {
        fprintf(stderr, "cwatch: no usable font found\n");
        return 1;
    }
    images_init(renderer);
    net_init(6);
    playback_init();
    a->player_count = discover_players(a->players);

    library_init(lib, argc >= 2 ? argv[1] : "~/media");
    library_scan(lib);
    library_request_matches(lib);
    library_request_durations(lib);
    set_tab(a, TAB_HOME);
    SDL_StartTextInput();
    const char *test_magnet = getenv("CWATCH_TEST_MAGNET");
    if (test_magnet) {
        const char *mode = getenv("CWATCH_TEST_MODE");
        if (mode && strcmp(mode, "download") == 0) download_start(test_magnet, -1, lib->roots[0], "test download");
        else play_torrent(test_magnet, -1, "test stream", NULL);
    }

    int last_state = ST_IDLE;
    Uint32 last_activity = SDL_GetTicks();
    Uint32 start = SDL_GetTicks();
    while (a->running) {
        SDL_Event ev;
        bool idle = !getenv("CWATCH_SHOT") && SDL_GetTicks() - last_activity > 2000 && net_pending() == 0 &&
                    stream_status()->state == ST_IDLE && downloads_active() == 0;
        bool got = idle ? SDL_WaitEventTimeout(&ev, 250) : SDL_PollEvent(&ev);
        while (got) {
            last_activity = SDL_GetTicks();
            switch (ev.type) {
            case SDL_QUIT:
                a->running = false;
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    win_w = ev.window.data1;
                    win_h = ev.window.data2;
                } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                    library_reload_progress(lib);
                    library_request_durations(lib);
                    browse_refresh_local(a);
                }
                break;
            case SDL_TEXTINPUT:
                if (typing_now(a)) on_text(a, ev.text.text);
                break;
            case SDL_KEYDOWN:
                on_key(a, ev.key.keysym.sym, ev.key.keysym.mod);
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) on_click(a, ev.button.x, ev.button.y);
                else if (ev.button.button == SDL_BUTTON_X1 || ev.button.button == SDL_BUTTON_RIGHT)
                    on_key(a, SDLK_ESCAPE, 0);
                break;
            case SDL_MOUSEMOTION:
                a->mouse_x = ev.motion.x;
                a->mouse_y = ev.motion.y;
                if (!a->det.open && !a->src.open && !a->files.open && a->tab != TAB_NYAA &&
                    a->tab != TAB_DOWNLOADS && !search_typing(a))
                    browse_hover(a, ev.motion.x, ev.motion.y);
                break;
            case SDL_MOUSEWHEEL:
                if (ev.wheel.y || ev.wheel.x) {
                    int dx = ev.wheel.x, dy = ev.wheel.y;
                    if (SDL_GetModState() & KMOD_SHIFT) {
                        dx = -dy;
                        dy = 0;
                    }
                    on_wheel(a, dx, dy);
                }
                break;
            default:
                break;
            }
            got = SDL_PollEvent(&ev);
        }

        net_poll();
        playback_poll();
        browse_tick(a);
        binge_tick(a);
        int state = stream_status()->state;
        if (state != last_state) {
            if (state == ST_IDLE) {
                if (stream_status()->error[0] && last_state == ST_CONNECTING)
                    app_toast(a, "Stream failed: %s", stream_status()->error);
                library_reload_progress(lib);
                browse_refresh_local(a);
            }
            last_state = state;
        }
        if (downloads_changed()) {
            library_scan(lib);
            library_request_matches(lib);
            browse_refresh_local(a);
            app_toast(a, "Download finished");
        }
        if (library_take_dirty()) browse_refresh_local(a);

        debug_keys(a, start);
        render(a);
        debug_shot(a, start);
        SDL_RenderPresent(renderer);
        frame_no++;
        gfx_frame_end();
        images_gc();
    }

    SDL_StopTextInput();
    stream_cancel();
    playback_shutdown();
    net_shutdown();
    images_shutdown();
    gfx_shutdown(&a->ui);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
