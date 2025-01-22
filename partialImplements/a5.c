#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define INBUF_SIZE 20 //fixed buffer size

volatile int is_running = 1; // signal flag for window close

/*
  Function create_pixbuf_from_rgb_buffer
  Helper function to create gdk_pixbuf object
*/
GdkPixbuf *create_pixbuf_from_rgb_buffer(uint8_t *data, int width, int height,
                                         int rowstride) {
  return gdk_pixbuf_new_from_data(
      data,               // The image data
      GDK_COLORSPACE_RGB, // The color space
      FALSE,              // No alpha channel
      8,                  // Bits per color sample
      width, height,      // Width and height of the image
      rowstride,          // The rowstride (number of bytes between rows)
      (GdkPixbufDestroyNotify)av_free, // Free function
      data                             // Callback data (the buffer to be freed)
  );
}

typedef struct {
  char *input_filename;
  int frame_rate;
  GdkPixbuf *pixbuf; // Pointer to the GdkPixbuf for displaying the image
  // Add synchronization primitives if needed, e.g., mutex, condition variable
} DecodeData;

typedef struct {
  GdkPixbuf **pixbufs;    // Array of pointers to GdkPixbuf
  int size;               // Maximum number of items in the buffer
  int start;              // Index of the oldest item
  int end;                // Index at which to write the new item
  int count;              // Current number of items in the buffer
  pthread_mutex_t mutex;  // Mutex for synchronizing access to the buffer
  pthread_cond_t notFull; // Condition variable to wait for when buffer is full
  pthread_cond_t notEmpty; // Condition variable to wait for when buffer is empty
} CircularBuffer;

CircularBuffer cb; //global circular buffer access

/*
  Function circularBufferInit
  Helper Function to intialize the circular buffer
  takes a buffer and its size as argument
*/
void circularBufferInit(CircularBuffer *cb, int size) {
  cb->size = size;
  cb->start = 0;
  cb->end = 0;
  cb->count = 0;
  cb->pixbufs = (GdkPixbuf **)malloc(sizeof(GdkPixbuf *) * size);
  pthread_mutex_init(&cb->mutex, NULL);
  pthread_cond_init(&cb->notFull, NULL);
  pthread_cond_init(&cb->notEmpty, NULL);
}

/*
  Function circularBufferDestroy
  Function designed to destroy the buffer and 
  unreference all objects
*/
void circularBufferDestroy(CircularBuffer *cb) {
  for (int i = 0; i < cb->count; i++) {
    int index = (cb->start + i) % cb->size;
    if (cb->pixbufs[index]) {
      g_object_unref(cb->pixbufs[index]);
    }
  }
  free(cb->pixbufs);
  pthread_mutex_destroy(&cb->mutex);
  pthread_cond_destroy(&cb->notFull);
  pthread_cond_destroy(&cb->notEmpty);
}

/*
  Function circularBufferPush
  pushes a frame onto the circular buffer this way we start at the tail and 
  increase the count, if the buffer is full we do not push, and we wait, logically
  we do the opposite as in popping, we movie forward towards the head
*/
bool circularBufferPush(CircularBuffer *cb, GdkPixbuf *pixbuf) {
  pthread_mutex_lock(&cb->mutex);
  while (cb->count == cb->size) {
    pthread_cond_wait(&cb->notFull, &cb->mutex); // Wait if buffer is full
  }
  cb->pixbufs[cb->end] = pixbuf;
  g_object_ref(pixbuf); // Increase the reference count
  cb->end = (cb->end + 1) % cb->size;
  cb->count++;
  pthread_cond_signal(&cb->notEmpty); // Signal that buffer is not empty
  pthread_mutex_unlock(&cb->mutex);
  return true;
}

/*
  Function circularBufferPop
  pops a frame from the buffer in order to display in the gtk window
  logically what is done here is we work backwards and decrease the count
  moving back towards the tail, if the buffer is empty we wait
*/
bool circularBufferPop(CircularBuffer *cb, GdkPixbuf **pixbuf) {
  pthread_mutex_lock(&cb->mutex);
  while (cb->count == 0) {
    pthread_cond_wait(&cb->notEmpty, &cb->mutex); // Wait if buffer is empty
  }
  *pixbuf = cb->pixbufs[cb->start];
  cb->start = (cb->start + 1) % cb->size;
  cb->count--; // decrease the count
  pthread_cond_signal(&cb->notFull); // Signal that buffer is not full
  pthread_mutex_unlock(&cb->mutex);// unlock
  return true;
}

/*
  Function decode_thread_function
  argument that is passed to the pthread_create function 
  responsible for decoding the frame and pushing it onto the 
  circular buffer, also initializes the codec.
*/
void *decode_thread_function(void *args) {
  DecodeData *data = (DecodeData *)args;
  AVFormatContext *format_context = NULL;
  int video_stream_index = -1;
  AVCodecContext *codec_context = NULL;
  const AVCodec *codec = NULL;

  avformat_network_init();
  if (avformat_open_input(&format_context, data->input_filename, NULL, NULL) <
      0) {
    fprintf(stderr, "Could not open source file %s\n", data->input_filename);
    return NULL;
  }
  if (avformat_find_stream_info(format_context, NULL) < 0) {
    fprintf(stderr, "Could not find stream information\n");
    return NULL;
  }
  for (int i = 0; i < format_context->nb_streams; i++) {
    if (format_context->streams[i]->codecpar->codec_type ==
        AVMEDIA_TYPE_VIDEO) {
      video_stream_index = i;
      break;
    }
  }
  if (video_stream_index == -1) {
    fprintf(stderr, "Could not find a video stream\n");
    return NULL;
  }
  codec = avcodec_find_decoder(
      format_context->streams[video_stream_index]->codecpar->codec_id);
  codec_context = avcodec_alloc_context3(codec);
  avcodec_parameters_to_context(
      codec_context, format_context->streams[video_stream_index]->codecpar);
  avcodec_open2(codec_context, codec, NULL);

  AVPacket *pkt = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  AVFrame *rgb_frame = av_frame_alloc();
  struct SwsContext *sws_ctx = NULL;

  while (av_read_frame(format_context, pkt) >= 0) {
    if (pkt->stream_index == video_stream_index) {
      if (avcodec_send_packet(codec_context, pkt) < 0) {
        fprintf(stderr, "Error sending a packet for decoding\n");
        break;
      }
      g_autoptr(GError) error = NULL;
      while (is_running && (avcodec_receive_frame(codec_context, frame) >= 0)) {
        if (!sws_ctx) {
          sws_ctx = sws_getContext(
              frame->width, frame->height, codec_context->pix_fmt, frame->width,
              frame->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
        }
        int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, frame->width,
                                                frame->height, 32);
        uint8_t *buffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));
        av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, buffer,
                             AV_PIX_FMT_RGB24, frame->width, frame->height, 32);
        sws_scale(sws_ctx, (const uint8_t *const *)frame->data, frame->linesize,
                  0, frame->height, rgb_frame->data, rgb_frame->linesize);

        GdkPixbuf *pixbuf = create_pixbuf_from_rgb_buffer(
            rgb_frame->data[0], frame->width, frame->height,
            rgb_frame->linesize[0]);
        if (pixbuf != NULL) {
          circularBufferPush(&cb, pixbuf);
          g_object_unref(pixbuf); // Decrease reference count after pushing to buffer
        }
      }
    }
    av_packet_unref(pkt);
  }
  if (av_read_frame(format_context, pkt)<0){
  is_running=0;
   g_idle_add((GSourceFunc)g_application_quit, g_application_get_default());
  }
  av_frame_free(&frame);
  av_frame_free(&rgb_frame);
  av_packet_free(&pkt);
  avcodec_free_context(&codec_context);
  avformat_close_input(&format_context);
  avformat_network_deinit();
  if (sws_ctx) {
    sws_freeContext(sws_ctx);
  }
  return NULL;
}

/* 
  Function command_line_cb 
  Processes the command line arguments and sends them to the application
   */
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

/*
  Function update_display 
  continuously updates the display by popping images
  callback for our animation functionality
*/
gboolean update_display(GtkWidget *image_widget) {
  GdkPixbuf *pixbuf;
  if (circularBufferPop(&cb, &pixbuf)) {
    GdkPaintable *paintable =  (GdkPaintable *)gdk_texture_new_for_pixbuf(pixbuf); // Convert to GdkPaintable
    gtk_image_set_from_paintable(GTK_IMAGE(image_widget), paintable); // Use updated function
    g_object_unref(paintable); // Decrease reference count of paintable
    g_object_unref(pixbuf); // Decrease reference count after setting it
  }
  return G_SOURCE_CONTINUE;
}

/*
  Function on_window_destroy
  callback function for the destroy functionality when we close the application window
*/
static void on_window_destroy(GtkWidget *widget, gpointer data) {
  is_running = 0; // Set the global flag to 0
  // Quit the application
  g_application_quit(G_APPLICATION(data));
}

/*
  Function activate 
  callback function for application initialization 
  sets up the display and g_timeout_add controls the frame rate 
  for the video player
*/
static void activate(GtkApplication *app, gpointer user_data) {
  DecodeData *data = (DecodeData *)user_data;
  GtkWidget *window, *scrolled_window, *image_widget;

  window = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(window), "Frame Display");
  gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);

  scrolled_window = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  image_widget = gtk_image_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window),
                                image_widget);
  gtk_window_set_child(GTK_WINDOW(window), scrolled_window);
  g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), app);
  // Setup periodic update
  g_timeout_add(1000 / data->frame_rate, (GSourceFunc)update_display,
                image_widget);
  gtk_widget_set_visible(window, true);
}

/*
  Function main
  main function that intializes the buffer and the decoding thread
*/
int main(int argc, char *argv[]) {
  putenv("LIBGL_ALWAYS_SOFTWARE=1");
  if (argc < 3) {
    fprintf(stderr, "Usage: %s <input_file> <frame_rate>\n", argv[0]);
    exit(1);
  }

  circularBufferInit(&cb, INBUF_SIZE); // initialize circular buffer

  DecodeData data;
  data.input_filename = argv[1];
  data.frame_rate = atoi(argv[2]);
  data.pixbuf = NULL;

  pthread_t decode_thread;
  pthread_create(&decode_thread, NULL, decode_thread_function, &data); // create thread

  GtkApplication *app;
  int status;

  app = gtk_application_new("org.example.app",
                            G_APPLICATION_HANDLES_COMMAND_LINE);
  g_signal_connect(app, "activate", G_CALLBACK(activate), &data);
  g_signal_connect(app, "command-line", G_CALLBACK(command_line_cb), &data);
  status = g_application_run(G_APPLICATION(app), argc, argv);

  // Wait for the decode thread to finish after the GTK application has been run
  pthread_join(decode_thread, NULL);
  circularBufferDestroy(&cb);
  g_object_unref(app);

  return status;
}
