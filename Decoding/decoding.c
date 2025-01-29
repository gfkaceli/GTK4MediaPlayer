#include "decoding.h"

volatile int is_running = 1;
volatile int is_paused = 0;

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
    while (is_running) {
        if (is_paused) {
            checkPauseState();  // Wait while paused
            continue;
        }

        if (av_read_frame(format_context, packet) < 0) {
            break;  // Exit if no more frames to read
        }

        if (packet->stream_index == audio_stream_index) {
            if (avcodec_send_packet(codec_context, packet) == 0) {
                while (avcodec_receive_frame(codec_context, frame) == 0) {
                    if (is_paused) {
                        checkPauseState();  // Wait while paused
                        continue;
                    }

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