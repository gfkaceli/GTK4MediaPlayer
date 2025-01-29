#ifndef DECODING_H
#define DECODING_H

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <pthread.h>
#include "buffer.h"

typedef struct {
    char *input_filename;
    int frame_rate;
    GdkPixbuf *pixbuf;
} DecodeData;

extern volatile int is_running;
extern volatile int is_paused;

void *videoThread(void *args);
void *audioThread(void *args);
void togglePause();
bool checkPauseState();

#endif // DECODER_H