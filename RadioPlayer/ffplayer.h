#ifndef FFPLAYER_H
#define FFPLAYER_H

#include "ffconfig.h"

#ifdef TARGET_OS_IPHONE
#import <AudioToolbox/AudioToolbox.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <unistd.h>
#include <time.h>
 #include <sys/time.h>
    
static VideoState *videoState;

static AVInputFormat *file_iformat;
static AVPacket flush_pkt;
static const char *input_filename;
static int fs_screen_width;
static int fs_screen_height;
static int default_width  = 640;
static int default_height = 480;
static int screen_width  = 0;
static int screen_height = 0;
static int audio_disable;
static int video_disable;
static int subtitle_disable;
static const char* wanted_stream_spec[AVMEDIA_TYPE_NB] = {0};
static int seek_by_bytes = -1;
static const char *window_title;
static int display_disable;
static int show_status = 1;
static int av_sync_type = AV_SYNC_AUDIO_MASTER;
static int64_t start_time = AV_NOPTS_VALUE;
static int64_t duration = AV_NOPTS_VALUE;
static int fast = 0;
static int genpts = 0;
static int lowres = 0;
static int decoder_reorder_pts = -1;
static int loop = 1;
static int framedrop = -1;
static int infinite_buffer = -1;
static enum ShowMode show_mode = SHOW_MODE_NONE;

static const char *audio_codec_name;
static const char *subtitle_codec_name;
static const char *video_codec_name;
static double rdftspeed = 0.02; //TODO- not static
static int autoexit;
static int64_t audio_callback_time;
static const char **vfilters_list = NULL;
static char *afilters = NULL;
static int is_full_screen;

static SDL_Window *screen; //static SDL_Surface *screen;
static SDL_Renderer *renderer;

static int autorotate = 1;
extern AVDictionary *format_opts, *codec_opts, *resample_opts;

// Callbacks  ******************************
typedef void (*FFGix_AudioOpenCallback)(struct AudioParams *audio_hw_params);
typedef void (*FFGix_StreamStatusUpdatedCallback)(StreamStatus);
typedef void (*FFGix_LibraryFailed)(FFGixLibFailureReason);
typedef void (*FFGix_FFPlayerFailed)(const char *msg, const char *detail);

typedef void (*FFGix_StreamOpened)(void);
typedef void (*FFGix_StreamClosed)(void);
    
#ifdef TARGET_OS_IPHONE
    typedef void (*FFGix_OpenAudioPlayer)(AudioStreamBasicDescription *asbd, int bufSize, int frameSize);
#endif

    typedef void (*FFGix_StreamElapsedClock)(float delay, float clock);

//----****-----

typedef struct FFGIXcallbacks{
    FFGix_AudioOpenCallback _AudioOpenCallback;
    FFGix_StreamStatusUpdatedCallback _StreamStatusUpdatedCallback;
    FFGix_LibraryFailed _LibraryFailed;
    FFGix_FFPlayerFailed _FFPlayerFailed;
    FFGix_StreamOpened _StreamOpened;
    FFGix_StreamClosed _StreamClosed;

#ifdef TARGET_OS_IPHONE
    FFGix_OpenAudioPlayer _OpenAudioPlayer;
#endif
    FFGix_StreamElapsedClock _StreamElapsedClock;

}FFGIX_Callbacks;

static FFGIX_Callbacks callbacks;

//End of Callbacks

    void ffgix_init(FFGIX_Callbacks _callbacks);
    int ffgix_run();
    int ffgix_lockmgr(void **mtx, enum AVLockOp op);
    VideoState *ffgix_stream_open(const char *filename, AVInputFormat *iformat);

    void ffgix_stream_component_close(VideoState *is, int stream_index);
    void ffgix_stream_close(VideoState *is);
    void ffgix_do_exit(VideoState *is);\
    void ffgix_free_picture(Frame *vp);
    void ffgix_calculate_display_rect(SDL_Rect *rect,
                                       int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                       int pic_width, int pic_height, AVRational pic_sar);
    
    int ffgix_video_open(VideoState *is, int force_set_video_mode, Frame *vp);

    void ffgix_decoder_destroy(Decoder *d);
    void ffgix_decoder_abort(Decoder *d, FrameQueue *fq);

    void ffgix_frame_queue_unref_item(Frame *vp);
    int ffgix_frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last);
    void ffgix_frame_queue_destory(FrameQueue *f);
    void ffgix_frame_queue_signal(FrameQueue *f);
    Frame *ffgix_frame_queue_peek(FrameQueue *f);
    Frame *ffgix_frame_queue_peek_next(FrameQueue *f);
    Frame *ffgix_frame_queue_peek_last(FrameQueue *f);
    Frame *ffgix_frame_queue_peek_writable(FrameQueue *f);
    Frame *ffgix_frame_queue_peek_readable(FrameQueue *f);
    void ffgix_frame_queue_push(FrameQueue *f);
    void ffgix_frame_queue_next(FrameQueue *f);
    int ffgix_frame_queue_prev(FrameQueue *f);
    int ffgix_frame_queue_nb_remaining(FrameQueue *f);
    int64_t ffgix_frame_queue_last_pos(FrameQueue *f);

    int ffgix_packet_queue_put_private(PacketQueue *q, AVPacket *pkt);
    int ffgix_packet_queue_put(PacketQueue *q, AVPacket *pkt);
    int ffgix_packet_queue_put_nullpacket(PacketQueue *q, int stream_index);
    int ffgix_packet_queue_init(PacketQueue *q);
    void ffgix_packet_queue_flush(PacketQueue *q);
    void ffgix_packet_queue_destroy(PacketQueue *q);
    void ffgix_packet_queue_abort(PacketQueue *q);
    void ffgix_packet_queue_start(PacketQueue *q);
    int  ffgix_packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial);


    int ffgix_is_realtime(AVFormatContext *s);
    int ffgix_audio_thread(void *arg);
    int ffgix_subtitle_thread(void *arg);
    int ffgix_read_thread(void *arg);
    int ffgix_video_thread(void *arg);

    void ffgix_update_sample_display(VideoState *is, short *samples, int samples_size);
    int ffgix_synchronize_audio(VideoState *is, int nb_samples);
    int ffgix_audio_decode_frame(VideoState *is);

#ifndef TARGET_OS_IPHONE
    void ffgix_set_default_window_size(int width, int height, AVRational sar);
    void ffgix_sdl_audio_callback(void *opaque, Uint8 *stream, int len);
#else
    void ffgix_ios_audio_callback(AudioQueueBufferRef buffer);
#endif
    
    int ffgix_audio_open(void *opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams *audio_hw_params);
    int ffgix_stream_component_open(VideoState *is, int stream_index);
    void ffgix_stream_component_close(VideoState *is, int stream_index);
    int ffgix_decode_interrupt_cb(void *ctx);

    void ffgix_decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, pthread_cond_t *empty_queue_cond);
    int ffgix_decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub);
    void ffgix_decoder_start(Decoder *d, int (*fn)(void *), void *arg);


    void ffgix_video_display(VideoState *is);
    void ffgix_video_audio_display(VideoState *s); //need to make it compatble with SDL 2.0
    void ffgix_video_image_display(VideoState *is); //need to make it compatble with SDL 2.0

    int ffgix_get_video_frame(VideoState *is, AVFrame *frame);
    int ffgix_get_master_sync_type(VideoState *is);
    double ffgix_get_master_clock(VideoState *is);
    double ffgix_get_clock(Clock *c);
    void ffgix_set_clock_at(Clock *c, double pts, int serial, double time);
    void ffgix_set_clock(Clock *c, double pts, int serial);
    void ffgix_set_clock_speed(Clock *c, double speed);
    void ffgix_init_clock(Clock *c, int *queue_serial);
    void ffgix_sync_clock_to_slave(Clock *c, Clock *slave);
    int ffgix_get_master_sync_type(VideoState *is);
    double ffgix_get_master_clock(VideoState *is);
    void ffgix_check_external_clock_speed(VideoState *is);
    void ffgix_stream_seek(VideoState *is, int64_t pos, int64_t rel, int seek_by_bytes);
    void ffgix_stream_toggle_pause(VideoState *is);
    void ffgix_toggle_pause(VideoState *is);
    void ffgix_step_to_next_frame(VideoState *is);

    void ffgix_print_error(const char *filename, int err);
    double ffgix_compute_target_delay(double delay, VideoState *is);
    double ffgix_vp_duration(VideoState *is, Frame *vp, Frame *nextvp);
    void ffgix_update_video_pts(VideoState *is, double pts, int64_t pos, int serial);
    void ffgix_video_refresh(void *opaque, double *remaining_time);
    void ffgix_alloc_picture(VideoState *is);
    void ffgix_duplicate_right_border_pixels(SDL_Texture *bmp);
    int ffgix_queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial);
    int ffgix_get_video_frame(VideoState *is, AVFrame *frame);
    int ffgix_configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                                     AVFilterContext *source_ctx, AVFilterContext *sink_ctx);
    int ffgix_configure_video_filters(AVFilterGraph *graph, VideoState *is, const char *vfilters, AVFrame *frame);
    int ffgix_configure_audio_filters(VideoState *is, const char *afilters, int force_output_format);


    AVDictionary **ffgix_setup_find_stream_info_opts(AVFormatContext *s, AVDictionary *codec_opts);
    AVDictionary *ffgix_filter_codec_opts(AVDictionary *opts, enum AVCodecID codec_id,
                                    AVFormatContext *s, AVStream *st, AVCodec *codec);
    int ffgix_check_stream_specifier(AVFormatContext *s, AVStream *st, const char *spec);
    void ffgix_exit_program(int ret);

    void ffgix_init_opts(void);
    void ffgix_opt_input_file(void *optctx, const char *filename);

// must be removed
    static struct SwsContext *sws_opts;
    static AVDictionary *swr_opts;
    static AVDictionary *sws_dict;


    static void (*program_exit)(int ret);

    /* Extern "C" */
    #ifdef __cplusplus
    }
    #endif

#endif // FFPLAYER_H
