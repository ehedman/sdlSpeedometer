/*
 * extSpeedometer.c
 *
 *  Copyright (C) 2018-2026 by Erland Hedman <erland@hedmanshome.se>
 *
 * Common helper functions for sdlSpeedometer.c
 */
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <libswscale/swscale.h>
#include "fplaydSpeedometer.h"
#include "sdlSpeedometer.h"

void draw_textc(SDL_Renderer *r, TTF_Font *font, const char *txt, int x, int y, int l, SDL_Color c)
{
    if (txt == NULL || !strlen(txt))
        return;

    SDL_Surface *s = TTF_RenderUTF8_Blended(font, txt, c);
    if (!s)
        return;

    SDL_Texture *t = SDL_CreateTextureFromSurface(r, s);
    if (!t) {
        SDL_FreeSurface(s);
        return;
    }

    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);

    int dx = x;

    switch (l) {
        case 1: // Right aligned
            dx -= s->w;
            break;

        case 2: // Center aligned
            dx -= s->w / 2;
            break;

        case 0: // Left aligned
        default:
            break;
    }

    SDL_Rect d = { dx, y, s->w, s->h };

    SDL_RenderCopy(r, t, NULL, &d);

    SDL_FreeSurface(s);
    SDL_DestroyTexture(t);
}


void draw_text(SDL_Renderer *r, TTF_Font *font, const char *txt, int x, int y)
{
    draw_textc(r, font, txt, x, y, 0, (SDL_Color){0,0,0,255});
}

// Prepare for a new playlist - doMedia()
void doVaClean(sdl2_app *sdlApp)
{
    if (!sdlApp->conf->playlist.count) {
        memset(&sdlApp->conf->playlist, 0 , sizeof(Playlist));
        return;
    }

    for (size_t i = 0; i < sdlApp->conf->playlist.count; i++) {
        free(sdlApp->conf->playlist.files[i]);

    if (sdlApp->conf->playlist.track_no[i] > 0 && sdlApp->conf->playlist.titles[i] != NULL)
        {
            //printf("REMOVE=%s %ld\n", sdlApp->conf->playlist.titles[i], i);
            free(sdlApp->conf->playlist.titles[i]);
        }
    }

    free(sdlApp->conf->playlist.files);
    free(sdlApp->conf->playlist.titles);

    memset(&sdlApp->conf->playlist, 0 , sizeof(Playlist));
}

// Plotting aid in doEnvironment()
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

// Text rendering for doVideo()
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

// Radio buttons for doVideo()
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

//  Globael Volume controls
void set_alsa_volume(sdl2_app *sdlApp, float percent)
{
    long volume = sdlApp->conf->snd_minv + (long)((sdlApp->conf->snd_maxv - sdlApp->conf->snd_minv) * percent);

    snd_mixer_selem_set_playback_volume_all(sdlApp->conf->elem, volume);
}

float get_current_volume(sdl2_app *sdlApp)
{
    long volume;
    snd_mixer_selem_get_playback_volume(sdlApp->conf->elem, SND_MIXER_SCHN_FRONT_LEFT, &volume);

    return (double)(volume - sdlApp->conf->snd_minv) / (sdlApp->conf->snd_maxv - sdlApp->conf->snd_minv);
}


// draw text from font matrix - doRecord()
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

// File handling for doRecord()
void load_files(rFileOpts *df_opts) {
    DIR *d = opendir(df_opts->uploadPath);
    struct dirent *dir;
    df_opts->file_count = 0;

    if (d) {
        while ((dir = readdir(d)) != NULL && df_opts->file_count < RMAX_FILES) {
            if (dir->d_name[0] != '.') {
                strncpy(df_opts->file_list[df_opts->file_count], dir->d_name, RMAX_FILENAME);
                df_opts->file_count++;
            }
        }
        closedir(d);
    }
}

// Initilize r/w file operations - doRecord()
void do_file(char *how, rFileOpts *df_opts)
{

    if (df_opts->cnm->rwMode && df_opts->cnm->pbFile != NULL) {
        return;
    }

    char filePath[RMAX_FILENAME];
    static char statusMsg[200];
    SDL_Thread *threadPLB = NULL;

    size_t len = strlen(df_opts->current_input);
    if (len > 0) {

        df_opts->cnm->rwMode = how[0] == 'r'? 1 : 2;
        snprintf(filePath, sizeof(filePath), "%s/%s", df_opts->uploadPath, df_opts->current_input);

        df_opts->cnm->pbFile = fopen(filePath, how);
        if (df_opts->cnm->pbFile != NULL) {
            if (df_opts->cnm->rwMode == 1) {    // Read
                df_opts->configParams->runPlb = 1;
                threadPLB = SDL_CreateThread(df_opts->threadPLB, "threadPLB", df_opts->configParams);
                if (NULL == threadPLB) {
                    df_opts->configParams->runPlb = 0;
                    df_opts->cnm->rwMode = 0;
                    snprintf(statusMsg, sizeof(statusMsg), "SDL_CreateThread threadPLB failed: %s", SDL_GetError());
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", statusMsg);
                } else {
                    SDL_DetachThread(threadPLB);
                }
            }
            snprintf(statusMsg, sizeof(statusMsg), "Running NMEA file: %s(%c)", filePath, how[0]);
        } else {
            snprintf(statusMsg, sizeof(statusMsg), "Open %s (%s) failed: %s", filePath, how, SDL_GetError());
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, statusMsg);
            df_opts->cnm->rwMode = 0;
            df_opts->configParams->runPlb = 0;
        }
        load_files(df_opts);
    }
}

// Replacement for SDL_RenderDrawLine that supports arbitrary thickness - doGps()
void SDL_RenderDrawLineThick(SDL_Renderer* renderer, float x1, float y1, float x2, float y2, float thickness)
{
    // Calculate the angle and direction of the line
    float dx = x2 - x1;
    float dy = y2 - y1;
    float length = sqrtf(dx * dx + dy * dy);

    if (length == 0.0f) return; // Avoid division by zero

    // Calculate a normal vector (90 degrees to the line) with length thickness / 2
    float nx = -dy / length * (thickness / 2.0f);
    float ny = dx / length * (thickness / 2.0f);

    // Create the 4 corners for the rectangle that forms the thick line
    SDL_Vertex vertices[4];

    // Get the current color the renderer is set to
    Uint8 r, g, b, a;
    SDL_GetRenderDrawColor(renderer, &r, &g, &b, &a);
    SDL_Color color = { r, g, b, a };

    // Corners 1 & 2 (The starting point)
    vertices[0].position.x = x1 + nx; vertices[0].position.y = y1 + ny; vertices[0].color = color;
    vertices[1].position.x = x1 - nx; vertices[1].position.y = y1 - ny; vertices[1].color = color;

    // Corners 3 & 4 (The End Point)
    vertices[2].position.x = x2 + nx; vertices[2].position.y = y2 + ny; vertices[2].color = color;
    vertices[3].position.x = x2 - nx; vertices[3].position.y = y2 - ny; vertices[3].color = color;

    // Indices to connect the vertices into 2 triangles (0-1-2 and 1-2-3)
    int indices[6] = { 0, 1, 2, 1, 2, 3 };

    // Draw the thick line on the screen (hardware-accelerated)
    SDL_RenderGeometry(renderer, NULL, vertices, 4, indices, 6);
}

// Helper function for drawing a simple arrow (vector) in SDL2 - doGps()
void draw_vector_arrow(SDL_Renderer* renderer, int start_x, int start_y, int end_x, int end_y, int arrow_size)
{
    // Draw the main line of the vector yourself.
    SDL_RenderDrawLineThick(renderer, start_x, start_y, end_x, end_y, 3);

    // Calculate the angle of the line to be able to draw the arrowhead pointing backwards.
    float angle = atan2f(end_y - start_y, end_x - start_x);

    // The two rear corners of the arrowhead
    int arrow_x1 = end_x - arrow_size * cosf(angle - (float)M_PI / 6);
    int arrow_y1 = end_y - arrow_size * sinf(angle - (float)M_PI / 6);
    int arrow_x2 = end_x - arrow_size * cosf(angle + (float)M_PI / 6);
    int arrow_y2 = end_y - arrow_size * sinf(angle + (float)M_PI / 6);

    SDL_RenderDrawLineThick(renderer, end_x, end_y, arrow_x1, arrow_y1, 3);
    SDL_RenderDrawLineThick(renderer, end_x, end_y, arrow_x2, arrow_y2, 3);
}

/**
 * Renders the Vector Triangle (Current Navigation) inside the compass rose.
 *
 * This function dynamically draws a Vector Triangle (Current Triangle) to visually
 * represent the environmental forces acting on the vessel:
 *
 * - Heading Vector (Blue): Represents Heading (HDG) and Speed Through Water (STW).
 *   Shows where the bow is pointing and the boat's speed relative to the water.
 *
 * - Current Vector (Red): Represents the Set (SET) and Drift (DFT).
 *   Displays the direction the current is flowing toward and its speed.
 *
 * - Course Vector (Green): The resultant vector representing Course Over Ground (COG)
 *   and Speed Over Ground (SOG). This is the actual track and speed relative to the earth.
 *
 * - Drift Angle (CRX): Displays the angular difference between where the boat is
 *   heading and where it is actually moving (Heading vs COG) to Port (PRT) or Starboard (STB).
 */

// Drawing the power triangle in a nautical style - doGps()
void draw_vector_triangle(SDL_Renderer* renderer, int center_x, int center_y, int radius,
                        float hdg, float stw, float cog, float sog, float set, float dft)
{
    // Scaling factor: Determines how many pixels correspond to 1 knot.
    // We base this on the assumption that the maximum speed (e.g., SOG or STW) should fill approximately 80% of the radius.
    float max_speed = (stw > sog) ? stw : sog;
    if (max_speed < 1.0f) max_speed = 1.0f; // Avoid division by zero
    float scale = (radius * 0.85f) / max_speed;

    //--- PROJECT THE VECTORS ONTO SCREEN COORDINATES ---
    // Remember: 0° North is straight up (-Y in SDL), East is right (+X).

    // Water vector (HDG & STW) – Starts at the center
    float hdg_rad = hdg * ((float)M_PI / 180.0f);
    int water_end_x = center_x + (int)(stw * scale * sinf(hdg_rad));
    int water_end_y = center_y - (int)(stw * scale * cosf(hdg_rad)); // Minus pga SDL Y-axel

    // Ground Vector (COG & SOG) – Also starts at the center.
    float cog_rad = cog * ((float)M_PI / 180.0f);
    int ground_end_x = center_x + (int)(sog * scale * sinf(cog_rad));
    int ground_end_y = center_y - (int)(sog * scale * cosf(cog_rad));

    // --- 2. DRAW THE VECTORS (CURRENT TRIANGLE) ---
	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // Blue Vector: WATER (Heading & Speed ​​through the water)
    SDL_SetRenderDrawColor(renderer, 52, 152, 219, 180); // Blue
    draw_vector_arrow(renderer, center_x, center_y, water_end_x, water_end_y, 20);

    // Green Vector: GROUND (COG & SOG over ground)
    SDL_SetRenderDrawColor(renderer, 46, 204, 113, 180); // Green
    draw_vector_arrow(renderer, center_x, center_y, ground_end_x, ground_end_y, 20);

    // Red vector: SET & DRIFT (Leeway/Current)
    // This vector connects the end of the water vector to the end of the ground vector.
    SDL_SetRenderDrawColor(renderer, 231, 76, 60, 180); // Red
    draw_vector_arrow(renderer, water_end_x, water_end_y, ground_end_x, ground_end_y, 12);

    // Draw a small dot in the center of the instrument
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_Rect center_dot = { center_x - 2, center_y - 2, 4, 4 };
    SDL_RenderFillRect(renderer, &center_dot);
}

float to_radians(float degrees) { return degrees * ((float)M_PI / 180.0f); }
float to_degrees(float radians) { return radians * (180.0f / (float)M_PI); }

// Function to normalize angles to 0–360 degrees - doGps()
float normalize_degrees(float deg) {
    deg = fmodf(deg, 360.0f);
    if (deg < 0.0f) deg += 360.0f;
    return deg;
}

// Vector calculation - - doGps()
Vector2D nav_to_vector(float course_deg, float speed) {
    Vector2D vec;
    float rad = to_radians(course_deg);
    vec.x = speed * sinf(rad);
    vec.y = speed * cosf(rad);
    return vec;
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
           has_extension(name, ".mpg")  ||
           has_extension(name, ".mkv")  ||
           has_extension(name, ".wma")  ||
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
    char *path_copy = strdup(path);

    if (!path_copy)
        return -1;

    char **temp_paths = realloc(
        list->paths,
        (list->count + 1) * sizeof(*list->paths)
    );

    if (!temp_paths) {
        free(path_copy);
        return -1;
    }

    list->paths = temp_paths;

    SDL_Texture **temp_cover = realloc(
        list->cover,
        (list->count + 1) * sizeof(*list->cover)
    );

    if (!temp_cover) {
        free(path_copy);
        return -1;
    }

    list->cover = temp_cover;

    list->paths[list->count] = path_copy;

    list->cover[list->count] =
        IMG_LoadTexture(list->renderer, path_copy);

    // Texture may be NULL if image loading fails.
    // The path is still retained in the list.

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

// Recursive directory scanning - scan_cover_art()
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
        if (strcmp(entry->d_name, ".")  == 0 ||
            strcmp(entry->d_name, "..") == 0 ||
            strchr(entry->d_name, '$') /* windows/smb */ ) {
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

// Media progress bar - doMedia()
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

// get_video_clock time stamps - doMedia();
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

// clean up for doMedia()
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

// Aspect calculation for doMedia()
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

// Update a video frame - doMedia()
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

