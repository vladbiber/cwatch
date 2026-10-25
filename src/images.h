#ifndef CWATCH_IMAGES_H
#define CWATCH_IMAGES_H

#include "common.h"

void images_init(SDL_Renderer *r);
void images_shutdown(void);
SDL_Texture *img_get(const char *url);
SDL_Texture *img_thumb(const char *video_path);
void img_prefetch(const char *url);
void images_gc(void);

#endif
