/*
 * copyright (c) 2024 Jack Lau
 * 
 * This file is a tutorial about playing(decoding and rendering) video 
 * and audio through ffmpeg and SDL API 
 * 
 * FFmpeg version 5.1.4
 * SDL2 version 2.30.3
 *
 * Tested on MacOS 14.7.1, compiled with clang 14.0.3
 */

#include <SDL.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/fifo.h>

#include <pthread.h>

#define AUDIO_BUFFER_SIZE 1024
#define VIDEO_PICTURE_QUEUE_SIZE 1

#define ONESECOND 1000

#define true 1
#define false 0

typedef struct MyPacketEle{
    AVPacket *pkt;
}MyPacketEle;

typedef struct PacketQueue{
    AVFifo *pkts;
    int nb_packets;
    int size;
    int64_t duration;
    SDL_mutex *mutex;
    SDL_cond *cond;
}PacketQueue;

typedef struct VideoPicture
{
    AVFrame *   frame;
    int         width;
    int         height;
    int         allocated;
} VideoPicture;

typedef struct VideoState{
    AVFormatContext   *fmtCtx;
    AVStream          *aInStream;
    AVStream          *vInStream;
    AVCodecContext    *aCtx;
    AVCodecContext    *vCtx;
    const AVCodec     *aDecodec;
    const AVCodec     *vDecodec;

    AVPacket          *aPkt;
    AVPacket          *vPkt;
    AVFrame           *aFrame;
    AVFrame           *vFrame;

    struct SwrContext *swr_ctx;

    uint8_t           *audio_buf;
    uint              audio_buf_size;
    int               audio_buf_index;

    PacketQueue       audioQueue;
    /* cache video frame for sync thread */
    PacketQueue       videoQueue;
    VideoPicture      pictq[VIDEO_PICTURE_QUEUE_SIZE];
    int               pictq_size;
    int               pictq_rindex;
    int               pictq_windex;
    SDL_mutex *       pictq_mutex;
    SDL_cond *        pictq_cond;

    /* SDL variable */
    SDL_Texture       *texture;
    SDL_Renderer      *renderer;

    int               aIdx;
    int               vIdx;
    
    int               frameRate;

    char              *srcFilePath;
}VideoState;

SDL_mutex * screen_mutex;

static int w_width = 640;
static int w_height = 480;

static SDL_Window *screen = NULL;
// static SDL_Renderer *renderer = NULL;

#define FF_REFRESH_EVENT (SDL_USEREVENT)

static int quit = false;

static int eof = false;

static int packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->pkts = av_fifo_alloc2(1, sizeof(MyPacketEle), AV_FIFO_FLAG_AUTO_GROW);
    if(!q->pkts){
        av_log(NULL, AV_LOG_ERROR, "No Memory!\n");
        return AVERROR(ENOMEM);
    }
    q->mutex = SDL_CreateMutex();
    if(!q->mutex){
        av_log(NULL, AV_LOG_ERROR, "No Memory!\n");
        return AVERROR(ENOMEM);
    }
    q->cond = SDL_CreateCond();
    if(!q->cond){
        av_log(NULL, AV_LOG_ERROR, "No Memory!\n");
        return AVERROR(ENOMEM);
    }
    return 0;
}

static int packet_queue_put_priv(PacketQueue *q, AVPacket *pkt)
{
    MyPacketEle mypkt;
    int ret = -1;

    mypkt.pkt = pkt;

    ret = av_fifo_write(q->pkts, &mypkt, 1);
    if(ret < 0){
        return ret;
    }
    //update the queue info
    q->nb_packets++;
    q->size += mypkt.pkt->size + sizeof(mypkt);
    q->duration += mypkt.pkt->duration;
    //
    SDL_CondSignal(q->cond);

    return ret;
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    MyPacketEle mypkt;
    int ret = -1;

    SDL_LockMutex(q->mutex);
    for (;;){
        if(av_fifo_read(q->pkts, &mypkt, 1)>=0){
            q->nb_packets--;
            q->size -= mypkt.pkt->size + sizeof(mypkt);
            q->duration -= mypkt.pkt->duration;
            av_packet_move_ref(pkt, mypkt.pkt);
            av_packet_free(&mypkt.pkt);
            ret = 1;
            break;
        }else if(!block){
            ret = 0;
            break;
        }else{
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    
    return ret;
}

static void packet_queue_flush(PacketQueue *q)
{
    MyPacketEle mypkt;
    SDL_LockMutex(q->mutex);

    while (av_fifo_read(q->pkts, &mypkt, 1) > 0){
        av_packet_free(&mypkt.pkt);
    }
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;

    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_destroy(PacketQueue *q)
{
    packet_queue_flush(q);
    av_fifo_freep2(&q->pkts);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond); 
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    AVPacket *pkt1;
    int ret = -1;
    
    SDL_UnlockMutex(q->mutex);

    pkt1 = av_packet_alloc();
    if(!pkt1){
        av_packet_unref(pkt);
        return -1;
    }
    av_packet_move_ref(pkt1, pkt);

    SDL_LockMutex(q->mutex);

    ret = packet_queue_put_priv(q, pkt1);

    SDL_UnlockMutex(q->mutex);

    if(ret < 0){
        av_packet_free(&pkt1);
    }
    return ret;
}

static void render(VideoState *is)
{

    SDL_UpdateYUVTexture(is->texture, NULL,
                         is->vFrame->data[0], is->vFrame->linesize[0] ,
                         is->vFrame->data[1], is->vFrame->linesize[1],
                         is->vFrame->data[2], is->vFrame->linesize[2]);
    
    SDL_RenderClear(is->renderer);
    SDL_RenderCopy(is->renderer, is->texture, NULL, NULL);
    SDL_RenderPresent(is->renderer);

    // Uint32 delayTime = (Uint32)(ONESECOND/is->frameRate);
    // if(is->frameRate <= 0){
    //     av_log(NULL, AV_LOG_ERROR,  "Failed to get framerate!\n");
    //     SDL_Delay(33);
    //     return;
    // }
    // av_log(NULL, AV_LOG_DEBUG, "delayTime: %d\n", delayTime);
    SDL_Delay((Uint32)22);
    return;
}

static int decode(VideoState *is)
{
    int ret = -1;

    char buffer[1024];
    //send packet to decoder
    ret = avcodec_send_packet(is->vCtx, is->vPkt);
    if(ret < 0){
        av_log(NULL, AV_LOG_ERROR, "Failed to send frame to decoder!\n");
        goto end;
    }

    while (ret >= 0)
    {
        ret = avcodec_receive_frame(is->vCtx, is->vFrame);
        if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
            ret = 0;
            goto end;
        }else if(ret < 0){
            ret = -1;
            goto end;
        }
        render(is);
    }
    

end:
    return ret;
}

static void *video_thread(void * arg)
{
    // retrieve global VideoState reference
    VideoState * videoState = (VideoState *)arg;

    AVPacket * packet = av_packet_alloc();
    if (packet == NULL)
    {
        printf("Could not alloc packet.\n");
        //return;
    }

    int frameFinished;

    // allocate a new AVFrame, used to decode video packets
    static AVFrame * pFrame = NULL;
    pFrame = av_frame_alloc();
    if (!pFrame)
    {
        printf("Could not allocate AVFrame.\n");
        // return;
    }

    while (!quit)
    {
        if (packet_queue_get(&videoState->videoQueue, packet, 0) <= 0 && eof == true)
        {
            // means we quit getting packets
            av_log(NULL, AV_LOG_INFO, "can't get video pkt\n");
            quit = true;
            break;
        }

        // give the decoder raw compressed data in an AVPacket
        int ret = avcodec_send_packet(videoState->vCtx, packet);
        if (ret < 0)
        {
            printf("Error sending packet for decoding.\n");
            // return;
        }

        while (ret >= 0)
        {
            // get decoded output data from decoder
            ret = avcodec_receive_frame(videoState->vCtx, pFrame);

            // check an entire frame was decoded
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            {
                break;
            }
            else if (ret < 0)
            {
                printf("Error while decoding.\n");
                // return;
            }
            else
            {
                frameFinished = 1;
            }

            // Did we get a video frame?
            if (frameFinished)
            {
                // if(queue_picture(videoState, pFrame) < 0)
                // {
                //     break;
                // }
                videoState->vFrame = pFrame;
                render(videoState);
            }
        }

        // wipe the packet
        av_packet_unref(packet);
    }

    av_frame_free(&pFrame);
    av_free(pFrame);

    // return;
}

static int audio_decode_frame(VideoState *is)
{
    int ret = -1;
    int len2 = 0;

    int data_size = 0;
    AVPacket *pkt = av_packet_alloc();
    while (!quit) {
        if(packet_queue_get(&is->audioQueue, pkt, 0) <= 0 && eof == true){
            av_log(NULL, AV_LOG_INFO, "couldn't obtain audio pkt\n");
            quit = false;
            return -1;
        }

        ret = avcodec_send_packet(is->aCtx, pkt);
        if(ret < 0){
            av_log(is->aCtx, AV_LOG_ERROR, "Failed to send pkt to audio decoder!\n");
            goto end;
        }
        while (ret >= 0)
        {
            ret = avcodec_receive_frame(is->aCtx, is->aFrame);
            if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
                break;
            }else if(ret < 0){
                av_log(is->aCtx, AV_LOG_ERROR, "Failed to receive frame from audio decoder!\n");
                goto end;
            }

            
            //re-sampling
            if(!is->swr_ctx){
                AVChannelLayout in_ch_layout, out_ch_layout;
                av_channel_layout_copy(&in_ch_layout, &is->aCtx->ch_layout);
                av_channel_layout_copy(&out_ch_layout, &in_ch_layout);
                if(is->aCtx->sample_fmt != AV_SAMPLE_FMT_S16){
                    swr_alloc_set_opts2(&is->swr_ctx, 
                                    &out_ch_layout, 
                                    AV_SAMPLE_FMT_S16,
                                    is->aCtx->sample_rate,
                                    &in_ch_layout,
                                    is->aCtx->sample_fmt,
                                    is->aCtx->sample_rate,
                                    0,
                                    NULL);

                    swr_init(is->swr_ctx);
                }
            }

            if(is->swr_ctx){
                const uint8_t **in = (const uint8_t **)is->aFrame->extended_data;
                int in_count = is->aFrame->nb_samples;
                uint8_t **out = &is->audio_buf;
                int out_count = is->aFrame->nb_samples + 512;

                int out_size = av_samples_get_buffer_size(NULL, is->aFrame->ch_layout.nb_channels, out_count, AV_SAMPLE_FMT_S16, 0);
                av_fast_malloc(&is->audio_buf, &is->audio_buf_size, out_size);

                int samples = swr_convert(is->swr_ctx,
                            out,
                            out_count,
                            in,
                            in_count);

                //data_size = len2 * is->aFrame->ch_layout.nb_channels*av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
                data_size = samples * 2 * 2; 
            }else{
                is->audio_buf = is->aFrame->data[0];
                data_size = av_samples_get_buffer_size(NULL, is->aFrame->ch_layout.nb_channels, is->aFrame->nb_samples, is->aFrame->format, 1);
            }

            av_packet_unref(pkt);
            av_frame_unref(is->aFrame);

            return data_size;

        }
        
    }
end:
    return ret;

}

static void sdl_audio_callback(void *userdata, Uint8 *stream, int len)
{
    int len1 = 0;
    int audio_size = 0;
    VideoState *is = (VideoState*)userdata;


    if (len > 0){
        if(is->audio_buf_index >= is->audio_buf_size){
            audio_size = audio_decode_frame(is);
            if(audio_size < 0){
                is->audio_buf_size = AUDIO_BUFFER_SIZE;
                is->audio_buf = NULL;
            }else {
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
    }
    len1 = is->audio_buf_size - is->audio_buf_index;
    if(len1 > len){
        len1 = len;
    }
    if(is->audio_buf){
        memcpy(stream, (uint8_t*)(is->audio_buf + is->audio_buf_index), len1);
    }else{
        memset(stream, 0, len1);
    }

    len -= len1;
    stream += len1;
    is->audio_buf_index += len1;
}

static int open_media(VideoState *is) {
    int ret = -1;
    //open multimedia file and get stream info
    if( (ret = avformat_open_input(&is->fmtCtx, is->srcFilePath, NULL, NULL)) < 0 ){
        av_log(NULL, AV_LOG_ERROR, " %s \n", av_err2str(ret));
        goto end;
    }
    if((ret = avformat_find_stream_info(is->fmtCtx, NULL)) < 0){
        av_log(NULL, AV_LOG_ERROR, "%s\n", av_err2str(ret));
        goto end;
    }
    //find the best stream
    for(int i = 0; i < is->fmtCtx->nb_streams; i++){
        if(is->fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && is->vIdx < 0){
            is->vIdx = i;
        }
        if(is->fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && is->aIdx < 0){
            is->aIdx = i;
        }
        if(is->vIdx > -1 && is->aIdx > -1){
            break;
        }
    }
    if(is->vIdx == -1){
        av_log(NULL, AV_LOG_ERROR, "Couldn't find video stream!\n");
        goto end;
    }
    if(is->aIdx == -1){
        av_log(NULL, AV_LOG_ERROR, "Couldn't find audio stream!\n");
        goto end;
    }
    is->aInStream = is->fmtCtx->streams[is->aIdx];
    is->vInStream = is->fmtCtx->streams[is->vIdx];

    
    //get decodec by codec_id from stream info
    is->vDecodec = avcodec_find_decoder(is->vInStream->codecpar->codec_id);
    if(!is->vDecodec){
        av_log(NULL, AV_LOG_ERROR, "Couldn't find codec: libx264 \n");
        goto end;
    }
    //init decoder context
    is->vCtx = avcodec_alloc_context3(is->vDecodec);
    if(!is->vCtx){
        av_log(NULL, AV_LOG_ERROR, "No memory!\n");
        goto end;
    }
    //copy parameters 
    ret = avcodec_parameters_to_context(is->vCtx, is->vInStream->codecpar);
    if(ret < 0){
        av_log(is->vCtx, AV_LOG_ERROR, "Couldn't copy codecpar to codecContext");
        goto end;
    }
    //bind decoder and decoder context
    ret = avcodec_open2(is->vCtx, is->vDecodec, NULL);
    if(ret < 0){
        av_log(NULL, AV_LOG_ERROR, "Couldn't open the codec: %s\n", av_err2str(ret));
        goto end;
    }

    //get decodec by codec_id from stream info
    is->aDecodec = avcodec_find_decoder(is->aInStream->codecpar->codec_id);
    if(!is->aDecodec){
        av_log(NULL, AV_LOG_ERROR, "Couldn't find codec: libx264 \n");
        goto end;
    }
    //init decoder context
    is->aCtx = avcodec_alloc_context3(is->aDecodec);
    if(!is->aCtx){
        av_log(NULL, AV_LOG_ERROR, "No memory!\n");
        goto end;
    }
    //copy parameters 
    ret = avcodec_parameters_to_context(is->aCtx, is->aInStream->codecpar);
    if(ret < 0){
        av_log(is->aCtx, AV_LOG_ERROR, "Couldn't copy codecpar to codecContext");
        goto end;
    }
    //bind decoder and decoder context
    ret = avcodec_open2(is->aCtx, is->aDecodec, NULL);
    if(ret < 0){
        av_log(NULL, AV_LOG_ERROR, "Couldn't open the codec: %s\n", av_err2str(ret));
        goto end;
    }
    //init
    packet_queue_init(&is->audioQueue);
    packet_queue_init(&is->videoQueue);
    screen_mutex = SDL_CreateMutex();
end:
    return ret;
}

// Function to be run on the main thread for event handling
void sdl_event_thread() {
    SDL_Event event;
    static int running = true;
    while (running && !quit) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
                quit = true;
                break;
            }
            // Handle other events...
        }
        SDL_Delay(10); // Reduce CPU usage
    }
}

static void *decode_loop(void *args) {
    VideoState *is = (VideoState *)args;
    AVPacket *pkt = av_packet_alloc();
    static int cnt = 0;
    int ret = -1;
    //demux media
    while (ret = av_read_frame(is->fmtCtx, pkt) >= 0 && !quit) {
        if (pkt->stream_index == is->vIdx ){
            packet_queue_put(&is->videoQueue, pkt);
        } else if (pkt->stream_index == is->aIdx) {
            packet_queue_put(&is->audioQueue, pkt);
        } else {
            av_packet_unref(pkt);
        }
    }
    is->vPkt = NULL;
    eof = true;
}

static void video_state_init(VideoState *is) {
    is->fmtCtx = NULL;
    is->aInStream = NULL;
    is->vInStream = NULL;
    is->aCtx = NULL;
    is->vCtx = NULL;
    is->aDecodec = NULL;
    is->vDecodec = NULL;
    is->aIdx = -1;
    is->vIdx = -1;
    
    is->aPkt = av_packet_alloc();
    is->aFrame = av_frame_alloc();

    is->vPkt = av_packet_alloc();
    is->vFrame = av_frame_alloc();

    is->texture = NULL;
    is->renderer = NULL;
}

static int sdl_init(VideoState *is) {
    //init SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "Couldn't initialize SDL - %s\n", SDL_GetError);
        return -1;
    }
    //create window from SDL
    screen = SDL_CreateWindow("simple player",
                            SDL_WINDOWPOS_UNDEFINED,
                            SDL_WINDOWPOS_UNDEFINED,
                            w_width, w_height,
                            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if(!screen){
        fprintf(stderr, "Failed to create window, %s\n", SDL_GetError);
        return -1;
    }   
    is->renderer = SDL_CreateRenderer(screen, -1, 0);
    
    //create texture for render
    Uint32 pixformat = SDL_PIXELFORMAT_IYUV;
    is->texture = SDL_CreateTexture(is->renderer, pixformat, SDL_TEXTUREACCESS_STREAMING, 
                                    is->vCtx->width, is->vCtx->height);

    SDL_AudioSpec wanted_spec, spec;
    //set the parameters for audio device
    wanted_spec.freq = is->aCtx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = is->aCtx->ch_layout.nb_channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = AUDIO_BUFFER_SIZE;
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = (void*)is;

    if(SDL_OpenAudio(&wanted_spec, &spec)<0){
        av_log(NULL, AV_LOG_ERROR, "Failed to open audio device!\n");
        goto end;
    }
    SDL_PauseAudio(0);
end:
    return 0;
}

int main(int argc, char *argv[])
{

    int ret = -1;

    SDL_Event event;

    AVPacket *pkt = NULL;

    VideoState *is = av_mallocz(sizeof(VideoState));
    if(!is){
        av_log(NULL, AV_LOG_ERROR, "No Memory!\n");
        goto end;
    }
    video_state_init(is);
    // initialize locks for the display buffer (pictq)
    is->pictq_mutex = SDL_CreateMutex();
    is->pictq_cond = SDL_CreateCond();


    av_log_set_level(AV_LOG_DEBUG);

    if (argc < 2) {
        av_log(NULL, AV_LOG_ERROR, "the arguments must be more than 2!\n");
        exit(-1);
    }
    is->srcFilePath = argv[1];

    if ((ret = open_media(is)) < 0) {
        goto end;
    }

    if ((ret = sdl_init(is)) < 0 ) {
        goto end;
    }

    is->frameRate = is->vInStream->r_frame_rate.num / is->vInStream->r_frame_rate.den;

    pkt = av_packet_alloc();

    // SDL_CreateThread(decode_loop, "decode_loop_thread", is);
    pthread_t demux_thread, video_decode_thread, refresh_thread;
    pthread_create(&demux_thread, NULL, decode_loop, is);
    pthread_create(&video_decode_thread, NULL, video_thread, is);

    sdl_event_thread();

    pthread_join(demux_thread, NULL);
    pthread_join(video_decode_thread, NULL);

quit:
    ret = 0;
end:
    // if(is->vFrame){
    //     av_frame_free(&is->vFrame);
    // }
    if(is->aFrame){
        av_frame_free(&is->aFrame);
    }
    if (pkt){
        av_packet_free(&pkt);
    }
    if (is->aPkt){
        av_packet_free(&is->aPkt);
    }
    if (is->vPkt){
        av_packet_free(&is->vPkt);
    }
    if(is->aCtx){
        avcodec_free_context(&is->aCtx);
    }
    if(is->vCtx){
        avcodec_free_context(&is->vCtx);
    }
    if(is->fmtCtx){
        avformat_close_input(&is->fmtCtx);
    }
    if(screen){
        SDL_DestroyWindow(screen);
    }
    if(is->renderer){
        SDL_DestroyRenderer(is->renderer);
    }
    if(is->texture){
        SDL_DestroyTexture(is->texture);
    }
    if(is){
        av_free(is);
    }

    SDL_Quit();    
    return ret;
}