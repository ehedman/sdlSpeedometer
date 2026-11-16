/*
 * extSpeedometer.c
 *
 *  Copyright (C) 2018-2026 by Erland Hedman <erland@hedmanshome.se>
 */
#include <SDL2/SDL.h>
#include <dirent.h>
#include <sys/stat.h>
#include <libswscale/swscale.h>
#include "fplaydSpeedometer.h"
#include "sdlSpeedometer.h"

void draw_textc(SDL_Renderer *r,TTF_Font *font,const char *txt,int x,int y, SDL_Color c)
{
    if (txt == NULL || !strlen(txt))
        return;

    SDL_Surface *s=TTF_RenderUTF8_Blended(font,txt,c);
    SDL_Texture *t=SDL_CreateTextureFromSurface(r,s);

    SDL_Rect d={x,y,s->w,s->h};

    SDL_RenderCopy(r,t,NULL,&d);

    SDL_FreeSurface(s);
    SDL_DestroyTexture(t);
}

void draw_text(SDL_Renderer *r, TTF_Font *font, const char *txt, int x, int y)
{

    if (txt == NULL || !strlen(txt))
        return;

    SDL_Color c = {220,220,220,255};

    SDL_Surface *s = TTF_RenderUTF8_Blended(font, txt, c);
    SDL_Texture *t = SDL_CreateTextureFromSurface(r, s);

    SDL_Rect d = {x,y,s->w,s->h};

    SDL_RenderCopy(r,t,NULL,&d);

    SDL_FreeSurface(s);
    SDL_DestroyTexture(t);
}


void doVaClean(sdl2_app *sdlApp)
{
    if (!sdlApp->conf->playlist.count) return;
    for (size_t i = 0; i < sdlApp->conf->playlist.count; i++) {
        free(sdlApp->conf->playlist.files[i]);
    }
    free(sdlApp->conf->playlist.files);
    sdlApp->conf->playlist.count = 0;
}

void draw_text_r(SDL_Renderer* renderer, const char* text, int x, int y, int scale, const unsigned char font8x8[][8]) {
    SDL_Renderer *r = renderer;
    for (int i = 0; text[i] != '\0'; i++) {
        char c = text[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        for (int row = 0; row < 8; row++) {
            for (int col = 0; col < 8; col++) {
                if ((font8x8[(int)c][row] >> (7 - col)) & 1) {
                    SDL_Rect pixel = { x + (i * 8 * scale) + (col * scale), y + (row * scale), scale, scale };
                    SDL_RenderFillRect(r, &pixel);
                }
            }
        }
    }
}
float auto_scale(float maxDepth)
{
    int scale = 10;

    while (scale < maxDepth)
        scale += 10;

    return scale;
}

float auto_scale_p(float maxVal)
{
    if(maxVal <= 50) return 50;
    if(maxVal <= 500) return 500;
    return 1000;
}

SDL_Texture* renderText(SDL_Renderer *r, TTF_Font *font, const char *msg,
                        SDL_Color color, SDL_Rect *rect)
{
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, msg, color);
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);

    rect->w = surf->w;
    rect->h = surf->h;

    SDL_FreeSurface(surf);
    return tex;
}

void drawRadio(SDL_Renderer *r, RadioButton *b, int selected)
{
    SDL_RenderDrawRect(r, &b->circle);

    if(selected)
    {
        SDL_Rect dot = {
            b->circle.x + 5,
            b->circle.y + 5,
            b->circle.w - 10,
            b->circle.h - 10
        };
        SDL_RenderFillRect(r, &dot);
    }

    SDL_RenderCopy(r, b->text, NULL, &b->text_rect);
}


// Check file extension, case-insensitive
static int has_extension(const char *name, const char *ext)
{
    size_t name_len = strlen(name);
    size_t ext_len  = strlen(ext);

    if (name_len < ext_len)
        return 0;

    return strcasecmp(name + name_len - ext_len, ext) == 0;
}

// Check if the file is a cover image
static int is_image_file(const char *name)
{
    return has_extension(name, ".png") ||
           has_extension(name, ".jpg");
}

// Check whether the file is an av file
static int is_audio_file(const char *name)
{
    return has_extension(name, ".mp3")  ||
           has_extension(name, ".flac") ||
           has_extension(name, ".avi")  ||
           has_extension(name, ".mp4")  ||
           has_extension(name, ".m4a")  ||
           has_extension(name, ".VOB")  ||
           has_extension(name, ".wav");
}

// Create full path
static char *make_path(const char *dir, const char *name)
{
    size_t dir_len  = strlen(dir);
    size_t name_len = strlen(name);

    int add_slash = (dir_len > 0 && dir[dir_len - 1] != '/');

    char *path = malloc(dir_len + add_slash + name_len + 1);

    if (!path)
        return NULL;

    memcpy(path, dir, dir_len);

    if (add_slash)
        path[dir_len++] = '/';

    memcpy(path + dir_len, name, name_len + 1);

    return path;
}

// Add path to the results list
static int coverlist_add(CoverList *list, const char *path)
{
    char **temp = realloc(list->paths, (list->count + 1) * sizeof(*list->paths));

    if (!temp)
        return -1;

    list->paths = temp;

    // Expand the array using a pointer
    list->paths[list->count] = strdup(path);

    if (!list->paths[list->count])
        return -1;

    // Reallocate memory for the textures (cover)
    SDL_Texture **temp_covers = realloc(list->cover, (list->count + 1) * sizeof(*list->cover));
    if (!temp_covers) {
        free(list->paths[list->count]);
        return -1;
    }
    list->cover = temp_covers;


    list->cover[list->count] = IMG_LoadTexture(list->renderer, list->paths[list->count]);
    if (!list->cover[list->count]) {
        list->cover[list->count] = NULL;
    }

    list->cover[list->count] = IMG_LoadTexture(list->renderer, list->paths[list->count]);

    list->count++;

    return 0;
}

// Detach the results list
void coverlist_free(CoverList *list)
{
    if (!list)
        return;

    for (size_t i = 0; i < list->count; i++)
    {
        if (list->cover && list->cover[i]) {
            SDL_DestroyTexture(list->cover[i]);
        }

        if (list->paths && list->paths[i]) {
            free(list->paths[i]);
        }
    }

    free(list->paths);
    free(list->cover);

    list->paths = NULL;
    list->cover = NULL;
    list->count = 0;
}

// Recursive directory scanning
int scan_directory(const char *base_path, CoverList *list)
{
    DIR *dir = opendir(base_path);

    if (!dir) {
        perror(base_path);
        return -1;
    }

    struct dirent *entry;

    char **images = NULL;
    size_t image_count = 0;

    int audio_found = 0;
    int result = 0;

    while ((entry = readdir(dir)) != NULL) {

        // Skip "." and ".."
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char *full_path = make_path(base_path, entry->d_name);

        if (!full_path) {
            result = -1;
            break;
        }

        struct stat st;

        // lstat ensures that we do not follow symbolic links
        if (lstat(full_path, &st) != 0) {
            free(full_path);
            continue;
        }

        // Recursion into subdirectories
        if (S_ISDIR(st.st_mode)) {

            if (scan_directory(full_path, list) != 0)
                result = -1;

            free(full_path);

            if (result != 0)
                break;

            continue;
        }

        // Ignore everything that is not a regular file
        if (!S_ISREG(st.st_mode)) {
            free(full_path);
            continue;
        }

        // Find audio files in the current directory
        if (is_audio_file(entry->d_name)) {
            audio_found = 1;
        }

        // Collect image files in the current directory
        if (is_image_file(entry->d_name) && image_count < 1) {

            char **temp = realloc(images,(image_count + 1) * sizeof(*images));

            if (!temp) {
                free(full_path);
                result = -1;
                break;
            }

            images = temp;
            images[image_count++] = full_path;

        } else {
            free(full_path);
        }
    }

    closedir(dir);

    // Add the images only if the catalog contains audio
    if (result == 0 && audio_found) {

        for (size_t i = 0; i < image_count; i++) {

            if (coverlist_add(list, images[i]) != 0) {
                result = -1;
                break;
            }
        }
    }

    // Release local image paths
    for (size_t i = 0; i < image_count; i++)
        free(images[i]);

    free(images);

    return result;
}

void draw_progress_bar(SDL_Renderer *renderer,
                       int x, int y, int width, int height,
                       double position, double duration)
{
    if (duration <= 0.0)
        return;

    // Restrict the position to a valid range
    if (position < 0.0)
        position = 0.0;

    if (position > duration)
        position = duration;

    double ratio = position / duration;
    int progress_width = (int)(width * ratio);

    SDL_Rect background = { x, y, width, height };
    SDL_Rect progress   = { x, y, progress_width, height };

    // Background
    SDL_SetRenderDrawColor(renderer, 35, 35, 35, 255);
    SDL_RenderFillRect(renderer, &background);

    // Time played
    SDL_SetRenderDrawColor(renderer, 235, 235, 235, 255);

    if (progress_width > 0)
        SDL_RenderFillRect(renderer, &progress);

    // Marker at current position
    SDL_Rect marker = {
        x + progress_width - 2,
        y - 3,
        4,
        height + 6
    };

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);

    if (progress_width > 0)
        SDL_RenderFillRect(renderer, &marker);

    // Thin frame
    SDL_SetRenderDrawColor(renderer, 20, 50, 75, 255);
    SDL_RenderDrawRect(renderer, &background);
}

static Uint64 video_start_ticks = 0;

double get_video_clock(void)
{
    double aclock = get_audio_clock();
    if (aclock > 0.001) {
        video_start_ticks = 0;
        return aclock; // The sound clock is ticking – use it!
    }


    // Fallback timer for cases where audio has not started or lacks `played_bytes`:
    if (video_start_ticks == 0) {
        video_start_ticks = SDL_GetTicks64();
    }
    return (double)(SDL_GetTicks64() - video_start_ticks) / 1000.0;
}

void video_renderer_cleanup(VideoRenderer *vr)
{
    if (vr->yuv_frame) {
        av_frame_free(&vr->yuv_frame);
    }
    if (vr->sws_ctx) {
        sws_freeContext(vr->sws_ctx);
        vr->sws_ctx = NULL;
    }
    if (vr->texture) {
        SDL_DestroyTexture(vr->texture);
        vr->texture = NULL;
    }
}

SDL_Rect calculate_aspect_rect(SDL_Rect box, int video_w, int video_h)
{
    SDL_Rect dst = box;
    if (video_w <= 0 || video_h <= 0) {
        return dst;
    }

    float box_aspect   = (float)box.w / (float)box.h;
    float video_aspect = (float)video_w / (float)video_h;

    if (video_aspect > box_aspect) {
        // The video is wider than the frame (black bars at the top/bottom)
        dst.w = box.w;
        dst.h = (int)(box.w / video_aspect);
        dst.x = box.x;
        dst.y = box.y + (box.h - dst.h) / 2;
    } else {
        // The video is narrower/taller than the frame (black bars on the left/right)
        dst.h = box.h;
        dst.w = (int)(box.h * video_aspect);
        dst.x = box.x + (box.w - dst.w) / 2;
        dst.y = box.y;
    }

    return dst;
}

void update_video_texture(SDL_Renderer *renderer, VideoRenderer *vr,
                          VideoQueue *q, AVRational time_base, double audio_clock)
{
    AVFrame *vframe = NULL;
    double pts;

    // Skip invalid boxes and all late boxes except for the very latest one
    while ((vframe = video_queue_peek(q)) != NULL) {
        if (vframe->width <= 0 || vframe->height <= 0 || vframe->data[0] == NULL || vframe->linesize[0] <= 0) {
            video_queue_pop(q);
            continue;
        }

        pts = (vframe->pts != AV_NOPTS_VALUE) ? vframe->pts * av_q2d(time_base) : 0.0;
        if (pts > audio_clock) {
            return; //The next frame lies in the future; keep the current image.
        }

        // Check if there is already YET ANOTHER box in the queue that is also ready to be displayed
        // (This requires the queue to have at least 2 boxes remaining)
        if (q->count > 1) {
            int next_idx = (q->head + 1) % VIDEO_QUEUE_SIZE;
            AVFrame *next_frame = q->frames[next_idx];
            if (next_frame && next_frame->pts != AV_NOPTS_VALUE) {
                double next_pts = next_frame->pts * av_q2d(time_base);
                if (next_pts <= audio_clock) {
                    // The next frame should already be displayed! Discard the current one without drawing it.
                    video_queue_pop(q);
                    continue;
                }
            }
        }

        // Otherwise, vframe is the exact frame that should be displayed right now!
        break;
    }

    if (!vframe) return;

    // Create/update texture and sws_ctx as needed
    if (!vr->texture || !vr->yuv_frame ||
        vr->yuv_frame->width != vframe->width ||
        vr->yuv_frame->height != vframe->height)
    {
        video_renderer_cleanup(vr);

        int w = vframe->width;
        int h = vframe->height;
        vr->texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV,
                                        SDL_TEXTUREACCESS_STREAMING, w, h);
        vr->sws_ctx = sws_getContext(w, h, (enum AVPixelFormat)vframe->format,
                                     w, h, AV_PIX_FMT_YUV420P,
                                     SWS_BILINEAR, NULL, NULL, NULL);
        vr->yuv_frame = av_frame_alloc();
        vr->yuv_frame->format = AV_PIX_FMT_YUV420P;
        vr->yuv_frame->width  = w;
        vr->yuv_frame->height = h;
        av_frame_get_buffer(vr->yuv_frame, 32);
    }

    // Fast track if the video is already YUV420P (saves a lot of CPU)
    if (vframe->format == AV_PIX_FMT_YUV420P || vframe->format == AV_PIX_FMT_YUVJ420P) {
        SDL_UpdateYUVTexture(
            vr->texture,
            NULL,
            vframe->data[0], vframe->linesize[0],
            vframe->data[1], vframe->linesize[1],
            vframe->data[2], vframe->linesize[2]
        );
    } else {
        av_frame_make_writable(vr->yuv_frame);
        if (sws_scale(vr->sws_ctx, (const uint8_t * const *)vframe->data, vframe->linesize,
                      0, vframe->height, vr->yuv_frame->data, vr->yuv_frame->linesize) > 0)
        {
            SDL_UpdateYUVTexture(
                vr->texture,
                NULL,
                vr->yuv_frame->data[0], vr->yuv_frame->linesize[0],
                vr->yuv_frame->data[1], vr->yuv_frame->linesize[1],
                vr->yuv_frame->data[2], vr->yuv_frame->linesize[2]
            );
        }
    }

    video_queue_pop(q);
}

