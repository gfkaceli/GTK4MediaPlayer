#include "Buffer/buffer.h"
#include "Decoding/decoding.h"
#include "GUI/gui.h"

#define VIDEO_BUFFER_SIZE 20
#define AUDIO_BUFFER_SIZE 8192


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