# cwatch

`cwatch` is a video app for Linux with a Netflix-style interface. It combines
your local library (`~/media` by default) with online catalogs, and it can
stream or download episodes and movies from torrents.

- Home, Shows, Movies and Anime tabs with a large hero banner and poster rows
- Catalog and metadata from Cinemeta (shows and movies) and Kitsu (anime)
- Sources from Torrentio and Nyaa, side by side, with quality, seeders and size
- Stream a torrent straight into `mpv` without waiting for the full download
- Download a single episode, a pick from a batch, or a whole torrent into
  `~/media/<title>`
- Local folders are matched to their show automatically and get posters,
  descriptions and per-episode "on disk" badges
- Continue Watching with resume positions saved by `mpv`
- Nyaa search tab and a Downloads tab with pause and resume

## Dependencies

- C and C++ compilers, `make`, `pkg-config`
- SDL2, SDL2_ttf, SDL2_image
- libcurl, json-c
- libtorrent-rasterbar 2.x (for `cwatch-stream`)
- `mpv` for playback, streaming and resume support
- `ffmpeg` / `ffprobe` for thumbnails and progress bars
- Noto Sans fonts recommended (Noto Sans CJK for Japanese and Chinese titles)

Examples:

- Arch Linux / Artix Linux: `sudo pacman -S --needed git base-devel pkgconf sdl2 sdl2_ttf sdl2_image curl json-c libtorrent-rasterbar mpv ffmpeg noto-fonts noto-fonts-cjk`
- Debian / Ubuntu: `sudo apt install git build-essential pkg-config libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev libcurl4-openssl-dev libjson-c-dev libtorrent-rasterbar-dev mpv ffmpeg fonts-noto-core fonts-noto-cjk`
- Fedora: `sudo dnf install git gcc gcc-c++ make pkgconf-pkg-config SDL2-devel SDL2_ttf-devel SDL2_image-devel libcurl-devel json-c-devel rb_libtorrent-devel mpv ffmpeg google-noto-sans-fonts google-noto-sans-cjk-fonts`
- openSUSE: `sudo zypper install git gcc gcc-c++ make pkg-config libSDL2-devel SDL2_ttf-devel libSDL2_image-devel libcurl-devel libjson-c-devel libtorrent-rasterbar-devel mpv ffmpeg noto-sans-fonts noto-sans-cjk-fonts`
- Gentoo: `sudo emerge --ask dev-vcs/git virtual/pkgconfig media-libs/libsdl2 media-libs/sdl2-ttf media-libs/sdl2-image net-misc/curl dev-libs/json-c net-libs/libtorrent-rasterbar media-video/mpv media-video/ffmpeg media-fonts/noto media-fonts/noto-cjk`
- Alpine: `sudo apk add git build-base pkgconf sdl2-dev sdl2_ttf-dev sdl2_image-dev curl-dev json-c-dev libtorrent-rasterbar-dev mpv ffmpeg font-noto font-noto-cjk`

## Install

```sh
git clone https://github.com/Vifuddyxg/cwatch
cd cwatch
make
sudo make install-desktop
```

`make` builds two programs:

- `cwatch`, the app
- `cwatch-stream`, the torrent helper that streams to `mpv` over local HTTP
  and runs downloads

`install-desktop` installs both to `/usr/local/bin` and `cwatch.desktop` to
`/usr/local/share/applications`. Use `sudo make install` without the desktop
entry. `cwatch` looks for `cwatch-stream` next to its own binary first, then
in `PATH`, so `./cwatch` also works without installing.

```sh
cwatch
cwatch /path/to/media
```

## Layout

```text
~/media/
  Attack on Titan/
    Attack.on.Titan.S01E01.mkv
    Attack.on.Titan.S01E02.mkv
  Some Movie (2020).mkv
```

Each top-level folder is one show or movie. Loose files with the same name
(for example `Show.S04E07.mkv` and `Show.S04E08.mkv`) are grouped together.
Episode numbers come from `S01E02` or ` - 02` in the file name. Downloads go to
`~/media/<title>/`.

State is kept in `~/.local/state/cwatch` (resume positions, watched videos,
matches, history, downloads) and cache in `~/.cache/cwatch`.

## Controls

- Arrow keys: move; Up from the banner reaches the top menu
- `Enter` / click: open, play, or switch tab
- `Esc` / `Backspace` / right click: back
- `/`: search
- `p`: switch player for local files
- `r`: rescan the library
- `d` on Home or My Library: add another media folder

Detail page:

- `Enter`: play the episode (local file if it is on disk, otherwise pick a source)
- `d`: download the episode
- `m`: mark a local episode as watched
- Left / Right: change season

Sources window:

- `Enter`: play, `d`: download
- `Tab`: switch between Torrentio and Nyaa
- `/`: edit the Nyaa search

While playing in `mpv`, a Next Episode button shows up near the end of an
episode (click it or press `n`), and the next episode starts on its own when
one ends, for files on disk and for streams. Audio and subtitle choices are
remembered per show. Finished episodes are marked as watched.

Batch file list: `Enter` play, `d` download file, `a` download all.

Downloads tab: `Space` pause or resume, `Delete` remove from the list.

Streams stop when `cwatch` closes. Unfinished downloads pause and continue the
next time it starts.
