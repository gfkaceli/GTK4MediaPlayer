
#include "buffer.h"

VideoBuffer videoBuffer;
AudioBuffer audioBuffer;



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
