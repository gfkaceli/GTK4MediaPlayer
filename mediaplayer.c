#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIDEO_BUFFER_SIZE 20
#define AUDIO_BUFFER_SIZE 8192

volatile int is_running = 1;
volatile int is_paused = 0; // Added to track pause state

typedef struct {
  char *input_filename;
  int frame_rate;
  GdkPixbuf *pixbuf; // Pointer to the GdkPixbuf for displaying the image
  // Add synchronization primitives if needed, e.g., mutex, condition variable
} DecodeData;

// Circular buffer for video
typedef struct {
    GdkPixbuf **pixbufs;
    int size, start, end, count;
    pthread_mutex_t mutex;
    pthread_cond_t notFull, notEmpty;
} VideoBuffer;

// Circular buffer for audio
typedef struct {
    uint8_t *buffer;
    size_t size, write_pos, read_pos, count;
    pthread_mutex_t mutex;
    pthread_cond_t notFull, notEmpty;
} AudioBuffer;

// Global buffers
VideoBuffer videoBuffer;
AudioBuffer audioBuffer;

// Function prototypes
void videoBufferInit(VideoBuffer *vb, int size);
void audioBufferInit(AudioBuffer *ab, size_t size);
void videoBufferDestroy(VideoBuffer *vb);
void audioBufferDestroy(AudioBuffer *ab);
bool videoBufferPush(VideoBuffer *vb, GdkPixbuf *pixbuf);
bool videoBufferPop(VideoBuffer *vb, GdkPixbuf **pixbuf);
bool audioBufferPush(AudioBuffer *ab, const uint8_t *data, size_t bytes);
bool audioBufferPop(AudioBuffer *ab, uint8_t *data, size_t bytes);
void *videoThread(void *args);
void *audioThread(void *args);
gboolean updateDisplay(GtkWidget *imageWidget);
void onWindowDestroy(GtkWidget *widget, gpointer app);
void activate(GtkApplication *app, gpointer user_data);

// Pause and Resume Control
void togglePause() {
    is_paused = !is_paused;
}

// Update Threads to Handle Pause State
bool checkPauseState() {
    while (is_paused && is_running) {
        usleep(10000);
        pthread_cond_broadcast(&videoBuffer.notEmpty);
        pthread_cond_broadcast(&audioBuffer.notEmpty);
        pthread_cond_broadcast(&videoBuffer.notFull);
        pthread_cond_broadcast(&audioBuffer.notFull);
    }
    return is_running;
}

// Circular Buffer Functions for Video
void videoBufferInit(VideoBuffer *vb, int size) {
    vb->pixbufs = malloc(size * sizeof(GdkPixbuf *));
    vb->size = size;
    vb->start = vb->end = vb->count = 0;
    pthread_mutex_init(&vb->mutex, NULL);
    pthread_cond_init(&vb->notFull, NULL);
    pthread_cond_init(&vb->notEmpty, NULL);
}

void videoBufferDestroy(VideoBuffer *vb) {
    for (int i = 0; i < vb->count; i++) {
        g_object_unref(vb->pixbufs[(vb->start + i) % vb->size]);
    }
    free(vb->pixbufs);
    pthread_mutex_destroy(&vb->mutex);
    pthread_cond_destroy(&vb->notFull);
    pthread_cond_destroy(&vb->notEmpty);
}

bool videoBufferPush(VideoBuffer *vb, GdkPixbuf *pixbuf) {
    pthread_mutex_lock(&vb->mutex);
    while (vb->count == vb->size) {
        pthread_cond_wait(&vb->notFull, &vb->mutex);
    }
    vb->pixbufs[vb->end] = g_object_ref(pixbuf);
    vb->end = (vb->end + 1) % vb->size;
    vb->count++;
    pthread_cond_signal(&vb->notEmpty);
    pthread_mutex_unlock(&vb->mutex);
    return true;
}

bool videoBufferPop(VideoBuffer *vb, GdkPixbuf **pixbuf) {
    pthread_mutex_lock(&vb->mutex);

    while (vb->count == 0 && is_running) {
        if (is_paused) {
            pthread_mutex_unlock(&vb->mutex);
            checkPauseState(); // Wait while paused
            pthread_mutex_lock(&vb->mutex);
        }
        pthread_cond_wait(&vb->notEmpty, &vb->mutex);
    }

    if (!is_running) {
        pthread_mutex_unlock(&vb->mutex);
        return false;
    }

    *pixbuf = vb->pixbufs[vb->start];
    vb->start = (vb->start + 1) % vb->size;
    vb->count--;
    pthread_cond_signal(&vb->notFull); // Notify that buffer space is available
    pthread_mutex_unlock(&vb->mutex);
    return true;
}

// Circular Buffer Functions for Audio
void audioBufferInit(AudioBuffer *ab, size_t size) {
    ab->buffer =  (uint8_t *)malloc(size);
    ab->size = size;
    ab->write_pos = 0;
    ab->read_pos = 0;
    ab->count = 0;
    pthread_mutex_init(&ab->mutex, NULL);
    pthread_cond_init(&ab->notFull, NULL);
    pthread_cond_init(&ab->notEmpty, NULL);
}

void audioBufferDestroy(AudioBuffer *ab) {
    free(ab->buffer);
    pthread_mutex_destroy(&ab->mutex);
    pthread_cond_destroy(&ab->notFull);
    pthread_cond_destroy(&ab->notEmpty);
}

bool audioBufferPush(AudioBuffer *ab, const uint8_t *data, size_t bytes) {
    pthread_mutex_lock(&ab->mutex);

    size_t space_available = ab->size - ab->count;

    while (bytes > 0) {
        while (space_available == 0) {
            // Buffer is full, wait until there is space
            pthread_cond_wait(&ab->notFull, &ab->mutex);
            space_available = ab->size - ab->count; // Recalculate space available after waiting
        }

        size_t bytes_to_write = bytes < space_available ? bytes : space_available;
        size_t bytesToEnd = ab->size - ab->write_pos; // Bytes from write_pos to the end of the buffer

        if (bytes_to_write <= bytesToEnd) {
            // If all data fits before the end of the buffer, copy in one go
            memcpy(ab->buffer + ab->write_pos, data, bytes_to_write);
        } else {
            // Data needs to wrap around the buffer end
            memcpy(ab->buffer + ab->write_pos, data, bytesToEnd);
            memcpy(ab->buffer, data + bytesToEnd, bytes_to_write - bytesToEnd);
        }

        // Update write position, count, and bytes left to write
        ab->write_pos = (ab->write_pos + bytes_to_write) % ab->size;
        ab->count += bytes_to_write;
        bytes -= bytes_to_write;
        data += bytes_to_write;
        space_available = ab->size - ab->count; // Recalculate space available

        // Signal that the buffer is not empty
        pthread_cond_signal(&ab->notEmpty);
    }

    pthread_mutex_unlock(&ab->mutex);
    return true;
}

bool audioBufferPop(AudioBuffer *ab, uint8_t *data, size_t bytes) {
    pthread_mutex_lock(&ab->mutex);

    while (ab->count < bytes && is_running) {
        if (is_paused) {
            pthread_mutex_unlock(&ab->mutex);
            checkPauseState(); // Wait while paused
            pthread_mutex_lock(&ab->mutex);
        }
        pthread_cond_wait(&ab->notEmpty, &ab->mutex);
    }

    if (!is_running) {
        pthread_mutex_unlock(&ab->mutex);
        return false;
    }

    size_t bytes_to_read = (bytes <= ab->count) ? bytes : ab->count;
    memcpy(data, ab->buffer + ab->read_pos, bytes_to_read);
    ab->read_pos = (ab->read_pos + bytes_to_read) % ab->size;
    ab->count -= bytes_to_read;

    pthread_cond_signal(&ab->notFull); // Notify that buffer space is available
    pthread_mutex_unlock(&ab->mutex);
    return true;
}


// Threads
/*
  Function videoThread
  argument that is passed to the pthread_create function 
  responsible for decoding the frame and pushing it onto the 
  circular buffer, also initializes the codec.
*/
void *videoThread(void *args) {
    DecodeData *data = (DecodeData *)args;
    const char *input_file = data->input_filename;

    AVFormatContext *format_context = NULL;
    AVCodecContext *codec_context = NULL;
    const AVCodec *codec = NULL;
    struct SwsContext *sws_ctx = NULL;
    int video_stream_index = -1;

    avformat_network_init();
    if (avformat_open_input(&format_context, input_file, NULL, NULL) > 0) {
        fprintf(stderr, "Error: Could not open input file '%s'\n", input_file);
        return NULL;
    }

    if (avformat_find_stream_info(format_context, NULL) < 0) {
        fprintf(stderr, "Error: Could not find stream information\n");
        avformat_close_input(&format_context);
        return NULL;
    }

    for (int i = 0; i < format_context->nb_streams; i++) {
        if (format_context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            break;
        }
    }

    if (video_stream_index == -1) {
        fprintf(stderr, "Error: No video stream found\n");
        avformat_close_input(&format_context);
        return NULL;
    }

    codec = avcodec_find_decoder(format_context->streams[video_stream_index]->codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "Error: Codec not found\n");
        avformat_close_input(&format_context);
        return NULL;
    }

    codec_context = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_context, format_context->streams[video_stream_index]->codecpar);
    if (avcodec_open2(codec_context, codec, NULL) < 0) {
        fprintf(stderr, "Error: Could not open codec\n");
        avcodec_free_context(&codec_context);
        avformat_close_input(&format_context);
        return NULL;
    }

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVFrame *rgb_frame = av_frame_alloc();
    if (!packet || !frame || !rgb_frame) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        av_packet_free(&packet);
        av_frame_free(&frame);
        av_frame_free(&rgb_frame);
        avcodec_free_context(&codec_context);
        avformat_close_input(&format_context);
        return NULL;
    }

    while (is_running) {
        if (is_paused) {
            checkPauseState();  // Wait while paused
            continue;
        }

        if (av_read_frame(format_context, packet) < 0) {
            break;  // Exit if no more frames to read
        }

        if (packet->stream_index == video_stream_index) {
            if (avcodec_send_packet(codec_context, packet) < 0) {
                fprintf(stderr, "Error: Failed to send packet for decoding\n");
                break;
            }

            while (avcodec_receive_frame(codec_context, frame) >= 0) {
                if (is_paused) {
                    checkPauseState();  // Wait while paused
                    continue;
                }

                if (!sws_ctx) {
                    sws_ctx = sws_getContext(
                        frame->width, frame->height, codec_context->pix_fmt,
                        frame->width, frame->height, AV_PIX_FMT_RGB24,
                        SWS_BILINEAR, NULL, NULL, NULL);
                }

                int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, frame->width, frame->height, 1);
                uint8_t *buffer = av_malloc(num_bytes * sizeof(uint8_t));
                av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, buffer,
                                     AV_PIX_FMT_RGB24, frame->width, frame->height, 1);

                sws_scale(sws_ctx, (const uint8_t *const *)frame->data, frame->linesize,
                          0, frame->height, rgb_frame->data, rgb_frame->linesize);

                GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(
                    rgb_frame->data[0], GDK_COLORSPACE_RGB, FALSE, 8,
                    frame->width, frame->height, rgb_frame->linesize[0],
                    (GdkPixbufDestroyNotify)av_free, buffer);

                if (pixbuf) {
                    videoBufferPush(&videoBuffer, pixbuf);
                    g_object_unref(pixbuf);
                } else {
                    av_free(buffer);
                }
            }
        }

        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    av_frame_free(&frame);
    av_frame_free(&rgb_frame);
    avcodec_free_context(&codec_context);
    avformat_close_input(&format_context);
    if (sws_ctx) {
        sws_freeContext(sws_ctx);
    }

    return NULL;
}

void *audioThread(void *args) {
    DecodeData *data = (DecodeData *)args;
    const char *input_file = data->input_filename;
    AVFormatContext *format_context = NULL;
    AVCodecContext *codec_context = NULL;
    const AVCodec *codec = NULL;
    SwrContext *swr_ctx = NULL;
    int audio_stream_index = -1;

    // Initialize FFmpeg
    avformat_network_init();
    if (avformat_open_input(&format_context, input_file, NULL, NULL) < 0) {
        fprintf(stderr, "Error: Could not open input file '%s'\n", input_file);
        return NULL;
    }
    if (avformat_find_stream_info(format_context, NULL) < 0) {
        fprintf(stderr, "Error: Could not find stream information\n");
        avformat_close_input(&format_context);
        return NULL;
    }

    // Find the audio stream
    for (int i = 0; i < format_context->nb_streams; i++) {
        if (format_context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_index = i;
            break;
        }
    }
    if (audio_stream_index == -1) {
        fprintf(stderr, "Error: Could not find an audio stream\n");
        avformat_close_input(&format_context);
        return NULL;
    }

    // Initialize the codec
    codec = avcodec_find_decoder(format_context->streams[audio_stream_index]->codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "Error: Codec not found\n");
        avformat_close_input(&format_context);
        return NULL;
    }
    codec_context = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_context, format_context->streams[audio_stream_index]->codecpar);
    if (avcodec_open2(codec_context, codec, NULL) < 0) {
        fprintf(stderr, "Error: Could not open codec\n");
        avcodec_free_context(&codec_context);
        avformat_close_input(&format_context);
        return NULL;
    }

    // Initialize resampler
    swr_ctx = swr_alloc_set_opts(
        NULL,
        AV_CH_LAYOUT_STEREO,        // Output: Stereo
        AV_SAMPLE_FMT_S16,          // Output: Signed 16-bit PCM
        44100,                      // Output: 44.1 kHz sample rate
        codec_context->channel_layout, // Input channel layout
        codec_context->sample_fmt,     // Input sample format
        codec_context->sample_rate,    // Input sample rate
        0, NULL);
    if (!swr_ctx || swr_init(swr_ctx) < 0) {
        fprintf(stderr, "Error: Could not initialize resampler\n");
        if (swr_ctx) swr_free(&swr_ctx);
        avcodec_free_context(&codec_context);
        avformat_close_input(&format_context);
        return NULL;
    }

    // Initialize PulseAudio for playback
    pa_simple *pulse = NULL;
    pa_sample_spec sample_spec = {
        .format = PA_SAMPLE_S16LE,
        .rate = 44100,
        .channels = 2,
    };
    int pulse_error;
    pulse = pa_simple_new(NULL, "MediaPlayer", PA_STREAM_PLAYBACK, NULL, "Audio", &sample_spec, NULL, NULL, &pulse_error);
    if (!pulse) {
        fprintf(stderr, "Error: PulseAudio initialization failed: %s\n", pa_strerror(pulse_error));
        swr_free(&swr_ctx);
        avcodec_free_context(&codec_context);
        avformat_close_input(&format_context);
        return NULL;
    }

    // Allocate buffers
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    uint8_t *output_buffer = av_malloc(44100 * 2 * 2); // 44.1 kHz, stereo, 16-bit samples
    if (!packet || !frame || !output_buffer) {
        fprintf(stderr, "Error: Could not allocate buffers\n");
        av_packet_free(&packet);
        av_frame_free(&frame);
        if (output_buffer) av_free(output_buffer);
        pa_simple_free(pulse);
        swr_free(&swr_ctx);
        avcodec_free_context(&codec_context);
        avformat_close_input(&format_context);
        return NULL;
    }

    // Main decoding loop
    while (is_running && av_read_frame(format_context, packet) >= 0) {
        if (!checkPauseState()) continue; // Check pause state
        if (packet->stream_index == audio_stream_index) {
            if (avcodec_send_packet(codec_context, packet) == 0) {
                while (avcodec_receive_frame(codec_context, frame) == 0) {
                    if (!checkPauseState()) break; // Handle pause during decoding
                    int num_samples = swr_convert(swr_ctx, &output_buffer, 44100,
                                                  (const uint8_t **)frame->data, frame->nb_samples);
                    if (num_samples < 0) {
                        fprintf(stderr, "Error: Audio resampling failed\n");
                        continue;
                    }

                    // Calculate the size of the resampled data
                    int data_size = num_samples * 2 * 2; // Stereo, 16-bit samples
                    if (pa_simple_write(pulse, output_buffer, data_size, &pulse_error) < 0) {
                        fprintf(stderr, "Error: PulseAudio write failed: %s\n", pa_strerror(pulse_error));
                    }
                }
            }
        }
        av_packet_unref(packet);
    }

    // Drain any remaining audio
    if (pa_simple_drain(pulse, &pulse_error) < 0) {
        fprintf(stderr, "Error: PulseAudio drain failed: %s\n", pa_strerror(pulse_error));
    }

    // Cleanup
    av_packet_free(&packet);
    av_frame_free(&frame);
    av_free(output_buffer);
    pa_simple_free(pulse);
    swr_free(&swr_ctx);
    avcodec_free_context(&codec_context);
    avformat_close_input(&format_context);

    return NULL;
}

// GTK Callbacks
/*
  Function update_display 
  continuously updates the display by popping images
  callback for our animation functionality
*/
gboolean updateDisplay(GtkWidget *image_widget) {
    if (!is_paused) { // Only update if playing
        GdkPixbuf *pixbuf;
        if (videoBufferPop(&videoBuffer, &pixbuf)) {
            GdkPaintable *paintable =  (GdkPaintable *)gdk_texture_new_for_pixbuf(pixbuf); // Convert to GdkPaintable
            gtk_image_set_from_paintable(GTK_IMAGE(image_widget), paintable); // Use updated function
            g_object_unref(paintable); // Decrease reference count of paintable
            g_object_unref(pixbuf); // Decrease reference count after setting it
        }
    }
    return G_SOURCE_CONTINUE;
}

static int command_line_cb(GtkApplication *app,
                           GApplicationCommandLine *cmdline,
                           gpointer user_data) {
  gchar **argv;
  gint argc;
  GError *error = NULL;

  // Get the arguments
  argv = g_application_command_line_get_arguments(cmdline, &argc);

  // Here, process the arguments. For example:
  for (int i = 0; i < argc; ++i) {
    g_print("Argument %d: %s\n", i, argv[i]);
  }

  // If your application has a GUI, you might want to activate it here
  g_application_activate(G_APPLICATION(app));

  // Free the arguments array
  g_strfreev(argv);

  // Return 0 if successful, or an error code if not
  return 0;
}

void onWindowDestroy(GtkWidget *widget, gpointer app) {
    is_running = 0;
    g_application_quit(G_APPLICATION(app));
}


// GTK Button Callback
void onPausePlayToggle(GtkButton *button, gpointer user_data) {
    togglePause();

    // If a valid button reference is passed, update its label
    if (button != NULL) {
        gtk_button_set_label(button, is_paused ? "Play" : "Pause");
    }

    if (!is_paused) {
        // Notify all threads to resume from pause
        pthread_cond_broadcast(&videoBuffer.notEmpty);
        pthread_cond_broadcast(&audioBuffer.notEmpty);
    }
}

// Handle key press events
// Handle key press events using GtkEventControllerKey
gboolean onKeyPress(GtkEventController *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
    if (keyval == GDK_KEY_space) {  // Spacebar is pressed
        onPausePlayToggle(NULL, NULL);  // Toggle Play/Pause
        return TRUE;  // Event handled, stop propagation
    }
    return FALSE;  // Let other key events propagate
}


void activate(GtkApplication *app, gpointer user_data) {
        DecodeData *data = (DecodeData *)user_data;
    GtkWidget *window, *main_box, *scrolled_window, *image_widget, *button_box, *pause_button;
    GtkEventController *key_controller;  // Declare the key event controller

    // Create the main application window
    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Media Player");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);

    // Create a key event controller
    key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(onKeyPress), NULL);
    gtk_widget_add_controller(window, key_controller);  // Attach the controller to the window

    // Create a vertical box to hold the image and controls
    main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_window_set_child(GTK_WINDOW(window), main_box);

    // Create a scrolled window for video display
    scrolled_window = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scrolled_window, 800, 450); // Set a fixed size for the display
    image_widget = gtk_image_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), image_widget);
    gtk_box_append(GTK_BOX(main_box), scrolled_window);

    // Create a horizontal box for controls
    button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_append(GTK_BOX(main_box), button_box);

    // Create a Play/Pause button
    pause_button = gtk_button_new_with_label("Pause");
    g_signal_connect(pause_button, "clicked", G_CALLBACK(onPausePlayToggle), NULL);
    gtk_box_append(GTK_BOX(button_box), pause_button);

    // Connect destroy signal to clean up on window close
    g_signal_connect(window, "destroy", G_CALLBACK(onWindowDestroy), app);

    // Start updating the display periodically
    g_timeout_add(1000 / data->frame_rate, (GSourceFunc)updateDisplay, image_widget);

    gtk_widget_set_visible(window, true);
}

int main(int argc, char **argv) {
    putenv("LIBGL_ALWAYS_SOFTWARE=1");
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <input_file> <frame_rate>\n", argv[0]);
        return EXIT_FAILURE;
    }

    DecodeData data;
    data.input_filename = argv[1];
    data.frame_rate = atoi(argv[2]);
    data.pixbuf = NULL;

    videoBufferInit(&videoBuffer, VIDEO_BUFFER_SIZE);
    audioBufferInit(&audioBuffer, AUDIO_BUFFER_SIZE);

    pthread_t video_thread, audio_thread;
    pthread_create(&video_thread, NULL, videoThread, &data);
    pthread_create(&audio_thread, NULL, audioThread, &data);

    GtkApplication *app = gtk_application_new("org.mediaplayer.app", 
                    G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &data);
    g_signal_connect(app, "command-line", G_CALLBACK(command_line_cb), &data);
    int status = g_application_run(G_APPLICATION(app), argc, argv);

    is_running = 0;

    pthread_join(video_thread, NULL);
    pthread_join(audio_thread, NULL);

    videoBufferDestroy(&videoBuffer);
    audioBufferDestroy(&audioBuffer);

    g_object_unref(app);
    return status;
}