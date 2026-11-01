
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>

#include <SDL2/SDL.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include "sdlSpeedometer.h"
#include "fplaydSpeedometer.h"

AudioBuffer audio;
CommandQueue commands;
SDL_AudioDeviceID audio_device;

#define _POSIX_C_SOURCE 200809L

/* -------------------------------------------------- */
/* Find files function                                */
/* -------------------------------------------------- */

int scandirs(Playlist *plist)
{
    int rval = 0;

    DIR *dir;

    dir = opendir(plist->path);

    if (dir == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "doMedia(scandirs): Could not open \"%s\": %s", plist->path, SDL_GetError());
        return -1;
    }

    struct dirent *entry;

    int artFound;

    if (plist->files == NULL)
        artFound = 0;

    while ((entry = readdir(dir)) != NULL) {

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
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
        }

        // Expand the array using a pointer
        char **temp = realloc(plist->files, (plist->count + 1) * sizeof(*plist->files));

        if (temp == NULL) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "doMedia(scandirs): Could not allocate with  realloc() %s", SDL_GetError());
            rval = -1;
            goto cleanup;
        }

        plist->files = temp;

        // Copy the filename
        plist->files[plist->count] = strdup(entry->d_name);

        if (plist->files[plist->count] == NULL) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "doMedia(scandirs): Could not allocate with  strdup() %s", SDL_GetError());
            rval = -1;
            goto cleanup;
        }

#if 0
        // Diagnostics
        printf("DEBUG: [%zu] ptr=%p name=%s\n",
               plist->count,
               (void *)plist->files[plist->count],
               plist->files[plist->count]);
#endif

        plist->count++;
    }

    closedir(dir);
    dir = NULL;

cleanup:
    if (dir != NULL)
        closedir(dir);

    if (plist->files != NULL && rval) {
        // Release all strings
        for (size_t i = 0; i < plist->count; i++) {
            free(plist->files[i]);
        }

        free(plist->files);
    }

    return rval;
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


/* -------------------------------------------------- */
/* PCM ringbuffer                                     */
/* -------------------------------------------------- */

static void ring_clear(void)
{
    SDL_LockMutex(audio.mutex);

    audio.read_pos = 0;
    audio.write_pos = 0;
    audio.count = 0;
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
/* SDL audio callback                                 */
/* -------------------------------------------------- */

void audio_callback(void *userdata,
                           Uint8 *stream, int len)
{
    memset(stream, 0, len);

    SDL_LockMutex(audio.mutex);

    int n = audio.count;

    if (n > len)
        n = len;

    int first = RING_SIZE - audio.read_pos;

    if (first > n)
        first = n;

    memcpy(stream, audio.data + audio.read_pos, first);

    memcpy(stream + first, audio.data, n - first);

    audio.read_pos =
        (audio.read_pos + n) % RING_SIZE;

    audio.count -= n;

    SDL_UnlockMutex(audio.mutex);
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

        if (ret == AVERROR(EAGAIN) ||
            ret == AVERROR_EOF)
            break;

        if (ret < 0)
            break;

        convert_frame(job, codec, swr, frame);

        av_frame_unref(frame);
    }
}

static AVFormatContext  *g_fmt_ctx;

static void print_tag(AVDictionary *dict, const char *key, char *str)
{

    AVDictionaryEntry *tag = av_dict_get(dict, key, NULL, 0);

    if (tag && tag->value) {
//        printf("%-12s: %s\n", key, tag->value);
        snprintf(str, 79, "%-12s: %s", key, tag->value);
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

    if (avformat_open_input(
            &fmt, job->filename, NULL, NULL) < 0)
        goto cleanup;

    if (avformat_find_stream_info(fmt, NULL) < 0)
        goto cleanup;

    if (fmt != NULL)
        g_fmt_ctx = fmt;

    audio_stream = av_find_best_stream(
        fmt, AVMEDIA_TYPE_AUDIO,
        -1, -1, NULL, 0);

    if (audio_stream < 0)
        goto cleanup;

    const AVCodec *decoder =
        avcodec_find_decoder(
            fmt->streams[audio_stream]->codecpar->codec_id);

    if (!decoder)
        goto cleanup;

    codec = avcodec_alloc_context3(decoder);

    if (!codec)
        goto cleanup;

    if (avcodec_parameters_to_context(
            codec,
            fmt->streams[audio_stream]->codecpar) < 0)
        goto cleanup;

    if (avcodec_open2(codec, decoder, NULL) < 0)
        goto cleanup;

    /* Input channel layout */

    AVChannelLayout in_layout = {0};
    AVChannelLayout out_layout = {0};

    if (codec->ch_layout.nb_channels > 0)
        av_channel_layout_copy(&in_layout, &codec->ch_layout);
    else
        av_channel_layout_default(&in_layout, 2);

    av_channel_layout_default(
        &out_layout, AUDIO_CHANNELS);

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

    /* Read and decode */

    while (!SDL_AtomicGet(&job->cancel) &&
           av_read_frame(fmt, packet) >= 0)
    {
        if (packet->stream_index == audio_stream)
        {
            ret = avcodec_send_packet(codec, packet);

            if (ret >= 0)
                receive_frames(job, codec, swr, frame);
        }

        av_packet_unref(packet);
    }

    /* Flush decoder */

    if (!SDL_AtomicGet(&job->cancel))
    {
        avcodec_send_packet(codec, NULL);

        receive_frames(job, codec, swr, frame);

        /* Flush resampler */

        while (!SDL_AtomicGet(&job->cancel))
        {
            int delay = (int)swr_get_delay(swr, AUDIO_RATE);

            if (delay <= 0)
                break;

            uint8_t *output =
                av_malloc(delay * AUDIO_CHANNELS * 2);

            if (!output)
                break;

            uint8_t *out[] = { output };

            int samples = swr_convert(
                swr, out, delay, NULL, 0);

            if (samples > 0)
                ring_write(job, output,
                           samples * AUDIO_CHANNELS * 2);

            av_free(output);

            if (samples <= 0)
                break;
        }
    }

cleanup:

    if (swr)
        swr_free(&swr);

    if (frame)
        av_frame_free(&frame);

    if (packet)
        av_packet_free(&packet);

    if (codec)
        avcodec_free_context(&codec);

    if (fmt)
        avformat_close_input(&fmt);

    g_fmt_ctx = NULL;

    /* Signal end of file */

    SDL_LockMutex(audio.mutex);
    audio.eof = 1;
    SDL_UnlockMutex(audio.mutex);

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

    if (*thread)
    {
        SDL_WaitThread(*thread, NULL);
        *thread = NULL;
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

    if (index < 0 || index >= playlist->count)
        return -1;

    *job = calloc(1, sizeof(DecoderJob));

    if (!*job)
        return -1;

    (*job)->filename = playlist->files[index];

    SDL_AtomicSet(&(*job)->cancel, 0);

#if 0
    printf("Playing: %s\n", (*job)->filename);
#endif

    *thread = SDL_CreateThread(
        decoder_thread, "decoder", *job);

    if (!*thread)
    {
        free(*job);
        *job = NULL;
        return -1;
    }

    SDL_PauseAudioDevice(audio_device, 0);

    return 0;
}


int control_thread(void *arg)
{
    Playlist *playlist = arg;

    DecoderJob *job = NULL;
    SDL_Thread *decoder = NULL;

//    int index = 0;
//    int active = 0;
//    int paused = 0;
    int running = 1;

    while (running)
    {
        PlayerCommand cmd;

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

        if (running && playlist->active && !playlist->paused &&
            ring_finished())
        {
            stop_decoder(&job, &decoder);

            if (playlist->index + 1 < playlist->count)
            {
                playlist->index++;

                if (start_track(playlist, playlist->index,
                                &job, &decoder) == 0)
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
               // printf("Playlist finished\n");
            }
        }
    }

    stop_decoder(&job, &decoder);

    return 0;
}

