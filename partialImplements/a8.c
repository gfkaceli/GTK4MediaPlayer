#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <pthread.h>
#include <pulse/error.h>
#include <pulse/gccmacro.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define INBUF_SIZE                                                             \
  8192 // Adjust this based on expected audio bitrate and buffer duration

// Circular Buffer Definition for Audio Data
typedef struct {
  uint8_t *buffer;      // Buffer to store audio data
  size_t size;           // Maximum size of the buffer in bytes
  size_t write_pos;      // Current write position in the buffer
  size_t read_pos;       // Current read position in the buffer
  size_t count;          // Current number of bytes in the buffer
  pthread_mutex_t mutex; // Mutex for synchronizing access to the buffer
  pthread_cond_t
      not_full; // Condition variable to wait for when buffer is not full
  pthread_cond_t
      not_empty; // Condition variable to wait for when buffer is not empty
} CircularBuffer;

// Global variables for synchronization and state management
volatile int is_running = 1;
volatile int decode_finished = 0; // New flag to indicate decoding is done
CircularBuffer cb;

// Function Prototypes
void *decode_thread_function(void *args);
void *playback_thread_function(void *args);
void circularBufferInit(CircularBuffer *cb, size_t size);
void circularBufferDestroy(CircularBuffer *cb);
bool circularBufferPush(CircularBuffer *cb, const uint8_t *data, size_t bytes);
bool circularBufferPop(CircularBuffer *cb, uint8_t *data, size_t bytes);

// DecodeData and PulseAudioPlayback structures are no longer needed in the
// modified example

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <input_audio_file>\n", argv[0]);
    exit(EXIT_FAILURE);
  }

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

  // Wait for the decode thread to finish
  pthread_join(decode_thread, NULL);
  // Set is_running to 0 if not already set, to ensure playback thread can exit
  is_running = 0;
  pthread_cond_signal(
      &cb.not_empty); // Make sure to wake up the playback thread
  // Wait for the playback thread to finish
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

    size_t space_available = cb->size - cb->count;

    while (bytes > 0) {
        while (space_available == 0) {
            // Buffer is full, wait until there is space
            pthread_cond_wait(&cb->not_full, &cb->mutex);
            space_available = cb->size - cb->count; // Recalculate space available after waiting
        }

        size_t bytes_to_write = bytes < space_available ? bytes : space_available;
        size_t bytesToEnd = cb->size - cb->write_pos; // Bytes from write_pos to the end of the buffer

        if (bytes_to_write <= bytesToEnd) {
            // If all data fits before the end of the buffer, copy in one go
            memcpy(cb->buffer + cb->write_pos, data, bytes_to_write);
        } else {
            // Data needs to wrap around the buffer end
            memcpy(cb->buffer + cb->write_pos, data, bytesToEnd);
            memcpy(cb->buffer, data + bytesToEnd, bytes_to_write - bytesToEnd);
        }

        // Update write position, count, and bytes left to write
        cb->write_pos = (cb->write_pos + bytes_to_write) % cb->size;
        cb->count += bytes_to_write;
        bytes -= bytes_to_write;
        data += bytes_to_write;
        space_available = cb->size - cb->count; // Recalculate space available

        // Signal that the buffer is not empty
        pthread_cond_signal(&cb->not_empty);
    }

    pthread_mutex_unlock(&cb->mutex);
    return true;
}

bool circularBufferPop(CircularBuffer *cb, uint8_t *data, size_t bytes) {
    pthread_mutex_lock(&cb->mutex);

    while (cb->count < bytes) {
        if (!is_running && decode_finished) { // Exit condition if decoding finished and buffer empty
            pthread_mutex_unlock(&cb->mutex);
            return false;
        }
        // Wait for more data to be available
        pthread_cond_wait(&cb->not_empty, &cb->mutex);
    }

    size_t bytes_to_read = bytes <= cb->count ? bytes : cb->count; // Read only as much as available

    for (size_t i = 0; i < bytes_to_read; i++) {
        data[i] = cb->buffer[(cb->read_pos + i) % cb->size];
    }
    cb->read_pos = (cb->read_pos + bytes_to_read) % cb->size;
    cb->count -= bytes_to_read;

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
    SwrContext *swr_ctx = NULL;

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

    if (!codec_context->channel_layout) {
        codec_context->channel_layout = av_get_default_channel_layout(codec_context->channels);
    }

    if (avcodec_open2(codec_context, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        avcodec_free_context(&codec_context);
        avformat_close_input(&format_context);
        return NULL;
    }

    swr_ctx = swr_alloc_set_opts(NULL,
                                 AV_CH_LAYOUT_STEREO,
                                 AV_SAMPLE_FMT_S16,
                                 44100,
                                 codec_context->channel_layout,
                                 codec_context->sample_fmt,
                                 codec_context->sample_rate,
                                 0, NULL);

    if (!swr_ctx || swr_init(swr_ctx) < 0) {
        fprintf(stderr, "Could not initialize the resampling context\n");
        if (swr_ctx) swr_free(&swr_ctx);
        avcodec_free_context(&codec_context);
        avformat_close_input(&format_context);
        return NULL;
    }

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!pkt || !frame) {
        fprintf(stderr, "Could not allocate packet or frame\n");
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        swr_free(&swr_ctx);
        avcodec_free_context(&codec_context);
        avformat_close_input(&format_context);
        return NULL;
    }

    // Allocate an output buffer large enough to hold the resampled data
    uint8_t *output_buffer = NULL;
    int output_buffer_size = av_samples_get_buffer_size(frame->linesize, 2, 44100, AV_SAMPLE_FMT_S16, 0);
    output_buffer = (uint8_t *)av_malloc(output_buffer_size);

// Ensure output_buffer allocation was successful...
av_samples_alloc(&output_buffer, NULL, 2, 44100, AV_SAMPLE_FMT_S16, 0);

swr_convert(swr_ctx, &output_buffer, 44100,
            (const uint8_t **)frame->data, frame->nb_samples);

  while (av_read_frame(format_context, pkt) >= 0 && is_running) {
    if (pkt->stream_index == audio_stream_index) {
      if (avcodec_send_packet(codec_context, pkt) == 0) {
        while (avcodec_receive_frame(codec_context, frame) == 0) {
            // After successful decoding:
            int num_samples = swr_convert(swr_ctx, &output_buffer, 44100,
                              (const uint8_t **)frame->data, frame->nb_samples);

            if (num_samples < 0) {
              fprintf(stderr, "Error in resampling audio frame\n");
            } else {// the following line is for logging only 
              fprintf(stdout, "Resampled %d samples per channel successfully.\n", num_samples);
            }
            // Calculate the size of the resampled data
            int data_size = av_samples_get_buffer_size(NULL, 2, num_samples,
                                               AV_SAMPLE_FMT_S16, 1);
            if (data_size < 0) {
              fprintf(stderr, "Could not calculate data size for resampled audio frame\n");
            } else {// the following lines are for logging
                fprintf(stdout, "Resampled data size: %d bytes.\n", data_size);

              // Optionally log the first few bytes of the resampled data
                fprintf(stdout, "Resampled data start: %02x %02x %02x %02x...\n",
                output_buffer[0], output_buffer[1], output_buffer[2], output_buffer[3]);
            }


            circularBufferPush(&cb, output_buffer, data_size);
        }
      }
      av_packet_unref(pkt);
    }
  }

  // Cleanup
  av_frame_free(&frame);
  //av_frame_free(&resampled_frame);
  av_packet_free(&pkt);
  avcodec_free_context(&codec_context);
  avformat_close_input(&format_context);
  if (swr_ctx)
    swr_free(&swr_ctx);
  av_freep(&output_buffer);

  is_running = 0;                     // Signal playback thread to terminate
  decode_finished = 1;                // Decoding is complete
  pthread_cond_signal(&cb.not_empty); // Wake up the playback thread if it's
                                      // waiting for more data
  return NULL;
}

// Other includes and definitions

void *playback_thread_function(void *args) {
  pa_simple *s = NULL;
  int error;
  pa_sample_spec ss = {.format = PA_SAMPLE_S16LE, .rate = 44100, .channels = 2};

  s = pa_simple_new(NULL, "AudioPlayer", PA_STREAM_PLAYBACK, NULL, "Playback",
                    &ss, NULL, NULL, &error);
  if (!s) {
    fprintf(stderr, "PulseAudio simple connection failed: %s\n",
            pa_strerror(error));
    return NULL;
  }

  // This might need fine-tuning based on actual audio data
  uint8_t *buffer = (uint8_t *)malloc(INBUF_SIZE);
  if (!buffer) {
    fprintf(stderr, "Failed to allocate playback buffer\n");
    pa_simple_free(s);
    return NULL;
  }

  // Adjusted loop condition to ensure playback continues until all conditions
  // are met
  while (!decode_finished || (decode_finished && cb.count > 0)) {
    if (cb.count > 0) {
      if (!circularBufferPop(&cb, buffer, INBUF_SIZE)) {
        fprintf(stderr,
                "Playback thread: Buffer empty or waiting for more data\n");
            exit(1);
        // If decode has finished and buffer is empty, exit loop
        if (decode_finished && cb.count == 0)
          break;
        continue;
      }

      if (pa_simple_write(s, buffer, INBUF_SIZE, &error) < 0) {
        fprintf(stderr, "PulseAudio playback failed: %s\n", pa_strerror(error));
        break;
      }
    } else if (decode_finished) {
      // If no more data is coming, break the loop
      break;
    }
  }

  if (pa_simple_drain(s, &error) < 0) {
    fprintf(stderr, "Failed to drain PulseAudio: %s\n", pa_strerror(error));
  }

  free(buffer);
  pa_simple_free(s);
  printf("Playback thread: Exiting playback function after draining.\n");
  return NULL;
}
