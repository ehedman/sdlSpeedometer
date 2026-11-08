
#ifndef FPLAYSPEEDOMETER_H
#define FPLAYSPEEDOMETER_H

#define AUDIO_RATE       44100
#define AUDIO_CHANNELS   2
#define AUDIO_BUFFER     4096

#define RING_SIZE        (AUDIO_RATE * AUDIO_CHANNELS * 2 * 2)
#define COMMAND_QUEUE    16

#define DEF_ACOVER      "defsound.png"

/* -------------------------------------------------- */
/* Commands                                           */
/* -------------------------------------------------- */

typedef enum
{
    CMD_PLAY,
    CMD_PLAYSEL,
    CMD_PAUSE,
    CMD_STOP,
    CMD_NEXT,
    CMD_PREVIOUS,
    CMD_SHUTDOWN

} PlayerCommand;


/* -------------------------------------------------- */
/* PCM ringbuffer                                     */
/* -------------------------------------------------- */

typedef struct
{
    uint8_t data[RING_SIZE];

    int read_pos;
    int write_pos;
    int count;

    int eof;

    SDL_mutex *mutex;

} AudioBuffer;

/* -------------------------------------------------- */
/* Decoder job                                        */
/* -------------------------------------------------- */

typedef struct
{
    const char *filename;

    SDL_atomic_t cancel;

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
    int kalle;
    int seq_track_no[80];
    float s_duration;
    char **files;
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

} CommandQueue;

typedef struct
{
    AudioBuffer audio;
    CommandQueue commands;
    SDL_AudioDeviceID audio_device;

} AudioProps;

//extern AudioProps apropps;
extern void player_init(AudioProps *propps);
extern void audio_callback(void *userdata, Uint8 *stream, int len);
extern int control_thread(void *arg);
extern int command_push(PlayerCommand cmd);
extern int scandirs(Playlist *plist);
extern void show_audio_metadata(Playlist *pl);
extern void HDMI_Capture_Callback(void *userdata, Uint8 *stream, int len);

#endif /* FPLAYSPEEDOMETER_H */
