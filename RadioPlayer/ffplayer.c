#include "ffplayer.h"

AVDictionary *format_opts, *codec_opts, *resample_opts;

static inline
int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
                   enum AVSampleFormat fmt2, int64_t channel_count2)
{
    /* If channel count == 1, planar and non-planar formats are the same */
    if (channel_count1 == 1 && channel_count2 == 1)
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    else
        return channel_count1 != channel_count2 || fmt1 != fmt2;
}

static inline
int64_t ffgix_get_valid_channel_layout(int64_t channel_layout, int channels)
{
    if (channel_layout && av_get_channel_layout_nb_channels(channel_layout) == channels)
        return channel_layout;
    else
        return 0;
}

static inline int ffgix_compute_mod(int a, int b)
{
    return a < 0 ? a%b + b : a%b;
}

void ffgix_init(FFGIX_Callbacks _callbacks)
{

    /* register all codecs, demux and protocols */
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
#if CONFIG_AVFILTER
    avfilter_register_all();
#endif
    av_register_all();
    avformat_network_init();

    ffgix_init_opts();

    callbacks = _callbacks;

}


int ffgix_run()
{
    int flags;

    //flags =  SDL_INIT_AUDIO | SDL_INIT_TIMER;

    audio_disable = 0;
    display_disable = 1;
    if (display_disable) {
            video_disable = 1;
        }
    if (!display_disable) {
            //const SDL_VideoInfo *vi = SDL_GetVideoInfo();
            //fs_screen_width = vi->current_w;
            //fs_screen_height = vi->current_h;
    }
    
    
#ifndef TARGET_OS_IPHONE
    if (SDL_Init (flags)) {
        av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
        av_log(NULL, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
        (*callbacks._FFPlayerFailed)("Unable to init", "Initalization of SDL Failed");
        return -1;
    }
#endif

    av_init_packet(&flush_pkt);
    flush_pkt.data = (uint8_t *)&flush_pkt;
    
    
    

    videoState = ffgix_stream_open("rtmp://media.radiosedayemardom.com:1935/radiosedaye/myStream", file_iformat);
    if (!videoState) {
        ALog("Failed to initialize VideoState!\n");
        return -1;
    }

    //Update Stream Status
    //(*callbacks._StreamStatusUpdatedCallback)(StreamStatusConntected);

    return 1;
}

int ffgix_lockmgr(void **mtx, enum AVLockOp op)
{
    #ifndef TARGET_OS_IPHONE
   switch(op) {
      case AV_LOCK_CREATE:
          *mtx = SDL_CreateMutex();
          if(!*mtx)
              return 1;
          return 0;
      case AV_LOCK_OBTAIN:
          return !!SDL_LockMutex(*mtx);
      case AV_LOCK_RELEASE:
          return !!SDL_UnlockMutex(*mtx);
      case AV_LOCK_DESTROY:
          SDL_DestroyMutex(*mtx);
          return 0;
   }
#endif
   return 1;
}

VideoState* ffgix_stream_open(const char *filename, AVInputFormat *iformat)
{
    VideoState *is;
    is = (VideoState *) av_mallocz(sizeof(VideoState));
    if (!is)
        return NULL;

    memset( is->silence_buf, 0, sizeof(UInt8)*SDL_AUDIO_MIN_BUFFER_SIZE);
    
    is->filename = av_strdup(filename);
    if (!is->filename)
        goto fail;

    is->iformat = iformat;
    is->ytop    = 0;
    is->xleft   = 0;

    /* start video display */
    if (ffgix_frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
        goto fail;
    if (ffgix_frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
        goto fail;
    if (ffgix_frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
        goto fail;

    if (ffgix_packet_queue_init(&is->videoq) < 0 ||
            ffgix_packet_queue_init(&is->audioq) < 0 ||
            ffgix_packet_queue_init(&is->subtitleq) < 0)
        goto fail;

    errno = 0;
    if( pthread_cond_init( &is->continue_read_thread, NULL) != 0){
        ALog("Pthread condition failed to init continue_read_thread. %s", strerror(errno));
         (*callbacks._FFPlayerFailed)("Openning Stream Failed", "Unable to initialize pthread_condition");
        return NULL;
    }

    ffgix_init_clock(&is->vidclk, &is->videoq.serial);
    ffgix_init_clock(&is->audclk, &is->audioq.serial);
    ffgix_init_clock(&is->extclk, &is->extclk.serial);
    is->audio_clock_serial = -1;
    is->av_sync_type = av_sync_type;

    errno = 0;
    if(pthread_create( &is->read_tid, NULL, (void *)ffgix_read_thread, is)!=0){
        ALog("Pthread create failed for is->read_tid. %s", strerror(errno));
        return NULL;
fail:
        ffgix_stream_close(is);
        (*callbacks._FFPlayerFailed)("Openning Stream Failed", "Unable to create pthread");
        return NULL;
    }
    (*callbacks._StreamOpened)();
    return is;
}

void ffgix_stream_close(VideoState *is)
{
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    is->abort_request = 1;
    //SDL_WaitThread(is->read_tid, NULL);
    pthread_join( is->read_tid, NULL);

    /* close each stream */
    if (is->audio_stream >= 0)
        ffgix_stream_component_close(is, is->audio_stream);
    if (is->video_stream >= 0)
        ffgix_stream_component_close(is, is->video_stream);
    if (is->subtitle_stream >= 0)
        ffgix_stream_component_close(is, is->subtitle_stream);

    avformat_close_input(&is->ic);

    ffgix_packet_queue_destroy(&is->videoq);
    ffgix_packet_queue_destroy(&is->audioq);
    ffgix_packet_queue_destroy(&is->subtitleq);

    /* free all pictures */
    ffgix_frame_queue_destory(&is->pictq);
    ffgix_frame_queue_destory(&is->sampq);
    ffgix_frame_queue_destory(&is->subpq);
    pthread_cond_destroy( &is->continue_read_thread ); //SDL_DestroyCond(is->continue_read_thread);
#if !CONFIG_AVFILTER
    sws_freeContext(is->img_convert_ctx);
#endif
    sws_freeContext(is->sub_convert_ctx);
    av_free(is->filename);
    av_free(is);

    //Update Stream Status
    (*callbacks._StreamStatusUpdatedCallback)(StreamStatusDisconnected);
}

void ffgix_do_exit(VideoState *is)
{
    if (is) {
        ffgix_stream_close(is);
    }
    av_lockmgr_register(NULL);
    //uninit_opts();
#if CONFIG_AVFILTER
    av_freep(&vfilters_list);
#endif
    avformat_network_deinit();
    if (show_status)
        printf("\n");
    //SDL_Quit();
    av_log(NULL, AV_LOG_QUIET, "%s", "");
    exit(0);
}

void ffgix_free_picture(Frame *vp)
{
    #ifndef TARGET_OS_IPHONE
     if (vp->bmp) {
         SDL_DestroyTexture(vp->bmp); //SDL_FreeYUVOverlay(vp->bmp);
         vp->bmp = NULL;
     }
#endif
}

#ifndef TARGET_OS_IPHONE
void ffgix_calculate_display_rect(SDL_Rect *rect,
                                   int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                   int pic_width, int pic_height, AVRational pic_sar)
{
    float aspect_ratio;
    int width, height, x, y;

    if (pic_sar.num == 0)
        aspect_ratio = 0;
    else
        aspect_ratio = av_q2d(pic_sar);

    if (aspect_ratio <= 0.0)
        aspect_ratio = 1.0;
    aspect_ratio *= (float)pic_width / (float)pic_height;

    /* XXX: we suppose the screen has a 1.0 pixel ratio */
    height = scr_height;
    width = ((int)rint(height * aspect_ratio)) & ~1;
    if (width > scr_width) {
        width = scr_width;
        height = ((int)rint(width / aspect_ratio)) & ~1;
    }
    x = (scr_width - width) / 2;
    y = (scr_height - height) / 2;
    rect->x = scr_xleft + x;
    rect->y = scr_ytop  + y;
    rect->w = FFMAX(width,  1);
    rect->h = FFMAX(height, 1);
}


void ffgix_set_default_window_size(int width, int height, AVRational sar)
{
    SDL_Rect rect;
    ffgix_calculate_display_rect(&rect, 0, 0, INT_MAX, height, width, height, sar);
    default_width  = rect.w;
    default_height = rect.h;
}


int ffgix_video_open(VideoState *is, int force_set_video_mode, Frame *vp)
{
    int flags = SDL_WINDOW_SHOWN;// = SDL_HWSURFACE | SDL_ASYNCBLIT | SDL_HWACCEL;
    int w,h, _screenWdith, _screenHeight;

    if (is_full_screen) flags |= SDL_WINDOW_FULLSCREEN;
    else                flags |= SDL_WINDOW_RESIZABLE;

    if (vp && vp->width)
        ffgix_set_default_window_size(vp->width, vp->height, vp->sar);

    if (is_full_screen && fs_screen_width) {
        w = fs_screen_width;
        h = fs_screen_height;
    } else if (!is_full_screen && screen_width) {
        w = screen_width;
        h = screen_height;
    } else {
        w = default_width;
        h = default_height;
    }
    w = FFMIN(16383, w);
    SDL_GetWindowSize( screen, &_screenWdith, &_screenHeight);
    if (screen && is->width == _screenWdith && _screenWdith == w
       && is->height== _screenHeight && _screenHeight == h && !force_set_video_mode)
        return 0;
    //screen = SDL_SetVideoMode(w, h, 0, flags);

    if (!window_title)
        window_title = input_filename;

    screen = SDL_CreateWindow( window_title,
                              SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED,
                              w, h,
                              flags);

    if (!screen) {
        av_log(NULL, AV_LOG_FATAL, "SDL: could not set video mode - exiting\n");
        ffgix_do_exit(is);
    }

    SDL_GetWindowSize( screen, &_screenWdith, &_screenHeight);
    is->width  = _screenWdith;
    is->height = _screenHeight;

    return 0;
}
#endif

void ffgix_frame_queue_unref_item(Frame *vp)
{
    av_frame_unref(vp->frame);
    avsubtitle_free(&vp->sub);
}

int ffgix_frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last)
{
    int i;
    memset(f, 0, sizeof(FrameQueue));
    errno = 0;
    if ( pthread_mutex_init( &f->mutex, NULL) != 0 ) {
        ALog("Mutex init failed. %s", strerror(errno));
        return AVERROR(ENOMEM);
    }

    errno = 0;
    if( pthread_cond_init( &f->cond, NULL) != 0){
        ALog("Condition init failed. %s", strerror(errno));
    }

    f->pktq = pktq;
    f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
    f->keep_last = !!keep_last;
    for (i = 0; i < f->max_size; i++)
        if (!(f->queue[i].frame = av_frame_alloc()))
            return AVERROR(ENOMEM);
    return 0;
}

void ffgix_frame_queue_destory(FrameQueue *f)
{
    int i;
    for (i = 0; i < f->max_size; i++) {
        Frame *vp = &f->queue[i];
        ffgix_frame_queue_unref_item(vp);
        av_frame_free(&vp->frame);
        //free_picture(vp);
    }
    pthread_mutex_destroy( &f->mutex); //SDL_DestroyMutex(f->mutex);
    pthread_cond_destroy( &f->cond); //SDL_DestroyCond(f->cond);
}

void ffgix_frame_queue_signal(FrameQueue *f)
{
    pthread_mutex_lock( &f->mutex); //SDL_LockMutex(f->mutex);
    pthread_cond_signal( &f->cond); //SDL_CondSignal(f->cond);
    pthread_mutex_unlock(&f->mutex); //SDL_UnlockMutex(f->mutex);
}

Frame* ffgix_frame_queue_peek(FrameQueue *f)
{
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

Frame* ffgix_frame_queue_peek_next(FrameQueue *f)
{
    return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

Frame* ffgix_frame_queue_peek_last(FrameQueue *f)
{
    return &f->queue[f->rindex];
}

Frame* ffgix_frame_queue_peek_writable(FrameQueue *f)
{
    /* wait until we have space to put a new frame */
    pthread_mutex_lock( &f->mutex); //SDL_LockMutex(f->mutex);
    while (f->size >= f->max_size &&
           !f->pktq->abort_request) {
        pthread_cond_wait( &f->cond, &f->mutex); //SDL_CondWait(f->cond, f->mutex);
    }
    pthread_mutex_unlock( &f->mutex); //SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[f->windex];
}

Frame* ffgix_frame_queue_peek_readable(FrameQueue *f)
{
    /* wait until we have a readable a new frame */
    pthread_mutex_lock( &f->mutex);//SDL_LockMutex(f->mutex);
    while (f->size - f->rindex_shown <= 0 &&
           !f->pktq->abort_request) {
        pthread_cond_wait( &f->cond, &f->mutex); //SDL_CondWait(f->cond, f->mutex);
    }
    pthread_mutex_unlock( &f->mutex); //SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

void ffgix_frame_queue_push(FrameQueue *f)
{
    if (++f->windex == f->max_size)
        f->windex = 0;
    pthread_mutex_lock( &f->mutex);//SDL_LockMutex(f->mutex);
    f->size++;
    pthread_cond_signal( &f->cond ); //SDL_CondSignal(f->cond);
    pthread_mutex_unlock( &f->mutex); //SDL_UnlockMutex(f->mutex);
}

void ffgix_frame_queue_next(FrameQueue *f)
{
    if (f->keep_last && !f->rindex_shown) {
        f->rindex_shown = 1;
        return;
    }
    ffgix_frame_queue_unref_item(&f->queue[f->rindex]);
    if (++f->rindex == f->max_size)
        f->rindex = 0;
    pthread_mutex_lock( &f->mutex);//SDL_LockMutex(f->mutex);
    f->size--;
    pthread_cond_signal( &f->cond ); //SDL_CondSignal(f->cond);
    pthread_mutex_unlock( &f->mutex); //SDL_UnlockMutex(f->mutex);
}

/* jump back to the previous frame if available by resetting rindex_shown */
int ffgix_frame_queue_prev(FrameQueue *f)
{
    int ret = f->rindex_shown;
    f->rindex_shown = 0;
    return ret;
}

/* return the number of undisplayed frames in the queue */
int ffgix_frame_queue_nb_remaining(FrameQueue *f)
{
    return f->size - f->rindex_shown;
}

/* return last shown position */
int64_t ffgix_frame_queue_last_pos(FrameQueue *f)
{
    Frame *fp = &f->queue[f->rindex];
    if (f->rindex_shown && fp->serial == f->pktq->serial)
        return fp->pos;
    else
        return -1;
}

void ffgix_decoder_destroy(Decoder *d) {
    av_packet_unref(&d->pkt);
}

void ffgix_decoder_abort(Decoder *d, FrameQueue *fq)
{
    ffgix_packet_queue_abort(d->queue);
    ffgix_frame_queue_signal(fq);
    pthread_join( d->decoder_tid, NULL); //SDL_WaitThread(d->decoder_tid, NULL);
    d->decoder_tid = NULL;
    ffgix_packet_queue_flush(d->queue);
}

int ffgix_packet_queue_put_private(PacketQueue *q, AVPacket *pkt)
{
    MyAVPacketList *pkt1;

    if (q->abort_request)
       return -1;

    pkt1 = (MyAVPacketList *)av_malloc(sizeof(MyAVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;
    if (pkt == &flush_pkt)
        q->serial++;
    pkt1->serial = q->serial;

    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);
    /* XXX: should duplicate packet data in DV case */
    pthread_cond_signal( &q->cond);
    return 0;
}

int ffgix_packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    int ret;

    pthread_mutex_lock( &q->mutex); //SDL_LockMutex(q->mutex);
    ret = ffgix_packet_queue_put_private(q, pkt);
    pthread_mutex_unlock( &q->mutex ); //SDL_UnlockMutex(q->mutex);

    if (pkt != &flush_pkt && ret < 0)
        av_packet_unref(pkt);

    return ret;
}

int ffgix_packet_queue_put_nullpacket(PacketQueue *q, int stream_index)
{
    AVPacket pkt1, *pkt = &pkt1;
    av_init_packet(pkt);
    pkt->data = NULL;
    pkt->size = 0;
    pkt->stream_index = stream_index;
    return ffgix_packet_queue_put(q, pkt);
}

/* packet queue handling */
int ffgix_packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));

    errno = 0;
    if( pthread_mutex_init( &q->mutex, NULL) !=0 ){
        ALog("Pthread mutex initialize failed. %s", strerror(errno));
        return AVERROR(ENOMEM);
    }

    errno = 0;
    if( pthread_cond_init( &q->cond, NULL) !=0){
        ALog("Pthread condition initialize failed. %s", strerror(errno));
        return AVERROR(ENOMEM);
    }
    q->abort_request = 1;
    return 0;
}

void ffgix_packet_queue_flush(PacketQueue *q)
{
    MyAVPacketList *pkt, *pkt1;

    pthread_mutex_lock( &q->mutex ); //SDL_LockMutex(q->mutex);
    for (pkt = q->first_pkt; pkt; pkt = pkt1) {
        pkt1 = pkt->next;
        av_packet_unref(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    pthread_mutex_unlock( &q->mutex ); //SDL_UnlockMutex(q->mutex);
}

void ffgix_packet_queue_destroy(PacketQueue *q)
{
    ffgix_packet_queue_flush(q);
    pthread_mutex_destroy( &q->mutex ); //SDL_DestroyMutex(q->mutex);
    pthread_cond_destroy( &q->cond ); //SDL_DestroyCond(q->cond);
}

void ffgix_packet_queue_abort(PacketQueue *q)
{
    pthread_mutex_lock( &q->mutex ); //SDL_LockMutex(q->mutex);

    q->abort_request = 1;

    pthread_cond_signal(&q->cond ); //SDL_CondSignal(q->cond);

    pthread_mutex_unlock( &q->mutex ); //SDL_UnlockMutex(q->mutex);
}

void ffgix_packet_queue_start(PacketQueue *q)
{
    pthread_mutex_lock( &q->mutex ); //SDL_LockMutex(q->mutex);
    q->abort_request = 0;
    ffgix_packet_queue_put_private(q, &flush_pkt);
    pthread_mutex_unlock( &q->mutex ); //SDL_UnlockMutex(q->mutex);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
int ffgix_packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial)
{
    MyAVPacketList *pkt1;
    int ret;

    pthread_mutex_lock( &q->mutex ); //SDL_LockMutex(q->mutex);

    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size + sizeof(*pkt1);
            *pkt = pkt1->pkt;
            if (serial)
                *serial = pkt1->serial;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            pthread_cond_wait( &q->cond, &q->mutex ); //SDL_CondWait(q->cond, q->mutex);
        }
    }
    pthread_mutex_unlock( &q->mutex ); //SDL_UnlockMutex(q->mutex);
    return ret;
}

int ffgix_is_realtime(AVFormatContext *s)
{
    if(   !strcmp(s->iformat->name, "rtp")
       || !strcmp(s->iformat->name, "rtsp")
       || !strcmp(s->iformat->name, "sdp")
    )
        return 1;

    if(s->pb && (   !strncmp(s->filename, "rtp:", 4)
                 || !strncmp(s->filename, "udp:", 4)
                )
    )
        return 1;
    return 0;
}

int ffgix_audio_thread(void *arg)
{
    VideoState *is = arg;
    AVFrame *frame = av_frame_alloc();
    Frame *af;
#if CONFIG_AVFILTER
    int last_serial = -1;
    int64_t dec_channel_layout;
    int reconfigure;
#endif
    int got_frame = 0;
    AVRational tb;
    int ret = 0;

    if (!frame)
        return AVERROR(ENOMEM);

    do {
        if ((got_frame = ffgix_decoder_decode_frame(&is->auddec, frame, NULL)) < 0)
            goto the_end;

        if (got_frame) {
                tb = (AVRational){1, frame->sample_rate};

#if CONFIG_AVFILTER
                dec_channel_layout = ffgix_get_valid_channel_layout(frame->channel_layout, av_frame_get_channels(frame));

                reconfigure =
                    cmp_audio_fmts(is->audio_filter_src.fmt, is->audio_filter_src.channels,
                                   frame->format, av_frame_get_channels(frame))    || is->audio_filter_src.channel_layout != dec_channel_layout ||
                    is->audio_filter_src.freq           != frame->sample_rate || is->auddec.pkt_serial               != last_serial;

                if (reconfigure) {
                    char buf1[1024], buf2[1024];
                    av_get_channel_layout_string(buf1, sizeof(buf1), -1, is->audio_filter_src.channel_layout);
                    av_get_channel_layout_string(buf2, sizeof(buf2), -1, dec_channel_layout);
                    ALog("Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
                           is->audio_filter_src.freq, is->audio_filter_src.channels, av_get_sample_fmt_name(is->audio_filter_src.fmt), buf1, last_serial,
                           frame->sample_rate, av_frame_get_channels(frame), av_get_sample_fmt_name(frame->format), buf2, is->auddec.pkt_serial);

                    is->audio_filter_src.fmt            = frame->format;
                    is->audio_filter_src.channels       = av_frame_get_channels(frame);
                    is->audio_filter_src.channel_layout = dec_channel_layout;
                    is->audio_filter_src.freq           = frame->sample_rate;
                    last_serial                         = is->auddec.pkt_serial;

                    if ((ret = ffgix_configure_audio_filters(is, afilters, 1)) < 0)
                        goto the_end;
                }

            if ((ret = av_buffersrc_add_frame(is->in_audio_filter, frame)) < 0)
                goto the_end;

            while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter, frame, 0)) >= 0) {
                tb = is->out_audio_filter->inputs[0]->time_base;
#endif
                if (!(af = ffgix_frame_queue_peek_writable(&is->sampq)))
                    goto the_end;

                af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                af->pos = av_frame_get_pkt_pos(frame);
                af->serial = is->auddec.pkt_serial;
                af->duration = av_q2d((AVRational){frame->nb_samples, frame->sample_rate});

                av_frame_move_ref(af->frame, frame);
                ffgix_frame_queue_push(&is->sampq);

#if CONFIG_AVFILTER
                if (is->audioq.serial != is->auddec.pkt_serial)
                    break;
            }
            if (ret == AVERROR_EOF)
                is->auddec.finished = is->auddec.pkt_serial;
#endif
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
 the_end:
#if CONFIG_AVFILTER
    avfilter_graph_free(&is->agraph);
#endif
    av_frame_free(&frame);
    return ret;
}

int ffgix_subtitle_thread(void *arg)
{
    VideoState *is = arg;
    Frame *sp;
    int got_subtitle;
    double pts;
    uint i;
    int j;
    //int r, g, b, y, u, v, a;

    for (;;) {
        if (!(sp = ffgix_frame_queue_peek_writable(&is->subpq)))
            return 0;

        if ((got_subtitle = ffgix_decoder_decode_frame(&is->subdec, NULL, &sp->sub)) < 0)
            break;

        pts = 0;

        if (got_subtitle && sp->sub.format == 0) {
            if (sp->sub.pts != AV_NOPTS_VALUE)
                pts = sp->sub.pts / (double)AV_TIME_BASE;
            sp->pts = pts;
            sp->serial = is->subdec.pkt_serial;

            for (i = 0; i < sp->sub.num_rects; i++)
            {
                for (j = 0; j < sp->sub.rects[i]->nb_colors; j++)
                {
                    //RGBA_IN(r, g, b, a, (uint32_t*)sp->sub.rects[i]->pict.data[1] + j);
                    //y = RGB_TO_Y_CCIR(r, g, b);
                    //u = RGB_TO_U_CCIR(r, g, b, 0);
                    //v = RGB_TO_V_CCIR(r, g, b, 0);
                    //YUVA_OUT((uint32_t*)sp->sub.rects[i]->pict.data[1] + j, y, u, v, a);
                }
            }

            /* now we can update the picture count */
            ffgix_frame_queue_push(&is->subpq);
        } else if (got_subtitle) {
            avsubtitle_free(&sp->sub);
        }
    }
    return 0;
}


/* this thread gets the stream from the disk or the network */
int ffgix_read_thread(void *arg)
{
    VideoState *is = arg;
    AVFormatContext *ic = NULL;
    int err, ret;
    uint i;
    int st_index[AVMEDIA_TYPE_NB];
    AVPacket pkt1, *pkt = &pkt1;
    int64_t stream_start_time;
    int pkt_in_play_range = 0;
    AVDictionaryEntry *t;
    AVDictionary **opts;
    uint orig_nb_streams;
    pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER; //SDL_mutex *wait_mutex = SDL_CreateMutex();
    int scan_all_pmts_set = 0;
    int64_t pkt_ts;
    struct timespec timeToWait;
    struct timeval now;
    char errMsg[1000];

    memset(st_index, -1, sizeof(st_index));
    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;
    is->eof = 0;

    ic = avformat_alloc_context();
    if (!ic) {
        ALog("Could not allocate context.\n");
        ret = FFGixLibFailureReasonNoMemoryAvaileble;
        (*callbacks._FFPlayerFailed)("Reading Stream failed", "Unable to allocate 'ic' context.");
        goto fail;
    }
    ic->interrupt_callback.callback = ffgix_decode_interrupt_cb;
    ic->interrupt_callback.opaque = is;
    if (!av_dict_get(format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
        av_dict_set(&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = 1;
    }
    err = avformat_open_input(&ic, is->filename, is->iformat, &format_opts);
    if (err < 0) {
        ffgix_print_error(is->filename, err);
        ret = FFGixLibFailureReasonOpeningFormatFailed; //-1;
        (*callbacks._FFPlayerFailed)("Unable to open input stream", "avformat_open_input failed.");
        goto fail;
    }
    if (scan_all_pmts_set)
        av_dict_set(&format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);

    if ((t = av_dict_get(format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        ALog("Option %s not found.\n", t->key);
        memset(errMsg, 0, sizeof(char)*1000);
        sprintf(errMsg, "Option %s not found.\n", t->key);
        (*callbacks._FFPlayerFailed)("Read stream filed", errMsg);
        ret = FFGixLibFailureReasonOptionNotFound; //AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }
    is->ic = ic;

    if (genpts) ic->flags |= AVFMT_FLAG_GENPTS;

    av_format_inject_global_side_data(ic);

    opts = ffgix_setup_find_stream_info_opts(ic, codec_opts);
    orig_nb_streams = ic->nb_streams;

    err = avformat_find_stream_info(ic, opts);

    for (i = 0; i < orig_nb_streams; i++) av_dict_free(&opts[i]);

    av_freep(&opts);

    if (err < 0) {
        ALog("%s: could not find codec parameters\n", is->filename);
        memset(errMsg, 0, sizeof(char)*1000);
        sprintf(errMsg, "%s: could not find codec parameters\n", is->filename);
        (*callbacks._FFPlayerFailed)("Read stream filed", errMsg);
        ret = FFGixLibFailureReasonNoCodecParameterFound;//-1;
        goto fail;
    }

    if (ic->pb) ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

    if (seek_by_bytes < 0) seek_by_bytes = !!(ic->iformat->flags & AVFMT_TS_DISCONT) && strcmp("ogg", ic->iformat->name);

    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

    if (!window_title && (t = av_dict_get(ic->metadata, "title", NULL, 0)))
        window_title = av_asprintf("%s - %s", t->value, input_filename);

    /* if seeking requested, we execute it */
    if (start_time != AV_NOPTS_VALUE) {
        int64_t timestamp;

        timestamp = start_time;
        /* add the stream start time */
        if (ic->start_time != AV_NOPTS_VALUE)  timestamp += ic->start_time;

        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            ALog("%s: could not seek to position %0.3f\n", is->filename, (double)timestamp / AV_TIME_BASE);
        }
    }

    is->realtime = ffgix_is_realtime(ic);

    if (show_status)
        av_dump_format(ic, 0, is->filename, 0);

    for (i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        enum AVMediaType type = st->codec->codec_type;
        st->discard = AVDISCARD_ALL;
        if (wanted_stream_spec[type] && st_index[type] == -1)
            if (avformat_match_stream_specifier(ic, st, wanted_stream_spec[type]) > 0)
                st_index[type] = i;
    }
    for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
        if (wanted_stream_spec[i] && st_index[i] == -1) {
            ALog("Stream specifier %s does not match any %s stream\n", wanted_stream_spec[i], av_get_media_type_string(i));
            st_index[i] = INT_MAX;
        }
    }

    if (!video_disable)
        st_index[AVMEDIA_TYPE_VIDEO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);

    if (!audio_disable)
        st_index[AVMEDIA_TYPE_AUDIO] = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, st_index[AVMEDIA_TYPE_AUDIO], st_index[AVMEDIA_TYPE_VIDEO], NULL, 0);
    if (!video_disable && !subtitle_disable)
        st_index[AVMEDIA_TYPE_SUBTITLE] =
            av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE, st_index[AVMEDIA_TYPE_SUBTITLE],
                                (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ? st_index[AVMEDIA_TYPE_AUDIO] : st_index[AVMEDIA_TYPE_VIDEO]), NULL, 0);

    is->show_mode = show_mode;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        AVStream *st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
        AVCodecContext *avctx = st->codec;
        AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);
        
#ifndef TARGET_OS_IPHONE
        if (avctx->width)
            ffgix_set_default_window_size(avctx->width, avctx->height, sar);
#endif
    }

    /* open the streams */
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        ffgix_stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
    }

    ret = -1;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        ret = ffgix_stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
    }
    if (is->show_mode == SHOW_MODE_NONE)
        is->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;

    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
        ffgix_stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
    }

    if (is->video_stream < 0 && is->audio_stream < 0) {
        ALog("Failed to open file '%s' or configure filtergraph\n", is->filename);

        memset(errMsg, 0, sizeof(char)*1000);
        sprintf(errMsg, "Failed to open file '%s' or configure filtergraph\n", is->filename);
        (*callbacks._FFPlayerFailed)("No Video or Audio Stream found", errMsg);

        ret = FFGixLibFailureReasonNoStreamFound; //-1;
        goto fail;
    }

    if (infinite_buffer < 0 && is->realtime)
        infinite_buffer = 1;

    for (;;) {
        if (is->abort_request)
            break;
        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused)
                is->read_pause_return = av_read_pause(ic);
            else
                av_read_play(ic);
        }
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
        if (is->paused &&
                (!strcmp(ic->iformat->name, "rtsp") ||
                 (ic->pb && !strncmp(input_filename, "mmsh:", 5)))) {
            /* wait 10 ms to avoid trying to get another packet */
            /* XXX: horrible */
            //SDL_Delay(10);
            continue;
        }
#endif
        //If seek
        if (is->seek_req) {
            int64_t seek_target = is->seek_pos;
            int64_t seek_min    = is->seek_rel > 0 ? seek_target - is->seek_rel + 2: INT64_MIN;
            int64_t seek_max    = is->seek_rel < 0 ? seek_target - is->seek_rel - 2: INT64_MAX;
// FIXME the +-2 is due to rounding being not done in the correct direction in generation
//      of the seek_pos/seek_rel variables

            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
            if (ret < 0) {
                ALog("%s: error while seeking\n", is->ic->filename);
                //TODO- Error while seeking
            } else {
                if (is->audio_stream >= 0) {
                    ALog("on seek only::> Flush and put flush_pkt");
                    ffgix_packet_queue_flush(&is->audioq);
                    ffgix_packet_queue_put(&is->audioq, &flush_pkt);
                }
                if (is->subtitle_stream >= 0) {
                    ffgix_packet_queue_flush(&is->subtitleq);
                    ffgix_packet_queue_put(&is->subtitleq, &flush_pkt);
                }
                if (is->video_stream >= 0) {
                    ffgix_packet_queue_flush(&is->videoq);
                    ffgix_packet_queue_put(&is->videoq, &flush_pkt);
                }
                if (is->seek_flags & AVSEEK_FLAG_BYTE) {
                   ffgix_set_clock(&is->extclk, NAN, 0);
                } else {
                   ffgix_set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0);
                }
            }
            is->seek_req = 0;
            is->queue_attachments_req = 1;
            is->eof = 0;
            if (is->paused)
                ffgix_step_to_next_frame(is);
        }
        if (is->queue_attachments_req) {
            if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                AVPacket copy;
                if ((ret = av_copy_packet(&copy, &is->video_st->attached_pic)) < 0)
                    goto fail;
                ffgix_packet_queue_put(&is->videoq, &copy);
                ffgix_packet_queue_put_nullpacket(&is->videoq, is->video_stream);
            }
            is->queue_attachments_req = 0;
        }

        /* if the queue are full, no need to read more */
        if (infinite_buffer<1 &&
              (is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE
            || (   (is->audioq   .nb_packets > MIN_FRAMES || is->audio_stream < 0 || is->audioq.abort_request)
                && (is->videoq   .nb_packets > MIN_FRAMES || is->video_stream < 0 || is->videoq.abort_request
                    || (is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC))
                && (is->subtitleq.nb_packets > MIN_FRAMES || is->subtitle_stream < 0 || is->subtitleq.abort_request)))) {
            /* wait 10 ms */
            pthread_mutex_lock( &wait_mutex ); //SDL_LockMutex(wait_mutex);
            gettimeofday(&now,NULL);
            timeToWait.tv_sec  = now.tv_sec;
            timeToWait.tv_nsec = now.tv_usec * 1000;
            timeToWait.tv_nsec += 10*1000;
            pthread_cond_timedwait( &is->continue_read_thread, &wait_mutex, &timeToWait ); //SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            pthread_mutex_unlock( &wait_mutex ); //SDL_UnlockMutex(wait_mutex);
            continue;
        }
        if (!is->paused &&
            (!is->audio_st || (is->auddec.finished == is->audioq.serial && ffgix_frame_queue_nb_remaining(&is->sampq) == 0)) &&
            (!is->video_st || (is->viddec.finished == is->videoq.serial && ffgix_frame_queue_nb_remaining(&is->pictq) == 0))) {
            if (loop != 1 && (!loop || --loop)) {
                ffgix_stream_seek(is, start_time != AV_NOPTS_VALUE ? start_time : 0, 0, 0);
            } else if (autoexit) {
                ret = FFGixLibFailureReasonEndOfFile; //AVERROR_EOF;
                (*callbacks._FFPlayerFailed)("Read stream filed", "Auto Exit");
                goto fail;
            }
        }
        ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            if ((ret == FFGixLibFailureReasonEndOfFile || avio_feof(ic->pb)) && !is->eof) {
                if (is->video_stream >= 0)
                    ffgix_packet_queue_put_nullpacket(&is->videoq, is->video_stream);
                if (is->audio_stream >= 0)
                    ffgix_packet_queue_put_nullpacket(&is->audioq, is->audio_stream);
                if (is->subtitle_stream >= 0)
                    ffgix_packet_queue_put_nullpacket(&is->subtitleq, is->subtitle_stream);
                is->eof = 1;
            }
            if (ic->pb && ic->pb->error)
                break;
            pthread_mutex_lock( &wait_mutex ); //SDL_LockMutex(wait_mutex);
            gettimeofday(&now,NULL);
            timeToWait.tv_sec  = now.tv_sec;
            timeToWait.tv_nsec = now.tv_usec * 1000;
            timeToWait.tv_nsec += 10*1000;
            pthread_cond_timedwait( &is->continue_read_thread, &wait_mutex, &timeToWait ); //SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            pthread_mutex_unlock( &wait_mutex ); //SDL_UnlockMutex(wait_mutex);
            continue;
        } else {
            is->eof = 0;
        }
        /* check if packet is in play range specified by user, then queue, otherwise discard */
        stream_start_time = ic->streams[pkt->stream_index]->start_time;
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        pkt_in_play_range = duration == AV_NOPTS_VALUE ||
                (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                av_q2d(ic->streams[pkt->stream_index]->time_base) - (double)(start_time != AV_NOPTS_VALUE ? start_time : 0) / 1000000
                <= ((double)duration / 1000000);
        if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
            ffgix_packet_queue_put(&is->audioq, pkt);
        } else if (pkt->stream_index == is->video_stream && pkt_in_play_range
                   && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            ffgix_packet_queue_put(&is->videoq, pkt);
        } else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
            ffgix_packet_queue_put(&is->subtitleq, pkt);
        } else {
            av_free_packet(pkt);
        }
    }
    /* wait until the end */
    while (!is->abort_request) {
        usleep(100 *(1000*1000) ); //SDL_Delay(100); //Wait 100ms
    }

    ret = 0;
 fail:
    /* close each stream */
    if (is->audio_stream >= 0)
        ffgix_stream_component_close(is, is->audio_stream);
    if (is->video_stream >= 0)
        ffgix_stream_component_close(is, is->video_stream);
    if (is->subtitle_stream >= 0)
        ffgix_stream_component_close(is, is->subtitle_stream);
    if (ic) {
        avformat_close_input(&ic);
        is->ic = NULL;
    }

    (*callbacks._StreamClosed)();

    if (ret != 0) {
#ifndef TARGET_OS_IPHONE
        SDL_Event event;

        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
#endif
    }
    pthread_mutex_destroy( &wait_mutex ); //SDL_DestroyMutex(wait_mutex);

    (*callbacks._LibraryFailed)(ret);
    return ret;
}


int ffgix_video_thread(void *arg)
{
    VideoState *is = arg;
    AVFrame *frame = av_frame_alloc();
    double pts;
    double duration;
    int ret;
    AVRational tb = is->video_st->time_base;
    AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);

#if CONFIG_AVFILTER
    AVFilterGraph *graph = avfilter_graph_alloc();
    AVFilterContext *filt_out = NULL, *filt_in = NULL;
    int last_w = 0;
    int last_h = 0;
    enum AVPixelFormat last_format = -2;
    int last_serial = -1;
    int last_vfilter_idx = 0;
#endif

    if (!frame)
        return AVERROR(ENOMEM);

    for (;;) {
        ret = ffgix_get_video_frame(is, frame);
        if (ret < 0)
            goto the_end;
        if (!ret)
            continue;

#if CONFIG_AVFILTER
        if (   last_w != frame->width
            || last_h != frame->height
            || last_format != frame->format
            || last_serial != is->viddec.pkt_serial
            || last_vfilter_idx != is->vfilter_idx) {
            av_log(NULL, AV_LOG_DEBUG,
                   "Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
                   last_w, last_h,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(last_format), "none"), last_serial,
                   frame->width, frame->height,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(frame->format), "none"), is->viddec.pkt_serial);
            avfilter_graph_free(&graph);
            graph = avfilter_graph_alloc();
            if ((ret = ffgix_configure_video_filters(graph, is, vfilters_list ? vfilters_list[is->vfilter_idx] : NULL, frame)) < 0) {
                
#ifndef TARGET_OS_IPHONE
                SDL_Event event;
                event.type = FF_QUIT_EVENT;
                event.user.data1 = is;
                SDL_PushEvent(&event);
#endif
                goto the_end;
            }
            filt_in  = is->in_video_filter;
            filt_out = is->out_video_filter;
            last_w = frame->width;
            last_h = frame->height;
            last_format = frame->format;
            last_serial = is->viddec.pkt_serial;
            last_vfilter_idx = is->vfilter_idx;
            frame_rate = filt_out->inputs[0]->frame_rate;
        }

        ret = av_buffersrc_add_frame(filt_in, frame);
        if (ret < 0)
            goto the_end;

        while (ret >= 0) {
            is->frame_last_returned_time = av_gettime_relative() / 1000000.0;

            ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
            if (ret < 0) {
                if (ret == AVERROR_EOF)
                    is->viddec.finished = is->viddec.pkt_serial;
                ret = 0;
                break;
            }

            is->frame_last_filter_delay = av_gettime_relative() / 1000000.0 - is->frame_last_returned_time;
            if (fabs(is->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
                is->frame_last_filter_delay = 0;
            tb = filt_out->inputs[0]->time_base;
#endif
            duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);
            pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            ret = ffgix_queue_picture(is, frame, pts, duration, av_frame_get_pkt_pos(frame), is->viddec.pkt_serial);
            av_frame_unref(frame);
#if CONFIG_AVFILTER
        }
#endif

        if (ret < 0)
            goto the_end;
    }
 the_end:
#if CONFIG_AVFILTER
    avfilter_graph_free(&graph);
#endif
    av_frame_free(&frame);
    return 0;
}

/* copy samples for viewing in editor window */
void ffgix_update_sample_display(VideoState *is, short *samples, int samples_size)
{
    int size, len;

    size = samples_size / sizeof(short);
    while (size > 0) {
        len = SAMPLE_ARRAY_SIZE - is->sample_array_index;
        if (len > size)
            len = size;
        memcpy(is->sample_array + is->sample_array_index, samples, len * sizeof(short));
        samples += len;
        is->sample_array_index += len;
        if (is->sample_array_index >= SAMPLE_ARRAY_SIZE)
            is->sample_array_index = 0;
        size -= len;
    }
}

/* return the wanted number of samples to get better sync if sync_type is video
 * or external master clock */
int ffgix_synchronize_audio(VideoState *is, int nb_samples)
{
    int wanted_nb_samples = nb_samples;

    /* if not master, then we try to remove or add samples to correct the clock */
    if (ffgix_get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) {
        double diff, avg_diff;
        int min_nb_samples, max_nb_samples;

        diff = ffgix_get_clock(&is->audclk) - ffgix_get_master_clock(is);

        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
            if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                /* not enough measures to have a correct estimate */
                is->audio_diff_avg_count++;
            } else {
                /* estimate the A-V difference */
                avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);

                if (fabs(avg_diff) >= is->audio_diff_threshold) {
                    wanted_nb_samples = nb_samples + (int)(diff * is->audio_src.freq);
                    min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    wanted_nb_samples = FFMIN(FFMAX(wanted_nb_samples, min_nb_samples), max_nb_samples);
                }
                av_dlog(NULL, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
                        diff, avg_diff, wanted_nb_samples - nb_samples,
                        is->audio_clock, is->audio_diff_threshold);
            }
        } else {
            /* too big difference : may be initial PTS errors, so
               reset A-V filter */
            is->audio_diff_avg_count = 0;
            is->audio_diff_cum       = 0;
        }
    }

    return wanted_nb_samples;
}

/**
 * Decode one audio frame and return its uncompressed size.
 *
 * The processed audio frame is decoded, converted if required, and
 * stored in is->audio_buf, with size in bytes given by the return
 * value.
 */
int ffgix_audio_decode_frame(VideoState *is)
{
    int data_size, resampled_data_size;
    int64_t dec_channel_layout;
    av_unused double audio_clock0;
    int wanted_nb_samples;
    Frame *af;

    if (is->paused)
        return -1;

    do {
        if (!(af = ffgix_frame_queue_peek_readable(&is->sampq)))
            return -1;
        ffgix_frame_queue_next(&is->sampq);
    } while (af->serial != is->audioq.serial);

    data_size = av_samples_get_buffer_size(NULL, av_frame_get_channels(af->frame),
                                           af->frame->nb_samples,
                                           af->frame->format, 1);

    dec_channel_layout =
        (af->frame->channel_layout && av_frame_get_channels(af->frame) == av_get_channel_layout_nb_channels(af->frame->channel_layout)) ?
        af->frame->channel_layout : av_get_default_channel_layout(av_frame_get_channels(af->frame));
    wanted_nb_samples = ffgix_synchronize_audio(is, af->frame->nb_samples);

    if (af->frame->format        != is->audio_src.fmt            ||
        dec_channel_layout       != is->audio_src.channel_layout ||
        af->frame->sample_rate   != is->audio_src.freq           ||
        (wanted_nb_samples       != af->frame->nb_samples && !is->swr_ctx)) {
        swr_free(&is->swr_ctx);
        is->swr_ctx = swr_alloc_set_opts(NULL,
                                         is->audio_tgt.channel_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
                                         dec_channel_layout,           af->frame->format, af->frame->sample_rate,
                                         0, NULL);
        if (!is->swr_ctx || swr_init(is->swr_ctx) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                    af->frame->sample_rate, av_get_sample_fmt_name(af->frame->format), av_frame_get_channels(af->frame),
                    is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.channels);
            swr_free(&is->swr_ctx);
            return -1;
        }
        is->audio_src.channel_layout = dec_channel_layout;
        is->audio_src.channels       = av_frame_get_channels(af->frame);
        is->audio_src.freq = af->frame->sample_rate;
        is->audio_src.fmt = af->frame->format;
    }

    if (is->swr_ctx) {
        const uint8_t **in = (const uint8_t **)af->frame->extended_data;
        uint8_t **out = &is->audio_buf1;
        int out_count = (int64_t)wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256;
        int out_size  = av_samples_get_buffer_size(NULL, is->audio_tgt.channels, out_count, is->audio_tgt.fmt, 0);
        int len2;
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }
        if (wanted_nb_samples != af->frame->nb_samples) {
            if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * is->audio_tgt.freq / af->frame->sample_rate,
                                        wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate) < 0) {
                av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                return -1;
            }
        }
        av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
        if (!is->audio_buf1)
            return AVERROR(ENOMEM);
        len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return -1;
        }
        if (len2 == out_count) {
            av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
            if (swr_init(is->swr_ctx) < 0)
                swr_free(&is->swr_ctx);
        }
        is->audio_buf = is->audio_buf1;
        resampled_data_size = len2 * is->audio_tgt.channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
    } else {
        is->audio_buf = af->frame->data[0];
        resampled_data_size = data_size;
    }

    audio_clock0 = is->audio_clock;
    /* update the audio clock with the pts */
    if (!isnan(af->pts))
        is->audio_clock = af->pts + (double) af->frame->nb_samples / af->frame->sample_rate;
    else
        is->audio_clock = NAN;
    is->audio_clock_serial = af->serial;
    
    {
        static double last_clock;
        (*callbacks._StreamElapsedClock)(is->audio_clock - last_clock, is->audio_clock);

#ifdef DEBUG
        //ALog("audio: delay=%0.3f clock=%0.3f clock0=%0.3f", is->audio_clock - last_clock, is->audio_clock, audio_clock0);
 #endif       
        
        last_clock = is->audio_clock;
    }

    return resampled_data_size;
}

#ifndef TARGET_OS_IPHONE
/* prepare a new audio buffer */
void ffgix_sdl_audio_callback(void *opaque, Uint8 *stream, int len)
{
    VideoState *is = opaque;
    int audio_size, len1;

    audio_callback_time = av_gettime_relative();

    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
           audio_size = ffgix_audio_decode_frame(is);
           if (audio_size < 0) {
                /* if error, just output silence */
               is->audio_buf      = is->silence_buf;
               is->audio_buf_size = sizeof(is->silence_buf) / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
           } else {
               if (is->show_mode != SHOW_MODE_VIDEO)
                   ffgix_update_sample_display(is, (int16_t *)is->audio_buf, audio_size);
               is->audio_buf_size = audio_size;
           }
           is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)
            len1 = len;
        memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    /* Let's assume the audio driver that is used by SDL has two periods. */
    if (!isnan(is->audio_clock)) {
        ffgix_set_clock_at(&is->audclk, is->audio_clock - (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec, is->audio_clock_serial, audio_callback_time / 1000000.0);
        ffgix_sync_clock_to_slave(&is->extclk, &is->audclk);
    }
}
#else

/* prepare a new audio buffer */
void ffgix_ios_audio_callback(AudioQueueBufferRef buffer)
{
    AudioTimeStamp bufferStartTime;
    VideoState *is = videoState;
    int audio_size, len1;
    int len;
    
    len = buffer->mAudioDataBytesCapacity;
    
    audio_callback_time = av_gettime_relative();
    
    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
            audio_size = ffgix_audio_decode_frame(is);
            if (audio_size < 0) {
                // if error, just output silence
                is->audio_buf      = is->silence_buf;
                is->audio_buf_size = sizeof(is->silence_buf) / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
            } else {
                if (is->show_mode != SHOW_MODE_VIDEO)
                    ffgix_update_sample_display(is, (int16_t *)is->audio_buf, audio_size);
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)
            len1 = len;
        
        if (buffer->mPacketDescriptionCount == 0) {
            bufferStartTime.mSampleTime = is->audio_clock * is->audio_tgt.frame_size;
            bufferStartTime.mFlags = kAudioTimeStampSampleTimeValid;
        }
        
        memcpy((uint8_t *)buffer->mAudioData + buffer->mAudioDataByteSize, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        buffer->mPacketDescriptions[buffer->mPacketDescriptionCount].mStartOffset = buffer->mAudioDataByteSize;
        buffer->mPacketDescriptions[buffer->mPacketDescriptionCount].mDataByteSize = len1;
        buffer->mPacketDescriptions[buffer->mPacketDescriptionCount].mVariableFramesInPacket = is->audio_tgt.frame_size;
        
        
        len -= len1;
        buffer->mAudioDataByteSize += len1;
        buffer->mPacketDescriptionCount++;
        
        is->audio_buf_index += len1;
    }
    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    // Let's assume the audio driver that is used by SDL has two periods.
    if (!isnan(is->audio_clock)) {
        ffgix_set_clock_at(&is->audclk, is->audio_clock - (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec, is->audio_clock_serial, audio_callback_time / 1000000.0);
        ffgix_sync_clock_to_slave(&is->extclk, &is->audclk);
    }
    
}

#endif


int ffgix_audio_open(void *opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams *audio_hw_params)
{
    SDL_AudioSpec wanted_spec, spec;
    
#ifndef TARGET_OS_IPHONE
    const char *env;
    static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
#endif
    static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;

#ifndef TARGET_OS_IPHONE
    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_channels = atoi(env);
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
    }
#endif
    
    if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
        wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
    }
    wanted_nb_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        ALog("Invalid sample rate or channel count!");
        return -1;
    }
    while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
        next_sample_rate_idx--;

    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    wanted_spec.userdata = opaque;
    
    
#ifndef TARGET_OS_IPHONE
    
    wanted_spec.callback = ffgix_sdl_audio_callback;
    
    while (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
        ALog("SDL_OpenAudio (%d channels, %d Hz): %s\n", wanted_spec.channels, wanted_spec.freq, SDL_GetError());
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels) {
            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
            wanted_spec.channels = wanted_nb_channels;
            if (!wanted_spec.freq) {
                ALog("No more combinations to try, audio open failed");
                return -1;
            }
        }
        wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
    }
    if (spec.format != AUDIO_S16SYS) {
        ALog("SDL advised audio format %d is not supported!", spec.format);
        return -1;
    }
    if (spec.channels != wanted_spec.channels) {
        wanted_channel_layout = av_get_default_channel_layout(spec.channels);
        if (!wanted_channel_layout) {
            ALog("SDL advised channel count %d is not supported!", spec.channels);
            return -1;
        }
    }


    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    audio_hw_params->channel_layout = wanted_channel_layout;
    audio_hw_params->channels =  spec.channels;
    audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->channels, 1, audio_hw_params->fmt, 1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
        ALog( "av_samples_get_buffer_size failed");
        return -1;
    }
    (*callbacks._AudioOpenCallback)(audio_hw_params);
#endif
    
#ifdef TARGET_OS_IPHONE
    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = wanted_spec.freq;
    audio_hw_params->channel_layout = wanted_channel_layout;
    audio_hw_params->channels =  wanted_spec.channels;
    int frameSize = av_samples_get_buffer_size(NULL, wanted_spec.channels, 1, AV_SAMPLE_FMT_S16, 1);
    audio_hw_params->frame_size  = frameSize;
    int bufSize = av_samples_get_buffer_size(NULL, wanted_spec.channels, wanted_spec.freq, AV_SAMPLE_FMT_S16, 1);
    audio_hw_params->bytes_per_sec = bufSize;
    
    
    struct AudioStreamBasicDescription asbd = {0};
    asbd.mFormatID = kAudioFormatLinearPCM ;//kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    asbd.mSampleRate = wanted_spec.freq;
    asbd.mChannelsPerFrame = wanted_spec.channels;
    asbd.mBitsPerChannel = 16;
    asbd.mFramesPerPacket = 1;
    asbd.mBytesPerFrame = frameSize;
    asbd.mBytesPerPacket = asbd.mBytesPerFrame *asbd.mFramesPerPacket;
    
    ALog("SampleRate: %d   freq:%d  channels:%d   size:%d padding:%d  frameSize:%d  bufSize:%d", wanted_spec.samples, wanted_spec.freq, wanted_spec.channels, wanted_spec.size, wanted_spec.padding, frameSize, bufSize );
    (*callbacks._OpenAudioPlayer)(&asbd, bufSize, frameSize);
#endif
    
    
    return spec.size;
}

/* open a given stream. Return 0 if OK */
int ffgix_stream_component_open(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    AVCodec *codec;
    const char *forced_codec_name = NULL;
    AVDictionary *opts;
    AVDictionaryEntry *t = NULL;
    int sample_rate, nb_channels;
    int64_t channel_layout;
    int ret = 0;
    int stream_lowres = lowres;

    if (stream_index < 0 || stream_index >= (int)ic->nb_streams )
        return -1;
    avctx = ic->streams[stream_index]->codec;

    codec = avcodec_find_decoder(avctx->codec_id);

    switch(avctx->codec_type){
        case AVMEDIA_TYPE_AUDIO   : is->last_audio_stream    = stream_index; forced_codec_name =    audio_codec_name; break;
        case AVMEDIA_TYPE_SUBTITLE: is->last_subtitle_stream = stream_index; forced_codec_name = subtitle_codec_name; break;
        case AVMEDIA_TYPE_VIDEO   : is->last_video_stream    = stream_index; forced_codec_name =    video_codec_name; break;
    default:
        break;
    }
    if (forced_codec_name)
        codec = avcodec_find_decoder_by_name(forced_codec_name);
    if (!codec) {
        if (forced_codec_name)
            ALog("No codec could be found with name '%s'\n", forced_codec_name);
        else
            ALog("No codec could be found with id %d\n", avctx->codec_id);
        return -1;
    }

    avctx->codec_id = codec->id;
    if(stream_lowres > av_codec_get_max_lowres(codec)){
        av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n",
                av_codec_get_max_lowres(codec));
        stream_lowres = av_codec_get_max_lowres(codec);
    }
    av_codec_set_lowres(avctx, stream_lowres);

    if(stream_lowres) avctx->flags |= CODEC_FLAG_EMU_EDGE;
    if (fast)   avctx->flags2 |= CODEC_FLAG2_FAST;
    if(codec->capabilities & CODEC_CAP_DR1)
        avctx->flags |= CODEC_FLAG_EMU_EDGE;

    opts = ffgix_filter_codec_opts(codec_opts, avctx->codec_id, ic, ic->streams[stream_index], codec);
    if (!av_dict_get(opts, "threads", NULL, 0))
        av_dict_set(&opts, "threads", "auto", 0);
    if (stream_lowres)
        av_dict_set_int(&opts, "lowres", stream_lowres, 0);
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO || avctx->codec_type == AVMEDIA_TYPE_AUDIO)
        av_dict_set(&opts, "refcounted_frames", "1", 0);
    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
        goto fail;
    }
    if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
        ret =  AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }

    is->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
#if CONFIG_AVFILTER
        {
            AVFilterLink *link;

            is->audio_filter_src.freq           = avctx->sample_rate;
            is->audio_filter_src.channels       = avctx->channels;
            is->audio_filter_src.channel_layout = ffgix_get_valid_channel_layout(avctx->channel_layout, avctx->channels);
            is->audio_filter_src.fmt            = avctx->sample_fmt;
            if ((ret = ffgix_configure_audio_filters(is, afilters, 0)) < 0)
                goto fail;
            link = is->out_audio_filter->inputs[0];
            sample_rate    = link->sample_rate;
            nb_channels    = link->channels;
            channel_layout = link->channel_layout;
        }
#else
        sample_rate    = avctx->sample_rate;
        nb_channels    = avctx->channels;
        channel_layout = avctx->channel_layout;
#endif
            
        /* prepare audio output */
        if ((ret = ffgix_audio_open(is, channel_layout, nb_channels, sample_rate, &is->audio_tgt)) < 0)
            goto fail;
        is->audio_hw_buf_size = ret;
        is->audio_src = is->audio_tgt;
        is->audio_buf_size  = 0;
        is->audio_buf_index = 0;

        /* init averaging filter */
        is->audio_diff_avg_coef  = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
        is->audio_diff_avg_count = 0;
        /* since we do not have a precise anough audio fifo fullness,
           we correct audio sync only if larger than this threshold */
        is->audio_diff_threshold = (double)(is->audio_hw_buf_size) / is->audio_tgt.bytes_per_sec;

        is->audio_stream = stream_index;
        is->audio_st = ic->streams[stream_index];

        ffgix_decoder_init(&is->auddec, avctx, &is->audioq, &is->continue_read_thread);
        if ((is->ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) && !is->ic->iformat->read_seek) {
            is->auddec.start_pts = is->audio_st->start_time;
            is->auddec.start_pts_tb = is->audio_st->time_base;
        }
        ffgix_decoder_start(&is->auddec, &ffgix_audio_thread, is);
#ifndef TARGET_OS_IPHONE
        SDL_PauseAudio(0);
#endif
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_stream = stream_index;
        is->video_st = ic->streams[stream_index];

        ffgix_decoder_init(&is->viddec, avctx, &is->videoq, &is->continue_read_thread);
        ffgix_decoder_start(&is->viddec, ffgix_video_thread, is);
        is->queue_attachments_req = 1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_stream = stream_index;
        is->subtitle_st = ic->streams[stream_index];

        ffgix_decoder_init(&is->subdec, avctx, &is->subtitleq, &is->continue_read_thread);
        ffgix_decoder_start(&is->subdec, &ffgix_subtitle_thread, is);
        break;
    default:
        break;
    }

fail:
    av_dict_free(&opts);

    return ret;
}

void ffgix_stream_component_close(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;

    if (stream_index < 0 || stream_index >= (int)ic->nb_streams)
        return;
    avctx = ic->streams[stream_index]->codec;

    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        ffgix_decoder_abort(&is->auddec, &is->sampq);
#ifndef TARGET_OS_IPHONE
        SDL_CloseAudio();
#endif
        ffgix_decoder_destroy(&is->auddec);
        swr_free(&is->swr_ctx);
        av_freep(&is->audio_buf1);
        is->audio_buf1_size = 0;
        is->audio_buf = NULL;

        if (is->rdft) {
            av_rdft_end(is->rdft);
            av_freep(&is->rdft_data);
            is->rdft = NULL;
            is->rdft_bits = 0;
        }
        break;
    case AVMEDIA_TYPE_VIDEO:
        ffgix_decoder_abort(&is->viddec, &is->pictq);
        ffgix_decoder_destroy(&is->viddec);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        ffgix_decoder_abort(&is->subdec, &is->subpq);
        ffgix_decoder_destroy(&is->subdec);
        break;
    default:
        break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    avcodec_close(avctx);
    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->audio_st = NULL;
        is->audio_stream = -1;
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_st = NULL;
        is->video_stream = -1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_st = NULL;
        is->subtitle_stream = -1;
        break;
    default:
        break;
    }
}

int ffgix_decode_interrupt_cb(void *ctx)
{
    VideoState *is = ctx;
    return is->abort_request;
}

void ffgix_decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, pthread_cond_t *empty_queue_cond) {
    memset(d, 0, sizeof(Decoder));
    d->avctx = avctx;
    d->queue = queue;
    d->empty_queue_cond = *empty_queue_cond;
    d->start_pts = AV_NOPTS_VALUE;
}

int ffgix_decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub) {
    int got_frame = 0;

    do {
        int ret = -1;

        if (d->queue->abort_request)
            return -1;

        if (!d->packet_pending || d->queue->serial != d->pkt_serial) {
            AVPacket pkt;
            do {
                if (d->queue->nb_packets == 0)
                    pthread_cond_signal( &d->empty_queue_cond); //SDL_CondSignal(d->empty_queue_cond);
                if (ffgix_packet_queue_get(d->queue, &pkt, 1, &d->pkt_serial) < 0)
                    return -1;
                if (pkt.data == flush_pkt.data) {
                    avcodec_flush_buffers(d->avctx);
                    d->finished = 0;
                    d->next_pts = d->start_pts;
                    d->next_pts_tb = d->start_pts_tb;
                }
            } while (pkt.data == flush_pkt.data || d->queue->serial != d->pkt_serial);
            av_free_packet(&d->pkt);
            d->pkt_temp = d->pkt = pkt;
            d->packet_pending = 1;
        }

        switch (d->avctx->codec_type) {
            case AVMEDIA_TYPE_VIDEO:
                ret = avcodec_decode_video2(d->avctx, frame, &got_frame, &d->pkt_temp);
                if (got_frame) {
                    if (decoder_reorder_pts == -1) {
                        frame->pts = av_frame_get_best_effort_timestamp(frame);
                    } else if (decoder_reorder_pts) {
                        frame->pts = frame->pkt_pts;
                    } else {
                        frame->pts = frame->pkt_dts;
                    }
                }
                break;
            case AVMEDIA_TYPE_AUDIO:
                ret = avcodec_decode_audio4(d->avctx, frame, &got_frame, &d->pkt_temp);
                if (got_frame) {
                    AVRational tb = (AVRational){1, frame->sample_rate};
                    if (frame->pts != AV_NOPTS_VALUE)
                        frame->pts = av_rescale_q(frame->pts, d->avctx->time_base, tb);
                    else if (frame->pkt_pts != AV_NOPTS_VALUE)
                        frame->pts = av_rescale_q(frame->pkt_pts, av_codec_get_pkt_timebase(d->avctx), tb);
                    else if (d->next_pts != AV_NOPTS_VALUE)
                        frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                    if (frame->pts != AV_NOPTS_VALUE) {
                        d->next_pts = frame->pts + frame->nb_samples;
                        d->next_pts_tb = tb;
                    }
                }
                break;
            case AVMEDIA_TYPE_SUBTITLE:
                ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, &d->pkt_temp);
                break;
        default:
            break;
        }

        if (ret < 0) {
            d->packet_pending = 0;
        } else {
            d->pkt_temp.dts =
            d->pkt_temp.pts = AV_NOPTS_VALUE;
            if (d->pkt_temp.data) {
                if (d->avctx->codec_type != AVMEDIA_TYPE_AUDIO)
                    ret = d->pkt_temp.size;
                d->pkt_temp.data += ret;
                d->pkt_temp.size -= ret;
                if (d->pkt_temp.size <= 0)
                    d->packet_pending = 0;
            } else {
                if (!got_frame) {
                    d->packet_pending = 0;
                    d->finished = d->pkt_serial;
                }
            }
        }
    } while (!got_frame && !d->finished);

    return got_frame;
}

void ffgix_decoder_start(Decoder *d, int (*fn)(void *), void *arg)
{
    ffgix_packet_queue_start(d->queue);
    pthread_create( &d->decoder_tid, NULL, (void *) fn, arg); //d->decoder_tid = SDL_CreateThread(fn, arg);
}

/* display the current picture, if any */
void ffgix_video_display(VideoState *is)
{
    if (!screen)
        ffgix_video_open(is, 0, NULL);
    if (is->audio_st && is->show_mode != SHOW_MODE_VIDEO)
        ffgix_video_audio_display(is);
    else if (is->video_st)
        ffgix_video_image_display(is);
}

void ffgix_video_audio_display(VideoState *s)
{
    int i, i_start, x, y1, y, ys, delay, n, nb_display_channels;
    int ch, channels, h, h2;//, bgcolor, fgcolor;
    int64_t time_diff;
    int rdft_bits, nb_freq;

    for (rdft_bits = 1; (1 << rdft_bits) < 2 * s->height; rdft_bits++)
        ;
    nb_freq = 1 << (rdft_bits - 1);

    /* compute display index : center on currently output samples */
    channels = s->audio_tgt.channels;
    nb_display_channels = channels;
    if (!s->paused) {
        int data_used= s->show_mode == SHOW_MODE_WAVES ? s->width : (2*nb_freq);
        n = 2 * channels;
        delay = s->audio_write_buf_size;
        delay /= n;

        /* to be more precise, we take into account the time spent since
           the last buffer computation */
        if (audio_callback_time) {
            time_diff = av_gettime_relative() - audio_callback_time;
            delay -= (time_diff * s->audio_tgt.freq) / 1000000;
        }

        delay += 2 * data_used;
        if (delay < data_used)
            delay = data_used;

        i_start= x = ffgix_compute_mod(s->sample_array_index - delay * channels, SAMPLE_ARRAY_SIZE);
        if (s->show_mode == SHOW_MODE_WAVES) {
            h = INT_MIN;
            for (i = 0; i < 1000; i += channels) {
                int idx = (SAMPLE_ARRAY_SIZE + x - i) % SAMPLE_ARRAY_SIZE;
                int a = s->sample_array[idx];
                int b = s->sample_array[(idx + 4 * channels) % SAMPLE_ARRAY_SIZE];
                int c = s->sample_array[(idx + 5 * channels) % SAMPLE_ARRAY_SIZE];
                int d = s->sample_array[(idx + 9 * channels) % SAMPLE_ARRAY_SIZE];
                int score = a - d;
                if (h < score && (b ^ c) < 0) {
                    h = score;
                    i_start = idx;
                }
            }
        }

        s->last_i_start = i_start;
    } else {
        i_start = s->last_i_start;
    }

    //bgcolor = SDL_MapRGB(screen->format, 0x00, 0x00, 0x00);
    if (s->show_mode == SHOW_MODE_WAVES) {
        //fill_rectangle(screen, s->xleft, s->ytop, s->width, s->height, bgcolor, 0);

        //fgcolor = SDL_MapRGB(screen->format, 0xff, 0xff, 0xff);

        /* total height for one channel */
        h = s->height / nb_display_channels;
        /* graph height / 2 */
        h2 = (h * 9) / 20;
        for (ch = 0; ch < nb_display_channels; ch++) {
            i = i_start + ch;
            y1 = s->ytop + ch * h + (h / 2); /* position of center line */
            for (x = 0; x < s->width; x++) {
                y = (s->sample_array[i] * h2) >> 15;
                if (y < 0) {
                    y = -y;
                    ys = y1 - y;
                } else {
                    ys = y1;
                }
                //fill_rectangle(screen, s->xleft + x, ys, 1, y, fgcolor, 0);
                i += channels;
                if (i >= SAMPLE_ARRAY_SIZE)
                    i -= SAMPLE_ARRAY_SIZE;
            }
        }

        //fgcolor = SDL_MapRGB(screen->format, 0x00, 0x00, 0xff);

        for (ch = 1; ch < nb_display_channels; ch++) {
            y = s->ytop + ch * h;
            //fill_rectangle(screen, s->xleft, y, s->width, 1, fgcolor, 0);
        }
        //SDL_UpdateRect(screen, s->xleft, s->ytop, s->width, s->height);
    } else {
        nb_display_channels= FFMIN(nb_display_channels, 2);
        if (rdft_bits != s->rdft_bits) {
            av_rdft_end(s->rdft);
            av_free(s->rdft_data);
            s->rdft = av_rdft_init(rdft_bits, DFT_R2C);
            s->rdft_bits = rdft_bits;
            s->rdft_data = av_malloc_array(nb_freq, 4 *sizeof(*s->rdft_data));
        }
        if (!s->rdft || !s->rdft_data){
            av_log(NULL, AV_LOG_ERROR, "Failed to allocate buffers for RDFT, switching to waves display\n");
            s->show_mode = SHOW_MODE_WAVES;
        } else {
            FFTSample *data[2];
            for (ch = 0; ch < nb_display_channels; ch++) {
                data[ch] = s->rdft_data + 2 * nb_freq * ch;
                i = i_start + ch;
                for (x = 0; x < 2 * nb_freq; x++) {
                    double w = (x-nb_freq) * (1.0 / nb_freq);
                    data[ch][x] = s->sample_array[i] * (1.0 - w * w);
                    i += channels;
                    if (i >= SAMPLE_ARRAY_SIZE)
                        i -= SAMPLE_ARRAY_SIZE;
                }
                av_rdft_calc(s->rdft, data[ch]);
            }
            /* Least efficient way to do this, we should of course
             * directly access it but it is more than fast enough. */
            for (y = 0; y < s->height; y++) {
                double w = 1 / sqrt(nb_freq);
                int a = sqrt(w * sqrt(data[0][2 * y + 0] * data[0][2 * y + 0] + data[0][2 * y + 1] * data[0][2 * y + 1]));
                int b = (nb_display_channels == 2 ) ? sqrt(w * sqrt(data[1][2 * y + 0] * data[1][2 * y + 0]
                       + data[1][2 * y + 1] * data[1][2 * y + 1])) : a;
                a = FFMIN(a, 255);
                b = FFMIN(b, 255);
                //fgcolor = SDL_MapRGB(screen->format, a, b, (a + b) / 2);

                //fill_rectangle(screen, s->xpos, s->height-y, 1, 1, fgcolor, 0);
            }
        }
        //SDL_UpdateRect(screen, s->xpos, s->ytop, 1, s->height);
        if (!s->paused)
            s->xpos++;
        if (s->xpos >= s->width)
            s->xpos= s->xleft;
    }
}

void ffgix_video_image_display(VideoState *is)
{
    Frame *vp;
    Frame *sp;
    AVPicture pict, texturePict;
    //SDL_Rect rect;
    int bw, bh;

    uint32_t format;
    int access;

    vp = ffgix_frame_queue_peek(&is->pictq);

#ifndef TARGET_OS_IPHONE
    if( SDL_QueryTexture( vp->bmp, &format, &access , &bw, &bh ) != 0){
        ALog("SDL_QueryTexture failed.%s", strerror(errno));
    }


    if (vp->bmp) {
        if (is->subtitle_st) {
            if (ffgix_frame_queue_nb_remaining(&is->subpq) > 0) {
                sp = ffgix_frame_queue_peek(&is->subpq);

                if (vp->pts >= sp->pts + ((float) sp->sub.start_display_time / 1000)) {
                    SDL_LockTexture( vp->bmp, NULL, (void **)texturePict.data, texturePict.linesize); //SDL_LockYUVOverlay (vp->bmp);

                    pict.data[0] = texturePict.data[0];
                    pict.data[1] = texturePict.data[2];
                    pict.data[2] = texturePict.data[1];

                    pict.linesize[0] = texturePict.linesize[0];
                    pict.linesize[1] = texturePict.linesize[2];
                    pict.linesize[2] = texturePict.linesize[1];

                    //for (i = 0; i < sp->sub.num_rects; i++)
                        //blend_subrect(&pict, sp->sub.rects[i], bw, bh);

                    SDL_UnlockTexture( vp->bmp); //SDL_UnlockYUVOverlay (vp->bmp);
                }
            }
        }

        ffgix_calculate_display_rect(&rect, is->xleft, is->ytop, is->width, is->height, vp->width, vp->height, vp->sar);

        //SDL_DisplayYUVOverlay(vp->bmp, &rect); //Need to update and be compatible with SDL 2.0

        if (rect.x != is->last_display_rect.x || rect.y != is->last_display_rect.y || rect.w != is->last_display_rect.w || rect.h != is->last_display_rect.h || is->force_refresh) {
            //int bgcolor = SDL_MapRGB(screen->format, 0x00, 0x00, 0x00);
            //fill_border(is->xleft, is->ytop, is->width, is->height, rect.x, rect.y, rect.w, rect.h, bgcolor, 1);
            is->last_display_rect = rect;
        }
    }
#endif
}

int ffgix_get_master_sync_type(VideoState *is) {
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
        if (is->video_st)
            return AV_SYNC_VIDEO_MASTER;
        else
            return AV_SYNC_AUDIO_MASTER;
    } else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (is->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            return AV_SYNC_EXTERNAL_CLOCK;
    } else {
        return AV_SYNC_EXTERNAL_CLOCK;
    }
    return AV_SYNC_EXTERNAL_CLOCK;
}

double ffgix_get_clock(Clock *c)
{
    if (*c->queue_serial != c->serial)
        return NAN;
    if (c->paused) {
        return c->pts;
    } else {
        double time = av_gettime_relative() / 1000000.0;
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
    }
}

void ffgix_set_clock_at(Clock *c, double pts, int serial, double time)
{
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
    c->serial = serial;
}

void ffgix_set_clock(Clock *c, double pts, int serial)
{
    double time = av_gettime_relative() / 1000000.0;
    ffgix_set_clock_at(c, pts, serial, time);
}

void ffgix_set_clock_speed(Clock *c, double speed)
{
    ffgix_set_clock(c, ffgix_get_clock(c), c->serial);
    c->speed = speed;
}

void ffgix_init_clock(Clock *c, int *queue_serial)
{
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
    ffgix_set_clock(c, NAN, -1);
}

void ffgix_sync_clock_to_slave(Clock *c, Clock *slave)
{
    double clock = ffgix_get_clock(c);
    double slave_clock = ffgix_get_clock(slave);
    if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
        ffgix_set_clock(c, slave_clock, slave->serial);
}

/* get the current master clock value */
double ffgix_get_master_clock(VideoState *is)
{
    double val;

    switch (ffgix_get_master_sync_type(is)) {
        case AV_SYNC_VIDEO_MASTER:
            val = ffgix_get_clock(&is->vidclk);
            break;
        case AV_SYNC_AUDIO_MASTER:
            val = ffgix_get_clock(&is->audclk);
            break;
        default:
            val = ffgix_get_clock(&is->extclk);
            break;
    }
    return val;
}

void ffgix_check_external_clock_speed(VideoState *is) {
   if ( ((is->video_stream >= 0) && (is->videoq.nb_packets <= MIN_FRAMES / 2)) ||
        ((is->audio_stream >= 0) && (is->audioq.nb_packets <= MIN_FRAMES / 2))  ) {
       ffgix_set_clock_speed(&is->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
   } else if ((is->video_stream < 0 || is->videoq.nb_packets > MIN_FRAMES * 2) &&
              (is->audio_stream < 0 || is->audioq.nb_packets > MIN_FRAMES * 2)) {
       ffgix_set_clock_speed(&is->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
   } else {
       double speed = is->extclk.speed;
       if (speed != 1.0)
           ffgix_set_clock_speed(&is->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
   }
}

/* seek in the stream */
void ffgix_stream_seek(VideoState *is, int64_t pos, int64_t rel, int seek_by_bytes)
{
    if (!is->seek_req) {
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (seek_by_bytes)
            is->seek_flags |= AVSEEK_FLAG_BYTE;
        is->seek_req = 1;
        pthread_cond_signal( &is->continue_read_thread ); //SDL_CondSignal(is->continue_read_thread);
    }
}

/* pause or resume the video */
void ffgix_stream_toggle_pause(VideoState *is)
{
    if (is->paused) {
        is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
        if (is->read_pause_return != AVERROR(ENOSYS)) {
            is->vidclk.paused = 0;
        }
        ffgix_set_clock(&is->vidclk, ffgix_get_clock(&is->vidclk), is->vidclk.serial);
    }
    ffgix_set_clock(&is->extclk, ffgix_get_clock(&is->extclk), is->extclk.serial);
    is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = !is->paused;
}

void ffgix_toggle_pause(VideoState *is)
{
    ffgix_stream_toggle_pause(is);
    is->step = 0;
}

void ffgix_step_to_next_frame(VideoState *is)
{
    /* if the stream is paused unpause it, then step */
    if (is->paused)
        ffgix_stream_toggle_pause(is);
    is->step = 1;
}

void ffgix_print_error(const char *filename, int err)
{
    char errbuf[128];
    const char *errbuf_ptr = errbuf;

    if (av_strerror(err, errbuf, sizeof(errbuf)) < 0)
        errbuf_ptr = strerror(AVUNERROR(err));
    av_log(NULL, AV_LOG_ERROR, "%s: %s\n", filename, errbuf_ptr);
}

double ffgix_compute_target_delay(double delay, VideoState *is)
{
    double sync_threshold, diff;
    int ret;
    diff = 0;
    ret = ffgix_get_master_sync_type(is);

    /* update delay to follow master synchronisation source */
    if (  ret != AV_SYNC_VIDEO_MASTER)
    {
        /* if video is slave, we try to correct big delays by duplicating or deleting a frame */
        diff = ffgix_get_clock(&is->vidclk) - ffgix_get_master_clock(is);

        /* skip or repeat frame. We take into account the delay to compute the threshold. I still don't know if it is the best guess */
        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        if (!isnan(diff) && fabs(diff) < is->max_frame_duration) {
            if (diff <= -sync_threshold)
                delay = FFMAX(0, delay + diff);
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
                delay = delay + diff;
            else if (diff >= sync_threshold)
                delay = 2 * delay;
        }
    }

    av_dlog(NULL, "video: delay=%0.3f A-V=%f\n", delay, -diff);

    return delay;
}

double ffgix_vp_duration(VideoState *is, Frame *vp, Frame *nextvp) {
    if (vp->serial == nextvp->serial) {
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration)
            return vp->duration;
        else
            return duration;
    } else {
        return 0.0;
    }
}


void ffgix_update_video_pts(VideoState *is, double pts, int64_t pos, int serial) {
    /* update current video pts */
    ffgix_set_clock(&is->vidclk, pts, serial);
    ffgix_sync_clock_to_slave(&is->extclk, &is->vidclk);
    pos++; //just to remove warning of unused pos
}

/* called to display each frame */
void ffgix_video_refresh(void *opaque, double *remaining_time)
{
    VideoState *is = opaque;
    double time;

    Frame *sp, *sp2;

    if (!is->paused && ffgix_get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime)
        ffgix_check_external_clock_speed(is);

    if (!display_disable && is->show_mode != SHOW_MODE_VIDEO && is->audio_st) {
        time = av_gettime_relative() / 1000000.0;
        if (is->force_refresh || is->last_vis_time + rdftspeed < time) {
            ffgix_video_display(is);
            is->last_vis_time = time;
        }
        *remaining_time = FFMIN(*remaining_time, is->last_vis_time + rdftspeed - time);
    }

    if (is->video_st) {
        int redisplay = 0;
        if (is->force_refresh)
            redisplay = ffgix_frame_queue_prev(&is->pictq);
retry:
        if (ffgix_frame_queue_nb_remaining(&is->pictq) == 0) {
            // nothing to do, no picture to display in the queue
        } else {
            double last_duration, duration, delay;
            Frame *vp, *lastvp;

            /* dequeue the picture */
            lastvp = ffgix_frame_queue_peek_last(&is->pictq);
            vp = ffgix_frame_queue_peek(&is->pictq);

            if (vp->serial != is->videoq.serial) {
                ffgix_frame_queue_next(&is->pictq);
                redisplay = 0;
                goto retry;
            }

            if (lastvp->serial != vp->serial && !redisplay)
                is->frame_timer = av_gettime_relative() / 1000000.0;

            if (is->paused)
                goto display;

            /* compute nominal last_duration */
            last_duration = ffgix_vp_duration(is, lastvp, vp);
            if (redisplay)
                delay = 0.0;
            else
                delay = ffgix_compute_target_delay(last_duration, is);

            time= av_gettime_relative()/1000000.0;
            if (time < is->frame_timer + delay && !redisplay) {
                *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
                return;
            }

            is->frame_timer += delay;
            if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
                is->frame_timer = time;

            pthread_mutex_lock( &is->pictq.mutex ); //SDL_LockMutex(is->pictq.mutex);
            if (!redisplay && !isnan(vp->pts))
                ffgix_update_video_pts(is, vp->pts, vp->pos, vp->serial);
            pthread_mutex_unlock( &is->pictq.mutex); //SDL_UnlockMutex(is->pictq.mutex);

            if (ffgix_frame_queue_nb_remaining(&is->pictq) > 1) {
                Frame *nextvp = ffgix_frame_queue_peek_next(&is->pictq);
                duration = ffgix_vp_duration(is, vp, nextvp);
                if(!is->step && (redisplay || framedrop>0 || (framedrop && ffgix_get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) && time > is->frame_timer + duration){
                    if (!redisplay)
                        is->frame_drops_late++;
                    ffgix_frame_queue_next(&is->pictq);
                    redisplay = 0;
                    goto retry;
                }
            }

            if (is->subtitle_st) {
                    while (ffgix_frame_queue_nb_remaining(&is->subpq) > 0) {
                        sp = ffgix_frame_queue_peek(&is->subpq);

                        if (ffgix_frame_queue_nb_remaining(&is->subpq) > 1)
                            sp2 = ffgix_frame_queue_peek_next(&is->subpq);
                        else
                            sp2 = NULL;

                        if (sp->serial != is->subtitleq.serial
                                || (is->vidclk.pts > (sp->pts + ((float) sp->sub.end_display_time / 1000)))
                                || (sp2 && is->vidclk.pts > (sp2->pts + ((float) sp2->sub.start_display_time / 1000))))
                        {
                            ffgix_frame_queue_next(&is->subpq);
                        } else {
                            break;
                        }
                    }
            }

display:
            /* display picture */
            if (!display_disable && is->show_mode == SHOW_MODE_VIDEO)
                ffgix_video_display(is);

            ffgix_frame_queue_next(&is->pictq);

            if (is->step && !is->paused)
                ffgix_stream_toggle_pause(is);
        }
    }
    is->force_refresh = 0;
    if (show_status) {
        static int64_t last_time;
        int64_t cur_time;
        int aqsize, vqsize, sqsize;
        double av_diff;

        cur_time = av_gettime_relative();
        if (!last_time || (cur_time - last_time) >= 30000) {
            aqsize = 0;
            vqsize = 0;
            sqsize = 0;
            if (is->audio_st)
                aqsize = is->audioq.size;
            if (is->video_st)
                vqsize = is->videoq.size;
            if (is->subtitle_st)
                sqsize = is->subtitleq.size;
            av_diff = 0;
            if (is->audio_st && is->video_st)
                av_diff = ffgix_get_clock(&is->audclk) - ffgix_get_clock(&is->vidclk);
            else if (is->video_st)
                av_diff = ffgix_get_master_clock(is) - ffgix_get_clock(&is->vidclk);
            else if (is->audio_st)
                av_diff = ffgix_get_master_clock(is) - ffgix_get_clock(&is->audclk);
            av_log(NULL, AV_LOG_INFO,
                   "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB f=%"PRId64"/%"PRId64"   \r",
                   ffgix_get_master_clock(is),
                   (is->audio_st && is->video_st) ? "A-V" : (is->video_st ? "M-V" : (is->audio_st ? "M-A" : "   ")),
                   av_diff,
                   is->frame_drops_early + is->frame_drops_late,
                   aqsize / 1024,
                   vqsize / 1024,
                   sqsize,
                   is->video_st ? is->video_st->codec->pts_correction_num_faulty_dts : 0,
                   is->video_st ? is->video_st->codec->pts_correction_num_faulty_pts : 0);
            fflush(stdout);
            last_time = cur_time;
        }
    }
}

/* allocate a picture (needs to do that in main thread to avoid
   potential locking problems */
void ffgix_alloc_picture(VideoState *is)
{
    Frame *vp;
    int64_t bufferdiff;
    AVPicture pict;

    vp = &is->pictq.queue[is->pictq.windex];

    ffgix_free_picture(vp);
#ifndef TARGET_OS_IPHONE
    ffgix_video_open(is, 0, vp);
    renderer = SDL_CreateRenderer(screen, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);


    vp->bmp = SDL_CreateTexture( renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING,
                       vp->width, vp->height);
    //vp->bmp = SDL_CreateYUVOverlay(vp->width, vp->height,
    //                               SDL_YV12_OVERLAY,
    //                               screen);

    SDL_LockTexture(vp->bmp, NULL, (void **)pict.data, pict.linesize);

    bufferdiff = vp->bmp ? FFMAX(pict.data[0], pict.data[1]) - FFMIN(pict.data[0], pict.data[1]) : 0;
    if (!vp->bmp || pict.linesize[0] < vp->width || bufferdiff < (int64_t)vp->height * pict.linesize[0]) {
        /* SDL allocates a buffer smaller than requested if the video
         * overlay hardware is unable to support the requested size. */
        av_log(NULL, AV_LOG_FATAL,
               "Error: the video system does not support an image\n"
                        "size of %dx%d pixels. Try using -lowres or -vf \"scale=w:h\"\n"
                        "to reduce the image size.\n", vp->width, vp->height );

        SDL_UnlockTexture(vp->bmp);
        ffgix_do_exit(is);
    }
    SDL_UnlockTexture(vp->bmp);

    pthread_mutex_lock( &is->pictq.mutex ); //SDL_LockMutex(is->pictq.mutex);
    vp->allocated = 1;
    pthread_cond_signal( &is->pictq.cond ); //SDL_CondSignal(is->pictq.cond);
    pthread_mutex_unlock( &is->pictq.mutex ); //SDL_UnlockMutex(is->pictq.mutex);
#endif
}

void ffgix_duplicate_right_border_pixels(SDL_Texture *bmp) {
    int i, width, height, bw, bh;
    Uint8 *p, *maxp;
    AVPicture pict;

    Uint32 format;
    int access;

#ifndef TARGET_OS_IPHONE
    errno = 0;
    if( SDL_QueryTexture( bmp, &format, &access , &bw, &bh ) != 0){
        ALog("SDL_QueryTexture failed.%s", strerror(errno));
    }

    SDL_LockTexture( bmp, NULL, (void **)pict.data, pict.linesize);
    for (i = 0; i < 3; i++) {
        width  = bw;
        height = bh;
        if (i > 0) {
            width  >>= 1;
            height >>= 1;
        }

        if (pict.linesize[i] > width) {
            maxp = pict.data[i] + pict.linesize[i] * height - 1;
            for (p = pict.data[i] + width - 1; p < maxp; p += pict.linesize[i])
                *(p+1) = *p;
        }
    }
    SDL_UnlockTexture( bmp);
#endif
    
}

int ffgix_queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)
{
    Frame *vp;
    AVPicture texturePict;

#if defined(DEBUG_SYNC) && 0
    printf("frame_type=%c pts=%0.3f\n",
           av_get_picture_type_char(src_frame->pict_type), pts);
#endif

    if (!(vp = ffgix_frame_queue_peek_writable(&is->pictq)))
        return -1;

    vp->sar = src_frame->sample_aspect_ratio;
#ifndef TARGET_OS_IPHONE
    /* alloc or resize hardware picture buffer */
    if (!vp->bmp || vp->reallocate || !vp->allocated ||
        vp->width  != src_frame->width ||
        vp->height != src_frame->height) {
        SDL_Event event;

        vp->allocated  = 0;
        vp->reallocate = 0;
        vp->width = src_frame->width;
        vp->height = src_frame->height;

        /* the allocation must be done in the main thread to avoid
           locking problems. */
        event.type = FF_ALLOC_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);

        /* wait until the picture is allocated */
        pthread_mutex_lock( &is->pictq.mutex ); //SDL_LockMutex(is->pictq.mutex);
        while (!vp->allocated && !is->videoq.abort_request) {
            pthread_cond_wait( &is->pictq.cond, &is->pictq.mutex ); //SDL_CondWait(is->pictq.cond, is->pictq.mutex);
        }

        /* if the queue is aborted, we have to pop the pending ALLOC event or wait for the allocation to complete */
        if (is->videoq.abort_request && SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) != 1) {
            while (!vp->allocated && !is->abort_request) {
                pthread_cond_wait( &is->pictq.cond, &is->pictq.mutex); //SDL_CondWait(is->pictq.cond, is->pictq.mutex);
            }
        }

        pthread_mutex_unlock( &is->pictq.mutex ); //SDL_UnlockMutex(is->pictq.mutex);

        if (is->videoq.abort_request)
            return -1;
    }

    /* if the frame is not skipped, then display it */
    if (vp->bmp) {
        AVPicture pict = { { 0 }, { 0 } };

        /* get a pointer on the bitmap */
        //SDL_LockYUVOverlay (vp->bmp);
        SDL_LockTexture( vp->bmp, NULL, (void **)texturePict.data, texturePict.linesize);

        pict.data[0] = texturePict.data[0];
        pict.data[1] = texturePict.data[2];
        pict.data[2] = texturePict.data[1];

        pict.linesize[0] = texturePict.linesize[0];
        pict.linesize[1] = texturePict.linesize[2];
        pict.linesize[2] = texturePict.linesize[1];



#if CONFIG_AVFILTER
        // FIXME use direct rendering
        av_picture_copy(&pict, (AVPicture *)src_frame,
                        src_frame->format, vp->width, vp->height);
#else
        av_opt_get_int(sws_opts, "sws_flags", 0, &sws_flags);
        is->img_convert_ctx = sws_getCachedContext(is->img_convert_ctx,
            vp->width, vp->height, src_frame->format, vp->width, vp->height,
            AV_PIX_FMT_YUV420P, sws_flags, NULL, NULL, NULL);
        if (!is->img_convert_ctx) {
            av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
            exit(1);
        }
        sws_scale(is->img_convert_ctx, src_frame->data, src_frame->linesize,
                  0, vp->height, pict.data, pict.linesize);
#endif
        /* workaround SDL PITCH_WORKAROUND */
        ffgix_duplicate_right_border_pixels(vp->bmp);
        /* update the bitmap content */
        SDL_UnlockTexture(vp->bmp); //SDL_UnlockYUVOverlay(vp->bmp);

        vp->pts = pts;
        vp->duration = duration;
        vp->pos = pos;
        vp->serial = serial;

        /* now we can update the picture count */
        ffgix_frame_queue_push(&is->pictq);
    }
#endif
    return 0;
}

int ffgix_get_video_frame(VideoState *is, AVFrame *frame)
{
    int got_picture;

    if ((got_picture = ffgix_decoder_decode_frame(&is->viddec, frame, NULL)) < 0)
        return -1;

    if (got_picture) {
        double dpts = NAN;

        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(is->video_st->time_base) * frame->pts;

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

        if (framedrop>0 || (framedrop && ffgix_get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) {
            if (frame->pts != AV_NOPTS_VALUE) {
                double diff = dpts - ffgix_get_master_clock(is);
                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                    diff - is->frame_last_filter_delay < 0 &&
                    is->viddec.pkt_serial == is->vidclk.serial &&
                    is->videoq.nb_packets) {
                    is->frame_drops_early++;
                    av_frame_unref(frame);
                    got_picture = 0;
                }
            }
        }
    }

    return got_picture;
}

#if CONFIG_AVFILTER
int ffgix_configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                                 AVFilterContext *source_ctx, AVFilterContext *sink_ctx)
{
    int ret;
    uint i;
    int nb_filters = graph->nb_filters;
    AVFilterInOut *outputs = NULL, *inputs = NULL;

    if (filtergraph) {
        outputs = avfilter_inout_alloc();
        inputs  = avfilter_inout_alloc();
        if (!outputs || !inputs) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        outputs->name       = av_strdup("in");
        outputs->filter_ctx = source_ctx;
        outputs->pad_idx    = 0;
        outputs->next       = NULL;

        inputs->name        = av_strdup("out");
        inputs->filter_ctx  = sink_ctx;
        inputs->pad_idx     = 0;
        inputs->next        = NULL;

        if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, NULL)) < 0)
            goto fail;
    } else {
        if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0)
            goto fail;
    }

    /* Reorder the filters to ensure that inputs of the custom filters are merged first */
    for (i = 0; i < graph->nb_filters - nb_filters; i++)
        FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);

    ret = avfilter_graph_config(graph, NULL);
fail:
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    return ret;
}

int ffgix_configure_video_filters(AVFilterGraph *graph, VideoState *is, const char *vfilters, AVFrame *frame)
{
    static const enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };
    char sws_flags_str[128];
    char buffersrc_args[256];
    int ret;
    AVFilterContext *filt_src = NULL, *filt_out = NULL, *last_filter = NULL;
    AVCodecContext *codec = is->video_st->codec;
    AVRational fr = av_guess_frame_rate(is->ic, is->video_st, NULL);

    av_opt_get_int(sws_opts, "sws_flags", 0, &sws_flags);
    snprintf(sws_flags_str, sizeof(sws_flags_str), "flags=%"PRId64, sws_flags);
    graph->scale_sws_opts = av_strdup(sws_flags_str);

    snprintf(buffersrc_args, sizeof(buffersrc_args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             frame->width, frame->height, frame->format,
             is->video_st->time_base.num, is->video_st->time_base.den,
             codec->sample_aspect_ratio.num, FFMAX(codec->sample_aspect_ratio.den, 1));
    if (fr.num && fr.den)
        av_strlcatf(buffersrc_args, sizeof(buffersrc_args), ":frame_rate=%d/%d", fr.num, fr.den);

    if ((ret = avfilter_graph_create_filter(&filt_src,
                                            avfilter_get_by_name("buffer"),
                                            "ffplay_buffer", buffersrc_args, NULL,
                                            graph)) < 0)
        goto fail;

    ret = avfilter_graph_create_filter(&filt_out,
                                       avfilter_get_by_name("buffersink"),
                                       "ffplay_buffersink", NULL, NULL, graph);
    if (ret < 0)
        goto fail;

    if ((ret = av_opt_set_int_list(filt_out, "pix_fmts", pix_fmts,  AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto fail;

    last_filter = filt_out;

/* Note: this macro adds a filter before the lastly added filter, so the
 * processing order of the filters is in reverse */
#define INSERT_FILT(name, arg) do {                                         \
    AVFilterContext *filt_ctx;                                              \
                                                                            \
    ret = avfilter_graph_create_filter(&filt_ctx,                           \
                                       avfilter_get_by_name(name),          \
                                       "ffplay_" name, arg, NULL, graph);   \
    if (ret < 0)                                                            \
        goto fail;                                                          \
                                                                            \
    ret = avfilter_link(filt_ctx, 0, last_filter, 0);                       \
    if (ret < 0)                                                            \
        goto fail;                                                          \
                                                                            \
    last_filter = filt_ctx;                                                 \
} while (0)

    /* SDL YUV code is not handling odd width/height for some driver
     * combinations, therefore we crop the picture to an even width/height. */
    INSERT_FILT("crop", "floor(in_w/2)*2:floor(in_h/2)*2");

    if (autorotate) {
        AVDictionaryEntry *rotate_tag = av_dict_get(is->video_st->metadata, "rotate", NULL, 0);
        if (rotate_tag && *rotate_tag->value && strcmp(rotate_tag->value, "0")) {
            if (!strcmp(rotate_tag->value, "90")) {
                INSERT_FILT("transpose", "clock");
            } else if (!strcmp(rotate_tag->value, "180")) {
                INSERT_FILT("hflip", NULL);
                INSERT_FILT("vflip", NULL);
            } else if (!strcmp(rotate_tag->value, "270")) {
                INSERT_FILT("transpose", "cclock");
            } else {
                char rotate_buf[64];
                snprintf(rotate_buf, sizeof(rotate_buf), "%s*PI/180", rotate_tag->value);
                INSERT_FILT("rotate", rotate_buf);
            }
        }
    }

    if ((ret = ffgix_configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
        goto fail;

    is->in_video_filter  = filt_src;
    is->out_video_filter = filt_out;

fail:
    return ret;
}

int ffgix_configure_audio_filters(VideoState *is, const char *afilters, int force_output_format)
{
    static const enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE };
    int sample_rates[2] = { 0, -1 };
    int64_t channel_layouts[2] = { 0, -1 };
    int channels[2] = { 0, -1 };
    AVFilterContext *filt_asrc = NULL, *filt_asink = NULL;
    char aresample_swr_opts[512] = "";
    AVDictionaryEntry *e = NULL;
    char asrc_args[256];
    int ret;

    avfilter_graph_free(&is->agraph);
    if (!(is->agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);

    while ((e = av_dict_get(swr_opts, "", e, AV_DICT_IGNORE_SUFFIX)))
        av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
    if (strlen(aresample_swr_opts))
        aresample_swr_opts[strlen(aresample_swr_opts)-1] = '\0';
    av_opt_set(is->agraph, "aresample_swr_opts", aresample_swr_opts, 0);

    ret = snprintf(asrc_args, sizeof(asrc_args),
                   "sample_rate=%d:sample_fmt=%s:channels=%d:time_base=%d/%d",
                   is->audio_filter_src.freq, av_get_sample_fmt_name(is->audio_filter_src.fmt),
                   is->audio_filter_src.channels,
                   1, is->audio_filter_src.freq);
    if (is->audio_filter_src.channel_layout)
        snprintf(asrc_args + ret, sizeof(asrc_args) - ret,
                 ":channel_layout=0x%"PRIx64,  is->audio_filter_src.channel_layout);

    ret = avfilter_graph_create_filter(&filt_asrc,
                                       avfilter_get_by_name("abuffer"), "ffplay_abuffer",
                                       asrc_args, NULL, is->agraph);
    if (ret < 0)
        goto end;


    ret = avfilter_graph_create_filter(&filt_asink,
                                       avfilter_get_by_name("abuffersink"), "ffplay_abuffersink",
                                       NULL, NULL, is->agraph);
    if (ret < 0)
        goto end;

    if ((ret = av_opt_set_int_list(filt_asink, "sample_fmts", sample_fmts,  AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;
    if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;

    if (force_output_format) {
        channel_layouts[0] = is->audio_tgt.channel_layout;
        channels       [0] = is->audio_tgt.channels;
        sample_rates   [0] = is->audio_tgt.freq;
        if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 0, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "channel_layouts", channel_layouts,  -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "channel_counts" , channels       ,  -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "sample_rates"   , sample_rates   ,  -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
    }


    if ((ret = ffgix_configure_filtergraph(is->agraph, afilters, filt_asrc, filt_asink)) < 0)
        goto end;

    is->in_audio_filter  = filt_asrc;
    is->out_audio_filter = filt_asink;

end:
    if (ret < 0)
        avfilter_graph_free(&is->agraph);
    return ret;
}

AVDictionary **ffgix_setup_find_stream_info_opts(AVFormatContext *s, AVDictionary *codec_opts)
{
    uint i;
    AVDictionary **opts;

    if (!s->nb_streams)
        return NULL;
    opts = av_mallocz_array(s->nb_streams, sizeof(*opts));
    if (!opts) {
        av_log(NULL, AV_LOG_ERROR,
               "Could not alloc memory for stream options.\n");
        return NULL;
    }
    for (i = 0; i < s->nb_streams; i++)
        opts[i] = ffgix_filter_codec_opts(codec_opts, s->streams[i]->codec->codec_id,
                                    s, s->streams[i], NULL);
    return opts;
}

AVDictionary *ffgix_filter_codec_opts(AVDictionary *opts, enum AVCodecID codec_id,
                                AVFormatContext *s, AVStream *st, AVCodec *codec)
{
    AVDictionary    *ret = NULL;
    AVDictionaryEntry *t = NULL;
    int            flags = s->oformat ? AV_OPT_FLAG_ENCODING_PARAM
                                      : AV_OPT_FLAG_DECODING_PARAM;
    char          prefix = 0;
    const AVClass    *cc = avcodec_get_class();

    if (!codec)
        codec            = s->oformat ? avcodec_find_encoder(codec_id)
                                      : avcodec_find_decoder(codec_id);

    switch (st->codec->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        prefix  = 'v';
        flags  |= AV_OPT_FLAG_VIDEO_PARAM;
        break;
    case AVMEDIA_TYPE_AUDIO:
        prefix  = 'a';
        flags  |= AV_OPT_FLAG_AUDIO_PARAM;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        prefix  = 's';
        flags  |= AV_OPT_FLAG_SUBTITLE_PARAM;
        break;
    default:
        break;
    }

    while ( (t = av_dict_get(opts, "", t, AV_DICT_IGNORE_SUFFIX)) ) {
        char *p = strchr(t->key, ':');

        /* check stream specification in opt name */
        if (p)
            switch (ffgix_check_stream_specifier(s, st, p + 1)) {
            case  1: *p = 0; break;
            case  0:         continue;
            default:         ffgix_exit_program(1);
            }

        if (av_opt_find(&cc, t->key, NULL, flags, AV_OPT_SEARCH_FAKE_OBJ) ||
            !codec ||
            (codec->priv_class &&
             av_opt_find(&codec->priv_class, t->key, NULL, flags,
                         AV_OPT_SEARCH_FAKE_OBJ)))
            av_dict_set(&ret, t->key, t->value, 0);
        else if (t->key[0] == prefix &&
                 av_opt_find(&cc, t->key + 1, NULL, flags,
                             AV_OPT_SEARCH_FAKE_OBJ))
            av_dict_set(&ret, t->key + 1, t->value, 0);

        if (p)
            *p = ':';
    }
    return ret;
}

int ffgix_check_stream_specifier(AVFormatContext *s, AVStream *st, const char *spec)
{
    int ret = avformat_match_stream_specifier(s, st, spec);
    if (ret < 0)
        av_log(s, AV_LOG_ERROR, "Invalid stream specifier: %s.\n", spec);
    return ret;
}

void ffgix_exit_program(int ret)
{
    if (program_exit)
        program_exit(ret);

    exit(ret);
}

void ffgix_init_opts(void)
{
    av_dict_set(&sws_dict, "flags", "bicubic", 0);
}

void ffgix_opt_input_file(void *optctx, const char *filename)
{
    if(optctx){
        ALog("optctx specified");
    }
    if (input_filename) {
        av_log(NULL, AV_LOG_FATAL,
               "Argument '%s' provided as input filename, but '%s' was already specified.\n",
                filename, input_filename);
        exit(1);
    }
    if (!strcmp(filename, "-")) filename = "pipe:";
    input_filename = filename;
}


#endif  /* CONFIG_AVFILTER */
