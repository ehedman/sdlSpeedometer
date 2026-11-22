
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>

#include <SDL2/SDL.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include "sdlSpeedometer.h"
#include "fplaydSpeedometer.h"

/* ---------------------------------------------------------------- */
/* Find track number based on a file name i.e "02 Dinah-Moe Humm"   */
/* ---------------------------------------------------------------- */

static void get_track_from_filename(TrackItem *tmeta)
{
    const char *p = tmeta->filename;
    char *e = strrchr(tmeta->filename, '.');

    if (e != NULL)  // Remove extension from filename
        *e = '\0';

    if ((p = strrchr(tmeta->filename, '/')) != NULL) {
            p++;    // Point to filename only
    }

    // Skip any leading spaces or non-alphanumeric characters
    while (*p && !isdigit((unsigned char)*p) && !isalpha((unsigned char)*p) && *p == ' ') {
        p++;
    }

    // If the string begins with a digit, read it out.
    if (*p && isdigit((unsigned char)*p)) {

        tmeta->track_no = atoi(p);   // atoi reads digits until it encounters a space, hyphen, or period.

        // Move beyond digits and leading spaces efter digits
        while (*p && (isdigit((unsigned char)*p) || *p == ' ' || !isalpha((unsigned char)*p))) {
            p++;
        }
    }

    if (!tmeta->track_no)
        tmeta->track_no = tolower(p[0]);    // Alpha numeric on first letter

    snprintf(tmeta->title, tmeta->maxb, "%s", p);
}

/* --------------------------------------------------- */
/* Find track title and track number number from meta  */
/* --------------------------------------------------- */

static void get_plist_title(TrackItem *tmeta)
{
    memset(tmeta->title, 0, tmeta->maxb);
    tmeta->track_no = 0;

    struct stat stats;
    stat(tmeta->filename, &stats);
    if (S_ISDIR(stats.st_mode)) {
        printf("Should not happen\n");
        return;
    }

    const AVInputFormat *ifmt = NULL;
    char *p = strrchr(tmeta->filename, '.');

    if (p != NULL) {
        p++;
        if (!strcasecmp(p, "avi") || !strcasecmp(p, "vob") || !strcasecmp(p, "mpg")) {
             goto notfound;
        }
        // Give FFmpeg a direct hint about the format to avoid format probing.
        if (!strcasecmp(p, "wav") || !strcasecmp(p, "flac") || !strcasecmp(p, "mp3")) {
            ifmt = av_find_input_format(p);
        }
    }

    AVFormatContext *fmt_ctx = avformat_alloc_context();
    if (!fmt_ctx) {
        goto notfound;
    }

    // Skip unnecessary loading/index building upon opening
    fmt_ctx->flags |= AVFMT_FLAG_NOBUFFER | AVFMT_FLAG_FAST_SEEK;
    fmt_ctx->probesize = 4096;
    fmt_ctx->max_analyze_duration = 0;

    // Pass along ifmt (if found, otherwise NULL)
    if (avformat_open_input(&fmt_ctx, tmeta->filename, ifmt, NULL) < 0) {
        goto notfound;
    }

    AVDictionary *meta = fmt_ctx->metadata;
    if (!meta && fmt_ctx->nb_streams > 0) {
        meta = fmt_ctx->streams[0]->metadata;
    }

    if (meta) {
        const AVDictionaryEntry *title_tag = av_dict_get(meta, "title", NULL, 0);
        if (title_tag && title_tag->value) {
            snprintf(tmeta->title, tmeta->maxb, "%s", title_tag->value);
        }

        const AVDictionaryEntry *track_tag = av_dict_get(meta, "track", NULL, 0);
        if (track_tag && track_tag->value) {
            tmeta->track_no = atoi(track_tag->value);
        }
    }

    avformat_close_input(&fmt_ctx);

notfound:

    if (!tmeta->track_no) {
        get_track_from_filename(tmeta);
    }

//    printf("FOUND '%s'   %d\n", tmeta->title, tmeta->track_no);

}

// Comparison function for qsort
static int compare_tracks(const void *a, const void *b) {
    return ((TrackItem*)a)->track_no - ((TrackItem*)b)->track_no;
}

/* --------------------------------------------------------- */
/* Build a list with folders, track titles and track numbers */
/* --------------------------------------------------------- */

int scandirs(Playlist *plist)
{
    int folder_count = 0;
    char q_path[PATH_MAX];

    TrackItem tmeta;

    struct stat stats;

    DIR *dir;

    dir = opendir(plist->path);

    if (dir == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "doMedia(scandirs): Could not open \"%s\": %s", plist->path, SDL_GetError());
        return -1;
    }

    struct dirent *entry;

    int artFound = 0;

    while ((entry = readdir(dir)) != NULL) {

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        snprintf(q_path, sizeof(q_path), "%s%s", plist->path, entry->d_name);

        if (lstat(q_path, &stats)) {
            continue;
        }

        char *p = strrchr(entry->d_name, '.');
    
        if (p != NULL) {
            p++;
            if (!strcmp(p, "jpg") || !strcmp(p, "png")) {
                    if (plist->apath[0] != '\0' && !artFound) {
                        strcat(plist->apath, entry->d_name);
                        artFound = 1;       
                    }
                  continue;
            }

            if (   strcmp(p, "flac")    != 0
                && strcmp(p, "mp3")     != 0
                && strcmp(p, "wav")     != 0
                && strcmp(p, "avi")     != 0
                && strcmp(p, "mp4")     != 0
                && strcmp(p, "m4a")     != 0
                && strcmp(p, "mpg")     != 0
                && strcmp(p, "mkv")     != 0
                && strcmp(p, "VOB")     != 0)
                {
                    continue;   // Ignore other file types completely
                }
        }

        // Expand the array using a pointer
        char **tempf = realloc(plist->files,  (plist->count + 1) * sizeof(*plist->files));
        char **tempt = realloc(plist->titles, (plist->count + 1) * sizeof(*plist->titles));

        if (tempf == NULL || tempt == NULL) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "doMedia(scandirs): Could not allocate with  realloc() %s", SDL_GetError());
            goto cleanup;
        }

        plist->files = tempf;
        plist->titles = tempt;

        // Copy the filename
        plist->files[plist->count] = strdup(entry->d_name);

        stat(q_path, &stats);
        if (S_ISDIR(stats.st_mode)) {
            plist->files[plist->count] = strdup(entry->d_name); //NEW
            folder_count++;
        }

        plist->titles[plist->count] = NULL; // NEW
        plist->track_no[plist->count] = 0;  // NEW

        // Copy the title
        char ttl[100];
        if (!S_ISDIR(stats.st_mode)) {

            tmeta.filename = q_path;
            tmeta.maxb = sizeof(ttl);
            tmeta.title = ttl;

            plist->track_no[plist->count] = 0;
            get_plist_title(&tmeta);

            if (tmeta.track_no > 0) {
                if ((plist->titles[plist->count] = strdup(tmeta.title)) == NULL) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "doMedia(scandirs titles): Could not allocate with  strdup() %s", SDL_GetError());
                    goto cleanup;
                }
                plist->track_no[plist->count] = tmeta.track_no;
            }
        }

        *ttl = '\0';

        if (plist->files[plist->count] == NULL) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "doMedia(scandirs files): Could not allocate with  strdup() %s", SDL_GetError());
            goto cleanup;
        }

        plist->count++;
    }

    closedir(dir);
    dir = NULL;

/* ------------------------------------- */
/*            Sorting section            */
/* ------------------------------------- */

    // Allocate a temporary array for the sorting, based on the number of files.
    TrackItem *sort_list = malloc(sizeof(TrackItem) * plist->count);

    if (!sort_list)
        goto cleanup;

    int valid_track_count = 0;

    for (int i = 0; i < plist->count; i++) {

        snprintf(q_path, sizeof(q_path), "%s%s", plist->path, plist->files[i]);

        int track_no = -1;

        stat(q_path, &stats);

        if (plist->track_no[i] <= 0 && !S_ISDIR(stats.st_mode)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "doMedia(enum tracks): Fatal: Can't find title and track number");
            goto cleanup;
        } else { track_no = plist->track_no[i]; }

        // Simple sort. Pick the first letter a-z-A-Z - usually the artist or album name
        if (S_ISDIR(stats.st_mode)) {
            track_no = tolower(plist->files[i][0]);
        }

        if (track_no > 0) {
            // Move the pointer to the filename and track number in our sorting list
            sort_list[valid_track_count].filename = plist->files[i];
            sort_list[valid_track_count].title = plist->titles[i];
            sort_list[valid_track_count].track_no = track_no;
            valid_track_count++;
        }
    }

    // Sort the list using qsort
    qsort(sort_list, valid_track_count, sizeof(TrackItem), compare_tracks);

    // Overwrite your original plist->files with the new, sorted order
    // (We are simply copying the pointers back, which is lightning-fast)
    for (int i = 0; i < valid_track_count; i++) {
        plist->files[i] = sort_list[i].filename;
        plist->titles[i] = sort_list[i].title;
        plist->track_no[i] = sort_list[i].track_no; // NEW
    }

    // Update the counter in case any files were filtered out (e.g., files without track numbers)
    if (plist->count != valid_track_count) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "doMedia(sortdirs): Fatal: Corrupted track count =%d : Should be=%d",
            (int)valid_track_count, (int)plist->count);
        valid_track_count = plist->count;
    }
    plist->count = valid_track_count;

    // Free up temporary memory
    free(sort_list);

#if 0
    printf("\n--- After sorting ---\n");
    for (int i = 0; i < plist->count; i++) {
        if (plist->track_no[i] > 0 && plist->titles[i] != NULL) {
            printf("    title %d: title=%s\n", plist->track_no[i], plist->titles[i]);
        }
    }
#endif

    return 0;

cleanup:
    if (dir != NULL)
        closedir(dir);

    if (plist->files != NULL) {
        // Release all strings
        for (size_t i = 0; i < plist->count; i++) {

            free(plist->files[i]);

            if (plist->track_no[i] > 0)
                free(plist->titles[i]);
        }

        free(plist->files);
        free(plist->titles);
    }

    return 1;
}

/* ------------------------------------------------------------------------ */
/*                             Player Section                               */
/* ------------------------------------------------------------------------ */

static AudioBuffer audio;
static CommandQueue commands;
static SDL_AudioDeviceID audio_device;
static float seek_target_seconds;
static AVFormatContext  *g_fmt_ctx;

RTSPAudioBuffer rtsp_audio;
DecoderJob *g_current_job = NULL;
SDL_mutex *g_job_mutex = NULL;
DecoderJob *job = NULL;
VideoRenderer vr = {0};

/* ---------------------------------------------------------------- */
/* Initialize things to start with                                  */
/* ---------------------------------------------------------------- */

static void init_rtsp_audio(AudioProps *apropps)
{
    memset(&rtsp_audio, 0, sizeof(RTSPAudioBuffer));

    if ((rtsp_audio.data = malloc(RTSP_RING_SIZE)) == NULL) {
        rtsp_audio.enabled = 0; rtsp_audio.muted = 1;
        return;
    }

    rtsp_audio.enabled = 1; // Activate the stream
    rtsp_audio.muted = 1;   // Make sure it is muted until user wants it

    // Mutex used in the callback and FFmpeg thread.
    rtsp_audio.mutex = apropps->commands.rtsp_mutex;
}

/* ------------------------------------------------------------------------ */
/*                             Public Section                               */
/* ------------------------------------------------------------------------ */
void player_init(AudioProps *apropps)
{
    audio = apropps->audio;
    commands = apropps->commands;
    audio_device = apropps->audio_device;
    init_rtsp_audio(apropps);
}

float player_get_cur_pos_sec(void)
{
    return audio.cur_pos_sec;
}

void player_set_cur_pos_sec(const float s)
{
    seek_target_seconds = s;
}

int media_is_video(void)
{
    return audio.is_video;
}

/* -------------------------------------------------- */
/* Command FIFO                                       */
/* -------------------------------------------------- */

int command_push(PlayerCommand cmd)
{
    SDL_LockMutex(commands.mutex);

    if (commands.count >= COMMAND_QUEUE)
    {
        SDL_UnlockMutex(commands.mutex);
        return -1;
    }

    commands.queue[commands.write_pos] = cmd;

    commands.write_pos =
        (commands.write_pos + 1) % COMMAND_QUEUE;

    commands.count++;

    SDL_CondSignal(commands.cond);

    SDL_UnlockMutex(commands.mutex);

    return 0;
}

/* ------------------------------------------------------------------------ */
/*                            Decoder Section                               */
/* ------------------------------------------------------------------------ */

/* -------------------------------------------------- */
/* PCM ringbuffer                                     */
/* -------------------------------------------------- */

static void ring_clear(void)
{
    SDL_LockMutex(audio.mutex);

    audio.read_pos = 0;
    audio.write_pos = 0;
    audio.count = 0;
    audio.played_bytes = 0;
    audio.audio_start_set = 0;
    audio.audio_start_sec = 0.0;
    audio.eof = 0;

    SDL_UnlockMutex(audio.mutex);
}

static int ring_write(DecoderJob *job,
                      uint8_t *data, int len)
{
    int written = 0;

    while (written < len &&
           !SDL_AtomicGet(&job->cancel))
    {
        SDL_LockMutex(audio.mutex);

        int free_space = RING_SIZE - audio.count;

        if (free_space > 0)
        {
            int n = len - written;

            if (n > free_space)
                n = free_space;

            int first = RING_SIZE - audio.write_pos;

            if (first > n)
                first = n;

            memcpy(audio.data + audio.write_pos,
                   data + written, first);

            memcpy(audio.data,
                   data + written + first,
                   n - first);

            audio.write_pos =
                (audio.write_pos + n) % RING_SIZE;

            audio.count += n;
            written += n;
        }

        SDL_UnlockMutex(audio.mutex);

        if (written < len)
            SDL_Delay(2);
    }

    return written;
}

/* -------------------------------------------------- */
/* Callback for the HDMI audio source                 */
/* -------------------------------------------------- */

void HDMI_Capture_Callback(void *userdata, Uint8 *stream, int len) {
    audRunner *doRun = (audRunner *)userdata;

    if (doRun->mute || !doRun->hdmi_resampler) {
        return;
    }

    // Feed the 48kHz data from the HDMI card into the resampler.
    SDL_AudioStreamPut(doRun->hdmi_resampler, stream, len);

    // Check how much 44.1 kHz data we have obtained after resampling.
    int available_bytes = SDL_AudioStreamAvailable(doRun->hdmi_resampler);
    if (available_bytes <= 0) return;

    // Allocate a temporary buffer for the resampled data
    Uint8 *converted_buffer = malloc(available_bytes);
    if (!converted_buffer) return;

    SDL_AudioStreamGet(doRun->hdmi_resampler, converted_buffer, available_bytes);

    // Lock the mutex and write the data to your existing ring buffer.
    SDL_LockMutex(audio.mutex);

    Uint32 bytes_to_copy = available_bytes;

    // If the ring buffer is too full, prevent overflow
    if (bytes_to_copy > (Uint32)(RING_SIZE - audio.count)) {
        bytes_to_copy = RING_SIZE - audio.count;
    }

    if (bytes_to_copy > 0) {
        int first = RING_SIZE - audio.write_pos;
        if (first > (int)bytes_to_copy) {
            first = bytes_to_copy;
        }

        // Copy the data
        memcpy(audio.data + audio.write_pos, converted_buffer, first);
        memcpy(audio.data, converted_buffer + first, bytes_to_copy - first);

        // Update the ring buffer's counter
        audio.write_pos = (audio.write_pos + bytes_to_copy) % RING_SIZE;
        audio.count += bytes_to_copy;
    }

    SDL_UnlockMutex(audio.mutex);
    free(converted_buffer);
}

/* -------------------------------------------------- */
/* SDL audio MAIN callback                            */
/* -------------------------------------------------- */

void audio_callback(void *userdata, Uint8 *stream, int len)
{
    audUserd *ud = (audUserd *)userdata;
    float seconds_played;

    // Reset the flow (silence as a foundation)
    memset(stream, 0, len);

    // Retrieve and copy data from the regular app audio buffer
    SDL_LockMutex(audio.mutex);

    int n = audio.count;
    if (n > len) n = len;

    int first = RING_SIZE - audio.read_pos;
    if (first > n) first = n;

    // Copy the audio stream to the speakers ONLY if mute is OFF (e.g., ud->mute == 0)
    if (ud && ud->mute == 0)
    {
        memcpy(stream, audio.data + audio.read_pos, first);
        memcpy(stream + first, audio.data, n - first);
    }

    audio.read_pos = (audio.read_pos + n) % RING_SIZE;
    audio.count -= n;

    audio.played_bytes += n;
    audio.last_callback_ticks = SDL_GetTicks64();

    seconds_played = (float)n / (AUDIO_RATE * AUDIO_CHANNELS * 2);
    audio.cur_pos_sec += seconds_played;

    SDL_UnlockMutex(audio.mutex);

    //  MIX IN THE RTSP CAMERA'S AUDIO
    if (ud && rtsp_audio.enabled && !rtsp_audio.muted && !ud->mute)
    {
        SDL_LockMutex(rtsp_audio.mutex);

        int rtsp_n = rtsp_audio.count;
        if (rtsp_n > len) rtsp_n = len;

        if (rtsp_n > 0)
        {
            // Allocate a temporary buffer on the stack (C99 Variable Length Array)
            Uint8 temp_rtsp[rtsp_n];

            int rtsp_first = RTSP_RING_SIZE - rtsp_audio.read_pos;
            if (rtsp_first > rtsp_n) rtsp_first = rtsp_n;

            // Copy from the RTSP ring buffer to our temp buffer
            memcpy(temp_rtsp, rtsp_audio.data + rtsp_audio.read_pos, rtsp_first);
            memcpy(temp_rtsp + rtsp_first, rtsp_audio.data, rtsp_n - rtsp_first);

            // Update the RTSP buffer positions
            rtsp_audio.read_pos = (rtsp_audio.read_pos + rtsp_n) % RTSP_RING_SIZE;
            rtsp_audio.count -= rtsp_n;

            // Mix the camera audio into what is already present in the stream.
            // Replace AUDIO_S16SYS if you are using a different format (e.g., AUDIO_F32SYS)
            SDL_MixAudioFormat(stream, temp_rtsp, AUDIO_S16SYS, rtsp_n, SDL_MIX_MAXVOLUME);
        }

        SDL_UnlockMutex(rtsp_audio.mutex);
    }
}

/* -------------------------------------------------- */
/* Convert decoded frame to PCM                       */
/* -------------------------------------------------- */

static int convert_frame(DecoderJob *job,
                         AVCodecContext *codec,
                         SwrContext *swr,
                         AVFrame *frame)
{
    int max_samples = av_rescale_rnd(
        swr_get_delay(swr, codec->sample_rate)
        + frame->nb_samples,
        AUDIO_RATE,
        codec->sample_rate,
        AV_ROUND_UP);

    int output_size = av_samples_get_buffer_size(
        NULL,
        AUDIO_CHANNELS,
        max_samples,
        AV_SAMPLE_FMT_S16,
        1);

    if (output_size <= 0)
        return -1;

    uint8_t *output = av_malloc(output_size);

    if (!output)
        return -1;

    uint8_t *out[] = { output };

    int samples = swr_convert(
        swr,
        out,
        max_samples,
        (const uint8_t **)frame->extended_data,
        frame->nb_samples);

    if (samples > 0)
    {
        int bytes = samples * AUDIO_CHANNELS * 2;

        ring_write(job, output, bytes);
    }

    av_free(output);

    return samples;
}

/* -------------------------------------------------- */
/* Receive decoded frames                             */
/* -------------------------------------------------- */

static void receive_frames(DecoderJob *job,
                           AVCodecContext *codec,
                           SwrContext *swr,
                           AVFrame *frame)
{
    while (!SDL_AtomicGet(&job->cancel))
    {
        int ret = avcodec_receive_frame(codec, frame);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;

        if (ret < 0)
            break;

        // Check whether we are seeking to an exact point following a jump
        if (job->exact_seek_pts > 0)
        {
            // Make sure frame->pts actually contains a valid time before we compare
            if (frame->pts != AV_NOPTS_VALUE && frame->pts < job->exact_seek_pts)
            {
                // We haven't reached the goal yet: clean up this frame and move on to the next one in the loop.
                av_frame_unref(frame);
                continue;
            }
            // We have reached (or passed) the target!
            // Reset so that everything plays back normally from this point on.
            job->exact_seek_pts = 0;
        }

        if (!audio.audio_start_set && frame->pts != AV_NOPTS_VALUE)
        {
            SDL_LockMutex(audio.mutex);
            audio.audio_start_sec = frame->pts * av_q2d(codec->pkt_timebase);
            audio.audio_start_set = 1;
            audio.last_callback_ticks = SDL_GetTicks64();
            SDL_UnlockMutex(audio.mutex);
        }

        convert_frame(job, codec, swr, frame);

        av_frame_unref(frame);
    }
}

static void print_tag(AVDictionary *dict, const char *key, char *str)
{

    AVDictionaryEntry *tag = av_dict_get(dict, key, NULL, 0);

    if (tag && tag->value) {
//        printf("%-12s: %s\n", key, tag->value);
        snprintf(str, 46, "%-12s: %s", key, tag->value); // Don't spill through GUI limits
    }
}

void show_audio_metadata(Playlist *pl)
{
    if (!g_fmt_ctx) {
        return;
    }
  
    AVDictionary *metadata = g_fmt_ctx->metadata;

    print_tag(metadata, "title",    pl->title);
    print_tag(metadata, "artist",   pl->artist);
    print_tag(metadata, "album",    pl->album);
    print_tag(metadata, "album_artist",pl->album_artist);
    print_tag(metadata, "genre",    pl->genre);
    print_tag(metadata, "date",     pl->date);
    print_tag(metadata, "track",    pl->track);
    print_tag(metadata, "comment",  pl->comment);

    // Duration is expressed in AV_TIME_BASE units
    if (g_fmt_ctx->duration != AV_NOPTS_VALUE) {
        int64_t seconds = g_fmt_ctx->duration / AV_TIME_BASE;

#if 0
        printf("%-12s: %02" PRId64 ":%02" PRId64 "\n",
               "duration",
               seconds / 60,
               seconds % 60);
#endif
        
        snprintf(pl->duration, 79, "%-12s: %02" PRId64 ":%02" PRId64 " ",
            "duration",
            seconds / 60,
            seconds % 60);

        pl->s_duration =  (float)seconds;
    }
}

static void video_queue_destroy(VideoQueue *q)
{
    while (q->count > 0) {
        video_queue_pop(q);
    }
    if (q->cond) SDL_DestroyCond(q->cond);
    if (q->mutex) SDL_DestroyMutex(q->mutex);
}

static void video_queue_init(VideoQueue *q)
{
    memset(q, 0, sizeof(VideoQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

static void video_queue_push(VideoQueue *q, AVFrame *src_frame, SDL_atomic_t *cancel)
{
    AVFrame *clone = av_frame_clone(src_frame);
    if (!clone) return;

    SDL_LockMutex(q->mutex);
    while (q->count == VIDEO_QUEUE_SIZE && !SDL_AtomicGet(cancel)) {
        SDL_CondWaitTimeout(q->cond, q->mutex, 10);
    }

    if (SDL_AtomicGet(cancel)) {
        av_frame_free(&clone);
        SDL_UnlockMutex(q->mutex);
        return;
    }

    q->frames[q->tail] = clone;
    q->tail = (q->tail + 1) % VIDEO_QUEUE_SIZE;
    q->count++;

    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
}

static void video_queue_clear(VideoQueue *q)
{
    SDL_LockMutex(q->mutex);

    while (q->count > 0) {
        av_frame_free(&q->frames[q->head]);
        q->head = (q->head + 1) % VIDEO_QUEUE_SIZE;
        q->count--;
    }

    q->head = 0;
    q->tail = 0;

    SDL_CondBroadcast(q->cond);
    SDL_UnlockMutex(q->mutex);
}

/* -------------------------------------------------- */
/* Public functions                                   */
/* -------------------------------------------------- */

AVFrame *video_queue_peek(VideoQueue *q) {
    SDL_LockMutex(q->mutex);
    AVFrame *f = (q->count > 0) ? q->frames[q->head] : NULL;
    SDL_UnlockMutex(q->mutex);
    return f;
}

void video_queue_pop(VideoQueue *q) {
    SDL_LockMutex(q->mutex);
    if (q->count > 0) {
        av_frame_free(&q->frames[q->head]);
        q->head = (q->head + 1) % VIDEO_QUEUE_SIZE;
        q->count--;
        SDL_CondSignal(q->cond);
    }
    SDL_UnlockMutex(q->mutex);
}


double get_audio_clock(void)
{
    SDL_LockMutex(audio.mutex);
    uint64_t bytes = audio.played_bytes;
    double start_sec = audio.audio_start_sec;
    uint64_t last_ticks = audio.last_callback_ticks;
    int paused = audio.paused; // if you have a pause flag
    SDL_UnlockMutex(audio.mutex);

    double bytes_per_sec = (double)AUDIO_RATE * AUDIO_CHANNELS * 2.0;
    double clock = start_sec + ((double)bytes / bytes_per_sec);

    if (last_ticks > 0 && !paused) {
        double elapsed = (double)(SDL_GetTicks64() - last_ticks) / 1000.0;
        if (elapsed > 0.10) elapsed = 0.10; // clamp to max 1 callback period
        clock += elapsed;
    }

    return clock;
}

/* -------------------------------------------------- */
/* Decoder thread                                     */
/* -------------------------------------------------- */

static int decoder_thread(void *arg)
{
    DecoderJob *job = arg;

    AVFormatContext *fmt = NULL;
    AVCodecContext *codec = NULL;

    AVPacket *packet = NULL;
    AVFrame *frame = NULL;

    SwrContext *swr = NULL;

    int audio_stream = -1;
    int video_stream = -1;

    AVCodecContext *vcodec = NULL;
    AVFrame *vframe = NULL;

    if (avformat_open_input(&fmt, job->filename, NULL, NULL) < 0)
         goto cleanup;

    if (avformat_find_stream_info(fmt, NULL) < 0)
        goto cleanup;

    if (fmt != NULL)
        g_fmt_ctx = fmt;

    /* --- Audio setup --- */
    audio_stream = av_find_best_stream(
        fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);

    if (audio_stream < 0)
        goto cleanup;

    const AVCodec *decoder =
        avcodec_find_decoder(fmt->streams[audio_stream]->codecpar->codec_id);

    if (!decoder)
        goto cleanup;

    codec = avcodec_alloc_context3(decoder);
    if (!codec)
        goto cleanup;

    if (avcodec_parameters_to_context(
            codec, fmt->streams[audio_stream]->codecpar) < 0)
        goto cleanup;

    codec->pkt_timebase = fmt->streams[audio_stream]->time_base;

    if (avcodec_open2(codec, decoder, NULL) < 0)
        goto cleanup;

    /* --- Video setup --- */
    for (unsigned int i = 0; i < fmt->nb_streams; i++)
    {
        AVStream *st = fmt->streams[i];

        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            if (audio_stream < 0)
                audio_stream = (int)i;
        }
        else if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            // Ignore embedded album art
            if (!(st->disposition & AV_DISPOSITION_ATTACHED_PIC))
            {
                if (video_stream < 0)
                    video_stream = (int)i;
            }
        }
    }

    /* Classify media */
    if (audio_stream >= 0 && video_stream >= 0)
    {
        //printf("// Video file with audio (e.g., .mp4, .mkv)\n");
        audio.is_video = 1;   // Video with audio
    }
    else if (audio_stream >= 0)
    {
        //printf("// Pure audio, album art ignored\n");
        audio.is_video = 0;
    }
    else
    {
        //printf("// No audio stream: not supported by this audio player\n");
        goto cleanup;
    }

    if (video_stream >= 0)
    {
        job->video_time_base = fmt->streams[video_stream]->time_base;
        job->video_frame_rate = av_guess_frame_rate(fmt, fmt->streams[video_stream], NULL);
        job->video_frame_count = 0;

        if (video_stream >= 0) {
            const AVCodec *vdecoder =
                avcodec_find_decoder(fmt->streams[video_stream]->codecpar->codec_id);

            if (!vdecoder)
                 goto cleanup;

            vcodec = avcodec_alloc_context3(vdecoder);

            if (!vcodec)
                 goto cleanup;

            if (avcodec_parameters_to_context(vcodec, fmt->streams[video_stream]->codecpar) < 0)
                 goto cleanup;

            vcodec->pkt_timebase = fmt->streams[video_stream]->time_base;

            if (avcodec_open2(vcodec, vdecoder, NULL) < 0)
                 goto cleanup;

            vframe = av_frame_alloc();
            if (!vframe)
                goto cleanup;
        }
    }

    /* --- Input channel layout & SwrContext --- */
    AVChannelLayout in_layout = {0};
    AVChannelLayout out_layout = {0};

    if (codec->ch_layout.nb_channels > 0)
        av_channel_layout_copy(&in_layout, &codec->ch_layout);
    else
        av_channel_layout_default(&in_layout, 2);

    av_channel_layout_default(&out_layout, AUDIO_CHANNELS);

    int ret = swr_alloc_set_opts2(
        &swr,
        &out_layout,
        AV_SAMPLE_FMT_S16,
        AUDIO_RATE,
        &in_layout,
        codec->sample_fmt,
        codec->sample_rate,
        0, NULL);

    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);

    if (ret < 0 || !swr || swr_init(swr) < 0)
        goto cleanup;

    packet = av_packet_alloc();
    frame = av_frame_alloc();

    if (!packet || !frame)
        goto cleanup;

    /* --- Read and decode loop --- */
    while (!SDL_AtomicGet(&job->cancel))
    {

        /* --- LOGIC FOR EXACT SEARCH (SEEK) --- */
        if (SDL_AtomicGet(&job->seek_requested))
        {
            int target_ms = SDL_AtomicGet(&job->seek_target_ms);
            SDL_AtomicSet(&job->seek_requested, 0);

            double target_seconds = target_ms / 1000.0;

            int64_t seek_ts = (int64_t)(target_seconds * AV_TIME_BASE);

            if (av_seek_frame(fmt, -1, seek_ts, AVSEEK_FLAG_BACKWARD) >= 0)
            {
                // 1. Set exact PTS targets for both streams
                job->exact_seek_pts = av_rescale_q(
                    (int64_t)(target_seconds * AV_TIME_BASE),
                    AV_TIME_BASE_Q,
                    fmt->streams[audio_stream]->time_base
                );

                avcodec_flush_buffers(codec);
                swr_init(swr);

                if (video_stream >= 0 && vcodec) {
                    job->exact_video_seek_pts = av_rescale_q(
                        (int64_t)(target_seconds * AV_TIME_BASE),
                        AV_TIME_BASE_Q,
                        fmt->streams[video_stream]->time_base
                    );
                    avcodec_flush_buffers(vcodec);
                    video_queue_clear(&job->video_queue); // <-- Use your queue's clear/flush function
                }


                // 2. Reset audio buffer and clock baseline
                SDL_LockMutex(audio.mutex);
                audio.read_pos = 0;
                audio.count = 0;
                audio.eof = 0;
                audio.write_pos = 0;
                audio.played_bytes = 0;
                audio.audio_start_sec = target_seconds;
                audio.audio_start_set = 0;
                audio.cur_pos_sec = target_seconds;
                SDL_UnlockMutex(audio.mutex);
            }
        }

        if (av_read_frame(fmt, packet) < 0) {
            break; // End of file or error
        }

        if (packet->stream_index == audio_stream)
        {
            ret = avcodec_send_packet(codec, packet);
            if (ret >= 0)
                receive_frames(job, codec, swr, frame);
        }

        else if (video_stream >= 0 && packet->stream_index == video_stream)
        {
            ret = avcodec_send_packet(vcodec, packet);
            while (ret >= 0) {
                ret = avcodec_receive_frame(vcodec, vframe);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    goto cleanup;
                }

                if (vframe->pts == AV_NOPTS_VALUE) {
                    vframe->pts = vframe->best_effort_timestamp;
                }
                if (vframe->pts == AV_NOPTS_VALUE && job->video_frame_rate.num > 0) {
                    double fps = av_q2d(job->video_frame_rate);
                    double sec = (double)job->video_frame_count / fps;
                    vframe->pts = (int64_t)(sec / av_q2d(job->video_time_base));
                }
                job->video_frame_count++;

                // Discard decoded frames before the exact seek target
                if (job->exact_video_seek_pts > 0) {
                    if (vframe->pts != AV_NOPTS_VALUE && vframe->pts < job->exact_video_seek_pts) {
                        av_frame_unref(vframe);
                        continue;
                    }
                    job->exact_video_seek_pts = 0;
                }

                video_queue_push(&job->video_queue, vframe, &job->cancel);
                av_frame_unref(vframe);
            }
        }

        av_packet_unref(packet);
    }

    /* --- Flush decoders --- */
    if (!SDL_AtomicGet(&job->cancel))
    {
        /* Flush audio */
        avcodec_send_packet(codec, NULL);
        receive_frames(job, codec, swr, frame);

        /* Flush video */
        if (vcodec) {
            avcodec_send_packet(vcodec, NULL);
            while (avcodec_receive_frame(vcodec, vframe) >= 0) {
                if (vframe->pts == AV_NOPTS_VALUE) {
                    vframe->pts = vframe->best_effort_timestamp;
                }
                if (vframe->pts == AV_NOPTS_VALUE && job->video_frame_rate.num > 0) {
                    double fps = av_q2d(job->video_frame_rate);
                    double sec = (double)job->video_frame_count / fps;
                    vframe->pts = (int64_t)(sec / av_q2d(job->video_time_base));
                }
                job->video_frame_count++;

                video_queue_push(&job->video_queue, vframe, &job->cancel);
                av_frame_unref(vframe);
            }
        }

        /* Flush resampler */
        while (!SDL_AtomicGet(&job->cancel))
        {
            int delay = (int)swr_get_delay(swr, AUDIO_RATE);
            if (delay <= 0)
                break;

            uint8_t *output = av_malloc(delay * AUDIO_CHANNELS * 2);
            if (!output)
                break;

            uint8_t *out[] = { output };
            int samples = swr_convert(swr, out, delay, NULL, 0);

            if (samples > 0)
                ring_write(job, output, samples * AUDIO_CHANNELS * 2);

            av_free(output);

            if (samples <= 0)
                break;
        }
    }

cleanup:

    /* Signal end of file */
    SDL_LockMutex(audio.mutex);
    audio.eof = 1;
    SDL_UnlockMutex(audio.mutex);

    if (swr)
        swr_free(&swr);

    if (frame)
        av_frame_free(&frame);

    if (vframe)
        av_frame_free(&vframe);

    if (packet)
        av_packet_free(&packet);

    if (codec)
        avcodec_free_context(&codec);

    if (vcodec)
        avcodec_free_context(&vcodec);

    if (fmt)
        avformat_close_input(&fmt);

    audio.is_video = 0;

    g_fmt_ctx = NULL;

    return 0;
}

/* -------------------------------------------------- */
/* Audio control thread                               */
/* -------------------------------------------------- */

static int ring_finished(void)
{
    int finished;

    SDL_LockMutex(audio.mutex);

    finished = audio.eof && audio.count == 0;

    SDL_UnlockMutex(audio.mutex);

    return finished;
}

static void stop_decoder(DecoderJob **job,
                         SDL_Thread **thread)
{
    SDL_PauseAudioDevice(audio_device, 1);

    if (*job)
        SDL_AtomicSet(&(*job)->cancel, 1);

    if (*job)
    {
        SDL_AtomicSet(&(*job)->cancel, 1);

        // Wake up decoder_thread if it is waiting during a full video_queue_push
        SDL_LockMutex((*job)->video_queue.mutex);
        SDL_CondBroadcast((*job)->video_queue.cond);
        SDL_UnlockMutex((*job)->video_queue.mutex);
    }


    // Wait until the decoder thread has finished executing its own cleanup: block;
    if (*thread)
    {
        SDL_WaitThread(*thread, NULL);
        *thread = NULL;
    }

    // Disconnect g_current_job so the main thread stops drawing immediately
    SDL_LockMutex(g_job_mutex);
    g_current_job = NULL;
    SDL_UnlockMutex(g_job_mutex);

    // Drain remaining AVFrame clones and destroy the queue (now that the thread has finished)
    if (*job)
    {
        video_queue_destroy(&(*job)->video_queue);
    }

    free(*job);
    *job = NULL;

    ring_clear();
}

static int start_track(Playlist *playlist,
                       int index,
                       DecoderJob **job,
                       SDL_Thread **thread)
{
    stop_decoder(job, thread);

 //   printf("index=%d    playlist->count=%d   file=%s\n", index, playlist->count, playlist->files[index]);

    if (index < 0 || index >= playlist->count)
        return -1;

    *job = calloc(1, sizeof(DecoderJob));

    if (!*job)
        return -1;

    (*job)->filename = playlist->files[index];

    SDL_AtomicSet(&(*job)->cancel, 0);

    video_queue_init(&(*job)->video_queue);

    *thread = SDL_CreateThread(decoder_thread, "decoder", *job);

    if (!*thread)
    {
        free(*job);
        *job = NULL;
        return -1;
    }

    audio.cur_pos_sec = 0.0f;
    audio.played_bytes = 0;
    SDL_LockMutex(g_job_mutex);
    g_current_job = (*job);
    SDL_UnlockMutex(g_job_mutex);

    SDL_PauseAudioDevice(audio_device, 0);

    return 0;
}

static int command_pop(PlayerCommand *cmd)
{
    SDL_LockMutex(commands.mutex);

    /* Wait at most 50 ms */

    if (commands.count == 0)
        SDL_CondWaitTimeout(commands.cond,
                            commands.mutex, 50);

    if (commands.count == 0)
    {
        SDL_UnlockMutex(commands.mutex);
        return 0;
    }

    *cmd = commands.queue[commands.read_pos];

    commands.read_pos =
        (commands.read_pos + 1) % COMMAND_QUEUE;

    commands.count--;

    SDL_UnlockMutex(commands.mutex);

    return 1;
}

int control_thread(void *arg)
{
    Playlist *playlist = arg;

    SDL_Thread *decoder = NULL;
    DecoderJob *job = NULL;

    int running = 1;

    while (running)
    {
        PlayerCommand cmd = 0;

        if (command_pop(&cmd))
        {
            switch (cmd)
            {
                case CMD_PLAY:

                    if (playlist->paused)
                    {
                        playlist->paused = 0;
                        SDL_PauseAudioDevice(audio_device, 0);
                    }
                    else if (!playlist->active)
                    {
                        if (start_track(playlist, playlist->index,
                                        &job, &decoder) == 0)
                        {
                            playlist->active = 1;
                        }
                    }
                    break;

                case CMD_PLAYSEL:


                    if (playlist->paused)
                    {
                        playlist->paused = 0;
                        SDL_PauseAudioDevice(audio_device, 0);
                    }
                    else if (!playlist->active)
                    {
                        if (start_track(playlist, playlist->index,
                                        &job, &decoder) == 0)
                        {
                            playlist->active = 1;
                        }
                    }
                    break;

                    case CMD_JUMP:

                    if (playlist->active && job != NULL)
                    {
                        // Ignore new hops if the decoder is still processing the previous one
                        if (SDL_AtomicGet(&job->seek_requested) == 0)
                        {
                            double target_secs = seek_target_seconds;

                            // Ensure that we never attempt to jump to a negative point in time.
                            if (target_secs < 0.0) {
                                target_secs = 0.0;
                            }

                            // Convert to milliseconds and trigger the decoding thread atomically
                            int target_ms = (int)(target_secs * 1000.0);
                            SDL_AtomicSet(&job->seek_target_ms, target_ms);
                            SDL_AtomicSet(&job->seek_requested, 1);
                        }
                    }

                    break;


                case CMD_PAUSE:

                    if (playlist->active && !playlist->paused)
                    {
                        playlist->paused = 1;
                        SDL_PauseAudioDevice(audio_device, 1);
                    }
                    break;


                case CMD_STOP:

                    stop_decoder(&job, &decoder);
                    playlist->active = 0;
                    playlist->paused = 0;
                    break;


                case CMD_NEXT:

                    playlist->index = (playlist->index + 1) % playlist->count;

                    if (start_track(playlist, playlist->index,
                                    &job, &decoder) == 0)
                    {
                        playlist->active = 1;
                        playlist->paused = 0;
                    }
                    break;


                case CMD_PREVIOUS:

                    playlist->index = (playlist->index - 1 + playlist->count)
                            % playlist->count;

                    if (start_track(playlist, playlist->index,
                                    &job, &decoder) == 0)
                    {
                        playlist->active = 1;
                        playlist->paused = 0;
                    }
                    break;


                case CMD_SHUTDOWN:

                    running = 0;
                    break;
            }
        }

        /* Automatic next track */

        if (running && playlist->active && !playlist->paused && ring_finished())
        {
            stop_decoder(&job, &decoder);

            if (playlist->index + 1 < playlist->count)
            {
                playlist->index++;

                if (start_track(playlist, playlist->index, &job, &decoder) == 0)
                {
                    playlist->active = 1;
                }
                else
                {
                    playlist->active = 0;
                }
            }
            else
            {
                playlist->active = 0;
            }
        }
    }

    stop_decoder(&job, &decoder);

    return 0;
}

