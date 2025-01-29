#ifndef BUFFER_H
#define BUFFER_H

#include <pthread.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include "decoding.h"

// Video Buffer Structure
typedef struct {
    GdkPixbuf **pixbufs;
    int size, start, end, count;
    pthread_mutex_t mutex;
    pthread_cond_t notFull, notEmpty;
} VideoBuffer;

// Audio Buffer Structure
typedef struct {
    uint8_t *buffer;
    size_t size, write_pos, read_pos, count;
    pthread_mutex_t mutex;
    pthread_cond_t notFull, notEmpty;
} AudioBuffer;

// Global Buffers
extern VideoBuffer videoBuffer;
extern AudioBuffer audioBuffer;

// Buffer Functions
void videoBufferInit(VideoBuffer *vb, int size);
void videoBufferDestroy(VideoBuffer *vb);
bool videoBufferPush(VideoBuffer *vb, GdkPixbuf *pixbuf);
bool videoBufferPop(VideoBuffer *vb, GdkPixbuf **pixbuf);

void audioBufferInit(AudioBuffer *ab, size_t size);
void audioBufferDestroy(AudioBuffer *ab);
bool audioBufferPush(AudioBuffer *ab, const uint8_t *data, size_t bytes);
bool audioBufferPop(AudioBuffer *ab, uint8_t *data, size_t bytes);

#endif // BUFFER_H