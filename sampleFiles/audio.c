#include <stdio.h>
#include <stdlib.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <math.h>

#define SAMPLE_RATE 44100
#define CHANNELS 2
#define AMPLITUDE 0.5

void FillWithSineWave(float *buffer, size_t sampleCount, float frequency, float amplitude) {
    float angularFrequency = 2.0 * M_PI * frequency / 44100;
    for (size_t i = 0; i < sampleCount; i++) {
        buffer[i] = sin(angularFrequency * i) * amplitude;
    }
}

static void print_volume(const pa_cvolume *volume) {
    if (volume->channels == 2) {
        printf("Left Channel Volume: %u\n", volume->values[0]);
        printf("Right Channel Volume: %u\n", volume->values[1]);
    } else {
        printf("Mono Volume: %d\n", volume->values[0]);
        for (int i = 0; i < volume->channels; ++i) {
            printf("Channel %d Volume: %d\n", i, volume->values[i]);
        }
    }
}

static void sink_info_cb(pa_context *c, const pa_sink_info *i, int eol, void *userdata) {
    if (eol > 0) {
        return;
    }
    printf("Sink Name: %s\n", i->name);
    printf("Description: %s\n", i->description);
    printf("Sample Format: %s\n", pa_sample_format_to_string(i->sample_spec.format));
    printf("Sample Rate: %u Hz\n", i->sample_spec.rate);
    printf("Channels: %u\n", i->sample_spec.channels);
    print_volume(&i->volume);
    printf("\n");
}

static void context_state_cb(pa_context *c, void *userdata) {
    pa_context_state_t state = pa_context_get_state(c);

    switch (state) {
        case PA_CONTEXT_READY:
            printf("Connection to PulseAudio server established.\n");
            pa_operation_unref(pa_context_get_sink_info_list(c, sink_info_cb, NULL));
            break;
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            printf("Connection to PulseAudio server failed.\n");
            break;
        default:
            break;
    }
}

int main() {
    pa_mainloop* m = pa_mainloop_new();
    pa_mainloop_api* api = pa_mainloop_get_api(m);
    pa_context* context = pa_context_new(api, "SinkInfoExample");
    pa_sink_info *info = NULL;
    pa_simple *s = NULL;
    pa_sample_spec ss;
    int error;

    ss.format = PA_SAMPLE_FLOAT32LE;
    ss.channels = CHANNELS;
    ss.rate = SAMPLE_RATE;

    s = pa_simple_new(NULL, "PlayExample", PA_STREAM_PLAYBACK, NULL, "Playback", &ss, NULL, NULL, &error);
    if (!s) {
        fprintf(stderr, __FILE__": pa_simple_new() failed: %s\n", pa_strerror(error));
        return 1;
    }
    if (!context) {
        fprintf(stderr, "Failed to create PulseAudio context.\n");
        exit(1);
    }

    pa_context_set_state_callback(context, context_state_cb, NULL);
    pa_context_connect(context, NULL, PA_CONTEXT_NOFLAGS, NULL);

    const size_t numSamples = 44100 * 2; // 2 seconds of audio
    float *buffer = malloc(numSamples * sizeof(float));
    if (!buffer) {
        fprintf(stderr, "Could not allocate buffer\n");
        if (s) pa_simple_free(s);
        return 1;
    }

    // Generate sine wave
    FillWithSineWave(buffer, numSamples, 440.0, 0.5);

    // Play audio
    if (pa_simple_write(s, buffer, numSamples * sizeof(float), &error) < 0) {
        fprintf(stderr, "pa_simple_write() failed: %s\n", pa_strerror(error));
    }

    // Cleanup
    free(buffer);
    if (s) pa_simple_free(s);
    

    int ret;
    if (pa_mainloop_run(m, &ret) < 0) {
        fprintf(stderr, "Failed to run main loop.\n");
        exit(1);
    }



    pa_context_unref(context);
    pa_mainloop_free(m);

    return 0;
}