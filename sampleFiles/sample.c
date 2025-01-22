#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <pthread.h>
#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/gccmacro.h>


#define INBUF_SIZE 2048 * 2048 // Adjust this based on expected audio bitrate and buffer duration

// Circular Buffer Definition for Audio Data
typedef struct {
    uint8_t *buffer;             // Buffer to store audio data
    size_t size;                 // Maximum size of the buffer in bytes
    size_t write_pos;            // Current write position in the buffer
    size_t read_pos;             // Current read position in the buffer
    size_t count;                // Current number of bytes in the buffer
    pthread_mutex_t mutex;       // Mutex for synchronizing access to the buffer
    pthread_cond_t not_full;     // Condition variable to wait for when buffer is not full
    pthread_cond_t not_empty;    // Condition variable to wait for when buffer is not empty
} CircularBuffer;

// Global variables for synchronization and state management
volatile int is_running = 1;
CircularBuffer cb; // Global circular buffer access

// Function Prototypes
void *decode_thread_function(void *args);
void *playback_thread_function(void *args);
void circularBufferInit(CircularBuffer *cb, size_t size);
void circularBufferDestroy(CircularBuffer *cb);
bool circularBufferPush(CircularBuffer *cb, const uint8_t *data, size_t bytes);
bool circularBufferPop(CircularBuffer *cb, uint8_t *data, size_t bytes);

// DecodeData and PulseAudioPlayback structures are no longer needed in the modified example

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input_audio_file>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Initialize FFmpeg
    avformat_network_init();

    // Initialize circular buffer for audio samples
    circularBufferInit(&cb, INBUF_SIZE);

    // Create and start the decoding thread
    pthread_t decode_thread;
    if (pthread_create(&decode_thread, NULL, decode_thread_function, argv[1])) {
        fprintf(stderr, "Error creating decoding thread\n");
        return -1;
    }

    // Create and start the playback thread
    pthread_t playback_thread;
    if (pthread_create(&playback_thread, NULL, playback_thread_function, NULL)) {
        fprintf(stderr, "Error creating playback thread\n");
        return -1;
    }

    // Wait for threads to finish
    pthread_join(decode_thread, NULL);
    pthread_join(playback_thread, NULL);

    // Cleanup
    circularBufferDestroy(&cb);
    avformat_network_deinit();

    return 0;
}

void circularBufferInit(CircularBuffer *cb, size_t size) {
    cb->size = size;
    cb->write_pos = 0;
    cb->read_pos = 0;
    cb->count = 0;
    cb->buffer = (uint8_t *)malloc(size);
    pthread_mutex_init(&cb->mutex, NULL);
    pthread_cond_init(&cb->not_full, NULL);
    pthread_cond_init(&cb->not_empty, NULL);
}

void circularBufferDestroy(CircularBuffer *cb) {
    free(cb->buffer);
    pthread_mutex_destroy(&cb->mutex);
    pthread_cond_destroy(&cb->not_full);
    pthread_cond_destroy(&cb->not_empty);
}

bool circularBufferPush(CircularBuffer *cb, const uint8_t *data, size_t bytes) {
    pthread_mutex_lock(&cb->mutex);

    if (cb->count + bytes > cb->size) {
        // Buffer is full, handle this case as needed
        pthread_mutex_unlock(&cb->mutex);
        return false; // Or implement a waiting mechanism
    }

    for (size_t i = 0; i < bytes; i++) {
        cb->buffer[(cb->write_pos + i) % cb->size] = data[i];
    }
    cb->write_pos = (cb->write_pos + bytes) % cb->size;
    cb->count += bytes;

    pthread_cond_signal(&cb->not_empty);
    pthread_mutex_unlock(&cb->mutex);
    return true;
}

bool circularBufferPop(CircularBuffer *cb, uint8_t *data, size_t bytes) {
    pthread_mutex_lock(&cb->mutex);
    while (cb->count < bytes) {
    // Buffer is empty, wait for data
    pthread_cond_wait(&cb->not_empty, &cb->mutex);
    if (!is_running) { // Check for exit condition when waking up
        pthread_mutex_unlock(&cb->mutex);
        return false;
    }
}

for (size_t i = 0; i < bytes; i++) {
    data[i] = cb->buffer[(cb->read_pos + i) % cb->size];
}
cb->read_pos = (cb->read_pos + bytes) % cb->size;
cb->count -= bytes;

pthread_cond_signal(&cb->not_full); // Signal that buffer has space
pthread_mutex_unlock(&cb->mutex);
return true;
}
void *decode_thread_function(void *args) {
const char *input_filename = (const char *)args;
AVFormatContext *format_context = NULL;
int audio_stream_index = -1;
AVCodecContext *codec_context = NULL;
const AVCodec *codec = NULL;
if (avformat_open_input(&format_context, input_filename, NULL, NULL) < 0) {
    fprintf(stderr, "Could not open input file '%s'\n", input_filename);
    return NULL;
}

if (avformat_find_stream_info(format_context, NULL) < 0) {
    fprintf(stderr, "Could not find stream information\n");
    avformat_close_input(&format_context);
    return NULL;
}

for (int i = 0; i < format_context->nb_streams; i++) {
    if (format_context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        audio_stream_index = i;
        break;
    }
}

if (audio_stream_index == -1) {
    fprintf(stderr, "Could not find an audio stream\n");
    avformat_close_input(&format_context);
    return NULL;
}

codec = avcodec_find_decoder(format_context->streams[audio_stream_index]->codecpar->codec_id);
if (!codec) {
    fprintf(stderr, "Codec not found\n");
    avformat_close_input(&format_context);
    return NULL;
}

codec_context = avcodec_alloc_context3(codec);
if (!codec_context) {
    fprintf(stderr, "Could not allocate audio codec context\n");
    avformat_close_input(&format_context);
    return NULL;
}

if (avcodec_parameters_to_context(codec_context, format_context->streams[audio_stream_index]->codecpar) < 0) {
    fprintf(stderr, "Could not copy codec context\n");
    avcodec_free_context(&codec_context);
    avformat_close_input(&format_context);
    return NULL;
}

if (avcodec_open2(codec_context, codec, NULL) < 0) {
    fprintf(stderr, "Could not open codec\n");
    avcodec_free_context(&codec_context);
    avformat_close_input(&format_context);
    return NULL;
}

AVPacket *pkt = av_packet_alloc();
AVFrame *frame = av_frame_alloc();
if (!pkt || !frame) {
    fprintf(stderr, "Could not allocate packet or frame\n");
    avcodec_free_context(&codec_context);
    avformat_close_input(&format_context);
    if (pkt) av_packet_free(&pkt);
    if (frame) av_frame_free(&frame);
    return NULL;
}

while (av_read_frame(format_context, pkt) >= 0 && is_running) {
    if (pkt->stream_index == audio_stream_index) {
        if (avcodec_send_packet(codec_context, pkt) == 0) {
            while (avcodec_receive_frame(codec_context, frame) == 0) {
                // Convert frame to desired audio format and sample rate, then push to buffer
                // For simplicity, assuming frame->data contains audio samples in a format that can be directly pushed
                size_t data_size = av_get_bytes_per_sample(codec_context->sample_fmt) * frame->nb_samples * codec_context->channels;
                if (!circularBufferPush(&cb, (const uint8_t *)frame->data[0], data_size)) {
                    fprintf(stderr, "Buffer full, dropping frame\n");
                }
            }
        }
        av_packet_unref(pkt);
    }
}

// Cleanup
av_frame_free(&frame);
av_packet_free(&pkt);
avcodec_free_context(&codec_context);
avformat_close_input(&format_context);

is_running = 0; // Signal playback thread to terminate
pthread_cond_signal(&cb.not_empty); // Ensure playback thread can exit if waiting
return NULL;
}
#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/gccmacro.h>

// Other includes and definitions

void *playback_thread_function(void *args) {
    pa_simple *s = NULL;
    int error;
    // Correctly define sample format and rate based on your audio format
    pa_sample_spec ss = {
        .format = PA_SAMPLE_S16LE, // Assuming the format is 16-bit little-endian
        .rate = 44100,             // Sample rate
        .channels = 2              // Number of channels (stereo)
    };

    // Create a new PulseAudio simple connection
    s = pa_simple_new(NULL,                // Use the default server
                      "AudioPlayer",       // Application name
                      PA_STREAM_PLAYBACK,
                      NULL,                // Use the default device
                      "Playback",          // Stream description
                      &ss,                 // Sample format
                      NULL,                // Use default channel map
                      NULL,                // Use default buffering attributes
                      &error               // Error code
    );
    if (!s) {
        fprintf(stderr, "PulseAudio simple connection failed: %s\n", pa_strerror(error));
        return NULL;
    }

    size_t buffer_length = 4096; // Adjust based on needs
    uint8_t *buffer = (uint8_t *)malloc(buffer_length);
    if (!buffer) {
        fprintf(stderr, "Failed to allocate playback buffer\n");
        if (s) pa_simple_free(s);
        return NULL;
    }

    while (is_running) {
        if (!circularBufferPop(&cb, buffer, buffer_length)) {
            // Handle buffer underflow or wait
            continue;
        }

        // Playback the audio data
        if (pa_simple_write(s, buffer, buffer_length, &error) < 0) {
            fprintf(stderr, "PulseAudio playback failed: %s\n", pa_strerror(error));
            break;
        }
    }

    // Drain to make sure all audio data is played
    if (pa_simple_drain(s, &error) < 0) {
        fprintf(stderr, "PulseAudio drain failed: %s\n", pa_strerror(error));
    }

    free(buffer);
    if (s) pa_simple_free(s);

    return NULL;
}
