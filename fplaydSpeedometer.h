#ifndef FPLAYSPEEDOMETER_H
#define FPLAYSPEEDOMETER_H

#include <SDL2/SDL.h>
#include <libswresample/swresample.h>

#define AUDIO_RATE      44100
#define AUDIO_CHANNELS  2
#define AUDIO_BUFFER    4096

#define RING_SIZE       (AUDIO_RATE * AUDIO_CHANNELS * 2 * 2)
#define COMMAND_QUEUE   16

#define RTSP_RING_SIZE  131072

/* -------------------------------------------------- */
/* Commands                                           */
/* -------------------------------------------------- */

typedef enum
{
    CMD_PLAY,
    CMD_PLAYSEL,
    CMD_JUMP,
    CMD_PAUSE,
    CMD_STOP,
    CMD_NEXT,
    CMD_PREVIOUS,
    CMD_SHUTDOWN

} PlayerCommand;

/* -------------------------------------------------- */
/* PCM ringbuffer                                     */
/* -------------------------------------------------- */

#define VIDEO_QUEUE_SIZE 60

typedef struct {
    AVFrame *frames[VIDEO_QUEUE_SIZE];
    int head;
    int tail;
    int count;
    SDL_mutex *mutex;
    SDL_cond *cond;
} VideoQueue;

typedef struct {
    SDL_Texture *texture;
    struct SwsContext *sws_ctx;
    AVFrame *yuv_frame;
} VideoRenderer;

typedef struct
{
    uint8_t data[RING_SIZE];
    int read_pos;
    int write_pos;
    int count;
    int64_t played_bytes;
    int64_t audio_start_set;
    double audio_start_sec;
    uint64_t last_callback_ticks;
    int is_video;
    int paused;
    int eof;

    SDL_mutex *mutex;

    volatile float cur_pos_sec;

} AudioBuffer;

/* -------------------------------------------------- */
/* Decoder job                                        */
/* -------------------------------------------------- */
typedef struct
{
    const char *filename;
    SDL_atomic_t cancel;
    int64_t exact_seek_pts;
    SDL_atomic_t seek_requested;
    SDL_atomic_t seek_target_ms;
    VideoQueue video_queue;
    AVRational video_time_base;
    AVRational video_frame_rate;
    double exact_video_seek_pts;
    int64_t video_frame_count;


} DecoderJob;

/* -------------------------------------------------- */
/* Shared data                                        */
/* -------------------------------------------------- */
typedef struct
{
    size_t count;
    int index;
    int active;
    int paused;
    int track_no[80];
    float s_duration;
    char **files;
    char **titles;
    char *path;
    char *apath;
    char title[80];
    char artist[80];
    char album[80];
    char album_artist[80];
    char genre[80];
    char date[80];
    char track[80];
    char duration[80];
    char comment[80];
} Playlist;

typedef struct {
    char *filename;
    char *title;
    int track_no;
    int maxb;
} TrackItem;

#define RTSP_RING_SIZE  131072

typedef struct {
    Uint8 *data;
    int read_pos;
    int write_pos;
    int count;
    int enabled;
    int muted;
    SDL_mutex *mutex;
} RTSPAudioBuffer;

/* -------------------------------------------------- */
/* Command FIFO                                       */
/* -------------------------------------------------- */

typedef struct
{
    PlayerCommand queue[COMMAND_QUEUE];

    int read_pos;
    int write_pos;
    int count;

    SDL_mutex *mutex;
    SDL_cond *cond;
    SDL_mutex *rtsp_mutex;

} CommandQueue;

typedef struct
{
    AudioBuffer audio;
    CommandQueue commands;
    SDL_AudioDeviceID audio_device;

} AudioProps;

extern void player_init(AudioProps *propps);
extern void audio_callback(void *userdata, Uint8 *stream, int len);
extern int control_thread(void *arg);
extern int command_push(PlayerCommand cmd);
extern int scandirs(Playlist *plist);
extern void show_audio_metadata(Playlist *pl);
extern void HDMI_Capture_Callback(void *userdata, Uint8 *stream, int len);
extern RTSPAudioBuffer rtsp_audio;
extern void player_set_cur_pos_sec(float s);
extern float player_get_cur_pos_sec(void);

extern AVFrame *video_queue_peek(VideoQueue *q);
extern void video_queue_pop(VideoQueue *q);
extern double get_audio_clock(void);
extern double get_video_clock(void);
extern int media_is_video(void);
extern void video_renderer_cleanup(VideoRenderer *vr);
extern SDL_Rect calculate_aspect_rect(SDL_Rect box, int video_w, int video_h);
extern void update_video_texture(SDL_Renderer *renderer, VideoRenderer *vr, VideoQueue *q, AVRational time_base, double audio_clock);
extern DecoderJob *job;
extern DecoderJob *g_current_job;
extern SDL_mutex *g_job_mutex;
extern VideoRenderer vr;

#endif /* FPLAYSPEEDOMETER_H */
