
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

static int get_track_from_filename(const char *filepath)
{
    if (!filepath || filepath[0] == '\0') return -1;

    struct stat stats;

    stat(filepath, &stats);
    if (S_ISDIR(stats.st_mode)) // Should not happen
        return -1;

    const char *p = filepath;

    // Skip any leading spaces or non-alphanumeric characters
    while (*p && !isdigit((unsigned char)*p) && !isalpha((unsigned char)*p)) {
        p++;
    }

    // If the string begins with a digit, read it out.
    if (*p && isdigit((unsigned char)*p)) {

        return atoi(p); // atoi reads digits until it encounters a space, hyphen, or period.
    }

    // Found no introductory tracks (e.g., "Intro.mp3")
    return 0;
}

/* -------------------------------------------------- */
/* Find track number in meta data for wav             */
/* -------------------------------------------------- */

static int get_wav_track(const char *filepath)
{
    struct stat stats;

    stat(filepath, &stats);
    if (S_ISDIR(stats.st_mode)) // Should not happen
        return -1;

    FILE *file = fopen(filepath, "rb");
    if (!file) return -1;

    char id[4];
    uint32_t chunk_size;

    // Verify RIFF/WAVE header (the first 12 bytes)
    if (fread(id, 1, 4, file) != 4 || memcmp(id, "RIFF", 4) != 0) {
        fclose(file);
        return 0;
    }
    fseek(file, 4, SEEK_CUR); // Skip total file size
    if (fread(id, 1, 4, file) != 4 || memcmp(id, "WAVE", 4) != 0) {
        fclose(file);
        return 0;
    }

    // Loop through the top-level chunks in the file by skipping over their size
    while (fread(id, 1, 4, file) == 4) {
        if (fread(&chunk_size, 4, 1, file) != 1) break;

        // We find a LIST INFO chunk (standard WAV metadata) ---
        if (memcmp(id, "LIST", 4) == 0) {
            long list_start = ftell(file);
            char list_type[4];
            if (fread(list_type, 1, 4, file) == 4 && memcmp(list_type, "INFO", 4) == 0) {
                // Loop through sub-chunks inside the INFO list
                while (ftell(file) < list_start + chunk_size) {
                    char sub_id[4];
                    uint32_t sub_size;
                    if (fread(sub_id, 1, 4, file) != 4) break;
                    if (fread(&sub_size, 4, 1, file) != 1) break;

                    // ITRK (or TRCK) = Track number in WAV INFO lists
                    if (memcmp(sub_id, "ITRK", 4) == 0 || memcmp(sub_id, "TRCK", 4) == 0) {
                        char num_buf[16] = {0};
                        size_t read_bytes = (sub_size > 15) ? 15 : sub_size;
                        if (fread(num_buf, 1, read_bytes, file) == read_bytes) {
                            num_buf[read_bytes] = '\0';
                            fclose(file);
                            return atoi(num_buf);
                        }
                    } else {
                        // The RIFF standard requires a padding byte for odd sizes.
                        uint32_t pad = sub_size % 2;
                        fseek(file, sub_size + pad, SEEK_CUR);
                    }
                }
            }
            // Reset to the end of the LIST chunk if nothing was found inside
            fseek(file, list_start + chunk_size + (chunk_size % 2), SEEK_SET);
        }
        // We find an embedded ID3v2 chunk (e.g., from Audacity) ---
        else if (memcmp(id, "id3 ", 4) == 0 || memcmp(id, "ID3 ", 4) == 0) {
            long id3_start = ftell(file);
            char sub_id[4];

            // Search for "TRCK" inside the ID3 block
            while (ftell(file) < id3_start + chunk_size - 10) {
                if (fread(sub_id, 1, 4, file) != 4) break;

                if (memcmp(sub_id, "TRCK", 4) == 0) {
                    fseek(file, 4, SEEK_CUR); // Skip ID3 frame size (4 bytes)
                    fseek(file, 2, SEEK_CUR); // Skip flags (2 bytes)

                    // Read in the next 20 bytes (covering text + any UTF encoding)
                    char raw_text[20];
                    size_t read_len = fread(raw_text, 1, 20, file);

                    char clean_num[16];
                    int clean_idx = 0;

                    // Clean the string: Extract only digits and skip hidden zeros/Unicode junk
                    for (size_t i = 0; i < read_len; i++) {
                        if (raw_text[i] >= '0' && raw_text[i] <= '9') {
                            clean_num[clean_idx++] = raw_text[i];
                            if (clean_idx >= 15) break;
                        }
                        else if (clean_idx > 0) {
                            // Stop if we have been reading digits and encounter something else (e.g., '/' or the end)
                            break;
                        }
                    }

                    if (clean_idx > 0) {
                        clean_num[clean_idx] = '\0';
                        fclose(file);
                        return atoi(clean_num);
                    }
                }
                // If it does not match, move back 3 bytes to continue searching byte-by-byte within the ID3 block.
                fseek(file, -3, SEEK_CUR);
            }
            fseek(file, id3_start + chunk_size + (chunk_size % 2), SEEK_SET);
        }
        else {
            // This is the magic: Skip right over the massive audio data in 0 milliseconds!
            uint32_t pad = chunk_size % 2;
            fseek(file, chunk_size + pad, SEEK_CUR);
        }
    }

    fclose(file);

    return 0; // Didn't find a tracking number
}

/* -------------------------------------------------- */
/* Find track number in meta data for mp3             */
/* -------------------------------------------------- */

static int get_mp3_track(const char *filepath)
{
    struct stat stats;

    stat(filepath, &stats);
    if (S_ISDIR(stats.st_mode)) // Should not happen
        return -1;

    FILE *f = fopen(filepath, "rb");
    if (!f) return -1;

    unsigned char id3_header[10];
    if (fread(id3_header, 1, 10, f) != 10) {
        fclose(f);
        return 0;
    }

    // Check that the file actually has an ID3v2 tag first.
    if (memcmp(id3_header, "ID3", 3) != 0) {
        fclose(f);
        return 0;  // No ID3v2 tag found
    }

    // Calculate the size of the ID3 block (Synchsafe integer: 4 bytes of 7 bits each)
    unsigned int id3_size = ((id3_header[6] & 0x7F) << 21) |
                            ((id3_header[7] & 0x7F) << 14) |
                            ((id3_header[8] & 0x7F) << 7)  |
                             (id3_header[9] & 0x7F);

    // Allocate a buffer for the entire ID3 tag
    unsigned char *buffer = malloc(id3_size);
    if (!buffer) {
        fclose(f);
        return 0;
    }

    if (fread(buffer, 1, id3_size, f) != id3_size) {
        free(buffer);
        fclose(f);
        return 0;
    }

    unsigned int ptr = 0;
    int track_num = -1;

    // Scan through the ID3 block for the "TRCK" frame
    while (ptr + 10 < id3_size) {
        // A frame header is 10 bytes (4 bytes name, 4 bytes size, 2 bytes flags)
        unsigned char *frame_id = &buffer[ptr];

        // Calculate the size of this specific metadata field (Big Endian)
        // Note: ID3v2.4 uses synchsafe here, while ID3v2.3 uses standard bytes.
        // This general shifting approach works perfectly for standard MP3 files.
        unsigned int frame_size = (frame_id[4] << 24) | (frame_id[5] << 16) | 
                                  (frame_id[6] << 8)  | frame_id[7];

        // If we encounter zeros, we have reached the end (padding) of the ID3 tag.
        if (frame_id[0] == 0) break;

        //Looking for "TRCK" (Track number)
        if (memcmp(frame_id, "TRCK", 4) == 0) {
            if (ptr + 10 + frame_size <= id3_size) {
            // The first byte inside the text field indicates text encoding (usually 0 for ISO or 1 for Unicode)
            // We skip that encoding byte (+1)
                unsigned char *text_data = &buffer[ptr + 10 + 1];
                unsigned int text_len = frame_size - 1;

                // Create a safe null-terminated string
                char track_str[16] = {0};
                if (text_len > 15) text_len = 15;
                memcpy(track_str, text_data, text_len);

                // atoi handles formats like "5" or "5/12" (stops at the slash)
                track_num = atoi(track_str);
            }
            break; // Found it! We can end the loop.
        }

        // Move on to the next frame
        ptr += 10 + frame_size;
    }

    free(buffer);
    fclose(f);
    return track_num;
}

/* -------------------------------------------------- */
/* Find track number in meta data for flac            */
/* -------------------------------------------------- */

static int get_flac_track(const char *filepath)
{

    struct stat stats;

    stat(filepath, &stats);
    if (S_ISDIR(stats.st_mode)) // Should not happen
        return -1;

    FILE *f = fopen(filepath, "rb");
    if (!f) return -1;

    char header[4];
    if (fread(header, 1, 4, f) != 4 || memcmp(header, "fLaC", 4) != 0) {
        fclose(f);
        return 0;  // Not a valid FLAC file
    }

    int is_last = 0;
    while (!is_last) {
        unsigned char block_header[4];
        if (fread(block_header, 1, 4, f) != 4) break;

        is_last = block_header[0] & 0x80;
        int block_type = block_header[0] & 0x7F;

        // Retrieve the 3 size bytes correctly (Big Endian)
        unsigned int block_size = (block_header[1] << 16) | (block_header[2] << 8) | block_header[3];

        if (block_type == 4) { // VORBIS_COMMENT block
            unsigned char *buffer = malloc(block_size);
            if (!buffer) { fclose(f); return 0; }

            if (fread(buffer, 1, block_size, f) == block_size) {
                unsigned int ptr = 0;

                // Skip the Vendor String (first there are 4 bytes for the length, then the string itself)
                if (ptr + 4 > block_size) { free(buffer); break; }
                unsigned int vendor_len = buffer[ptr] | (buffer[ptr+1] << 8) | (buffer[ptr+2] << 16) | (buffer[ptr+3] << 24); // Little Endian!
                ptr += 4 + vendor_len;

                // Retrieve number of comments/tags (4 bytes Little Endian)
                if (ptr + 4 > block_size) { free(buffer); break; }
                unsigned int user_comment_list_length = buffer[ptr] | (buffer[ptr+1] << 8) | (buffer[ptr+2] << 16) | (buffer[ptr+3] << 24);
                ptr += 4;

                //  Loop through all tags safely
                for (unsigned int i = 0; i < user_comment_list_length; i++) {
                    if (ptr + 4 > block_size) break;
                    unsigned int comment_len = buffer[ptr] | (buffer[ptr+1] << 8) | (buffer[ptr+2] << 16) | (buffer[ptr+3] << 24);
                    ptr += 4;

                    if (ptr + comment_len > block_size) break;

                    // Check whether this tag begins with "TRACK NUMBER=" (case-insensitive)
                    if (comment_len > 12 && strncasecmp((char*)&buffer[ptr], "TRACKNUMBER=", 12) == 0) {
                        // Create a temporary null-terminated string for atoi
                        char track_str[16] = {0};
                        unsigned int val_len = comment_len - 12;
                        if (val_len > 15) val_len = 15;

                        memcpy(track_str, &buffer[ptr + 12], val_len);
                        int track_num = atoi(track_str);

                        free(buffer);
                        fclose(f);
                        return track_num;
                    }

                    ptr += comment_len; // Go to next tag
                }
            }
            free(buffer);
            break; // We have finished reading the comments section.
        } else {
            // Skip the block if it is not VORBIS_COMMENT
            fseek(f, block_size, SEEK_CUR);
        }
    }

    fclose(f);

    return 0;
}

/* -------------------------------------------------- */
/* Find media files function                          */
/* -------------------------------------------------- */

int scandirs(Playlist *plist)
{
    int rval = 0;
    int sqtrack_no = 0;
    int folder_count = 0;
    char q_path[PATH_MAX];

    struct stat stats;

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

            if (strcmp(p, "flac") != 0 && strcmp(p, "mp3") != 0 && strcmp(p, "wav") != 0) {
                continue;   // Ignore other file types completely
            }

        }

        stat(q_path, &stats);
        if (S_ISDIR(stats.st_mode)) {
            folder_count++;
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

        if (!S_ISDIR(stats.st_mode))
            plist->seq_track_no[plist->count] = ++sqtrack_no;

#if 0
        // Diagnostics
        if (!S_ISDIR(stats.st_mode))
        printf("DEBUG: [%zu] ptr=%p name=%s  fallback track_no=%d   item=%d\n",
               plist->count,
               (void *)plist->files[plist->count],
               plist->files[plist->count], plist->seq_track_no[plist->count] , sqtrack_no);
#endif

        plist->count++;
    }

    closedir(dir);
    dir = NULL;

    /* Sorting Section */
    typedef struct {
        char *filename;
        int track;
    } TrackItem;

    // Comparison function for qsort
    int compare_tracks(const void *a, const void *b) {
        return ((TrackItem*)a)->track - ((TrackItem*)b)->track;
    }

    // Allocate a temporary array for the sorting, based on the number of files.
    TrackItem *sort_list = malloc(sizeof(TrackItem) * plist->count+folder_count);

    int valid_track_count = 0;

    for (int i = 0; i < plist->count; i++) {

        snprintf(q_path, sizeof(q_path), "%s%s", plist->path, plist->files[i]);

        char *ext = strrchr(q_path, '.');
        int track_no = -1;

        if (ext != NULL) {
            if (strcmp(ext, ".flac") == 0) {
                track_no = get_flac_track(q_path);
            } else if (strcmp(ext, ".mp3") == 0) {
                track_no = get_mp3_track(q_path);
            } else if (strcmp(ext, ".wav") == 0) {
                track_no = get_wav_track(q_path);
            }
        }

        if (track_no <= 0)
            track_no = get_track_from_filename(plist->files[i]);

        stat(q_path, &stats);

        if (track_no <= 0 && !S_ISDIR(stats.st_mode)) {
            track_no = plist->seq_track_no[i];
            SDL_Log("doMedia(sort): Using sequential track no %d since it is not found in meta data for file %s", track_no, plist->files[i]);
        }

        // Simple sort. Pick the first letter a-z-A-Z - usually the artist or album name
        if (S_ISDIR(stats.st_mode)) {
            track_no = tolower(plist->files[i][0]);
        }

        if (track_no > 0) {
//            printf("TRACK for %s is %d\n", plist->files[i], track_no);
            // Move the pointer to the filename and track number in our sorting list
            sort_list[valid_track_count].filename = plist->files[i];
            sort_list[valid_track_count].track = track_no;
            valid_track_count++;
        }
    }

    // Sort the list using qsort
    qsort(sort_list, valid_track_count, sizeof(TrackItem), compare_tracks);

    // Overwrite your original plist->files with the new, sorted order
    // (We are simply copying the pointers back, which is lightning-fast)
    for (int i = 0; i < valid_track_count; i++) {
        plist->files[i] = sort_list[i].filename;
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
        printf("File %d: %s\n", i + 1, plist->files[i]);
    }
#endif

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

/* ------------------------------------------------------------------------ */
/*                             Player Section                               */
/* ------------------------------------------------------------------------ */


static AudioBuffer audio;
static CommandQueue commands;
static SDL_AudioDeviceID audio_device;

RTSPAudioBuffer rtsp_audio;

/* ---------------------------------------------------------------- */
/* Initialize things to start with                                  */
/* ---------------------------------------------------------------- */

static void init_rtsp_audio(AudioProps *apropps)
{
    memset(&rtsp_audio, 0, sizeof(RTSPAudioBuffer));

    rtsp_audio.enabled = 1; // Activate the stream
    rtsp_audio.muted = 1;   // Make sure it is muted until user wants it

    // Mutex used in the callback and FFmpeg thread.
    rtsp_audio.mutex = apropps->commands.rtsp_mutex;
}

void player_init(AudioProps *apropps)
{
    audio = apropps->audio;
    commands = apropps->commands;
    audio_device = apropps->audio_device;
    av_log_set_level(AV_LOG_ERROR);

    init_rtsp_audio(apropps);
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
    // Reset the flow (silence as a foundation)
    memset(stream, 0, len);

    // Retrieve and copy data from the regular app audio buffer
    SDL_LockMutex(audio.mutex);

    int n = audio.count;
    if (n > len) n = len;

    int first = RING_SIZE - audio.read_pos;
    if (first > n) first = n;

    memcpy(stream, audio.data + audio.read_pos, first);
    memcpy(stream + first, audio.data, n - first);

    audio.read_pos = (audio.read_pos + n) % RING_SIZE;
    audio.count -= n;

    SDL_UnlockMutex(audio.mutex);

    //  MIX IN THE RTSP CAMERA'S AUDIO
    if (rtsp_audio.enabled && !rtsp_audio.muted)
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

    // printf("index=%d    playlist->count=%d   file=%s\n", index, playlist->count, playlist->files[index]);

    if (index < 0 || index >= playlist->count)
        return -1;

    *job = calloc(1, sizeof(DecoderJob));

    if (!*job)
        return -1;

    (*job)->filename = playlist->files[index+playlist->kalle];

    SDL_AtomicSet(&(*job)->cancel, 0);

    *thread = SDL_CreateThread(
        decoder_thread, "decoder", *job);

    if (!*thread)
    {
        free(*job);
        *job = NULL;
        return -1;
    }

    SDL_PauseAudioDevice(audio_device, 0);

    playlist->kalle = 0;
    return 0;
}

int control_thread(void *arg)
{
    Playlist *playlist = arg;

    DecoderJob *job = NULL;
    SDL_Thread *decoder = NULL;

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

