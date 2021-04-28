/**
 *  基于FFmpeg的视频播放器
 *  使用到了FFmpeg的解复用，解码和图像转换的接口，以及SDL2.0图像显示相关的接口
 */
extern "C"
{
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <SDL2/SDL.h>
}

#include <iostream>
#include <string>
#include <memory>

std::string strInput = "http://test-tx-flv.meituan.net/mttest/testdelay_264LD.flv";
std::shared_ptr<AVFormatContext> spInputFormat;
std::shared_ptr<AVCodecContext> spDecoderContext;
std::shared_ptr<SwsContext> spImageConvert;         // 图像转换器上下文，解码除的图像可能不是YUV420P类型，需要进行转换（也可能是YUV420P，但是可能不是在连续内存存储）
std::shared_ptr<AVFrame> spFrameYUV;
int videoIndex = -1; // 例程中只处理视频编码，记录视频流的流索引

// 控制刷新线程
bool isFinished = false;
bool isPaused = false;

// 自定义刷新事件
#define REFRESH_EVENT  (SDL_USEREVENT + 1)
#define BREAK_EVENT  (SDL_USEREVENT + 2)

bool initDemuxer()
{
    // init demuxer
    AVFormatContext *pInputTemp = nullptr;
    if(avformat_open_input(&pInputTemp, strInput.c_str(), nullptr, nullptr) < 0)
    {
        std::cout << "open input format error" << std::endl;
        return false;
    }
    spInputFormat.reset(pInputTemp, [](AVFormatContext *fmt) {
        avformat_close_input(&fmt);
    });
    if(avformat_find_stream_info(spInputFormat.get(), 0) < 0)
    {
        std::cout << "find stream info error" << std::endl;
        return false;
    }
    return true;
}

bool initDecoder(AVStream *stream)
{
    AVCodec *pCodec = avcodec_find_decoder(stream->codecpar->codec_id);
    if(pCodec == nullptr)
    {
        std::cout << "can't find decoder" << std::endl;
        return false;
    }

    AVCodecContext* codecContext = avcodec_alloc_context3(pCodec);
    avcodec_parameters_to_context(codecContext, stream->codecpar);
    codecContext->time_base = stream->time_base;

    if(avcodec_open2(codecContext, nullptr, nullptr) != 0)
    {
        // 打开解码器失败
        spDecoderContext.reset(codecContext, [](AVCodecContext *pContext){
            avcodec_free_context(&pContext);
        });
        std::cout << "open decoder error" << std::endl;
        return  false;
    }
    spDecoderContext.reset(codecContext, [](AVCodecContext *pContext){
        avcodec_close(pContext);
        avcodec_free_context(&pContext);
    });

    return true;
}

bool inieSwscontext()
{
    AVFrame *tmpFrame = av_frame_alloc();
    unsigned char* outBuffer = (unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, spDecoderContext->width, spDecoderContext->height, 1));
    av_image_fill_arrays(tmpFrame->data, tmpFrame->linesize, outBuffer, AV_PIX_FMT_YUV420P, spDecoderContext->width, spDecoderContext->height, 1);
    spFrameYUV.reset(tmpFrame, [](AVFrame *frame){
        if(frame->data[0])
            av_free(frame->data[0]); // 需要将创建的image_buffer释放，av_frame_free 不会释放自己填入frame中的buffer
        av_frame_free(&frame);
    });
    SwsContext* tmpImageConvert = sws_getContext(spDecoderContext->width, spDecoderContext->height, spDecoderContext->pix_fmt,
                                                spDecoderContext->width, spDecoderContext->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
    spImageConvert.reset(tmpImageConvert, [](SwsContext *imageConvert){
        sws_freeContext(imageConvert);
    });
    return true;
}

std::shared_ptr<AVFrame> decodePacket(std::shared_ptr<AVPacket> &spPacket)
{
    int ret = avcodec_send_packet(spDecoderContext.get(), spPacket.get());
    if(ret == AVERROR(EINVAL) || ret == AVERROR(ENOMEM))
    {
        std::cout << "send packet to decoder error" << std::endl;
        return nullptr;
    }
    if(ret == AVERROR_EOF)
        return nullptr;
    std::shared_ptr<AVFrame> spFrame(av_frame_alloc(), [](AVFrame *frame){
        av_frame_free(&frame);
    });
    while(ret >= 0)
    {
        ret = avcodec_receive_frame(spDecoderContext.get(), spFrame.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return nullptr;
        else if (ret < 0)
        {
           std::cout << "decode packet error" << std::endl;
           return nullptr;
        }
        // process
        return std::move(spFrame);
    }
    return nullptr;
}

int refreshThread(void *)
{
    isPaused = false;
    isFinished = false;
    while(!isFinished)
    {
        if(!isPaused)
        {
            SDL_Event event;
            event.type = REFRESH_EVENT;
            SDL_PushEvent(&event);
        }
        SDL_Delay(10);
    }
    //Break
    SDL_Event event;
    event.type = BREAK_EVENT;
    SDL_PushEvent(&event);
    return 0;
}

int main()
{
    // init demuxer
    if(!initDemuxer())
        return -1;

    // find video stream index
    for(unsigned int i = 0; i < spInputFormat->nb_streams; i++)
    {
        AVStream *inStream = spInputFormat->streams[i];
        if(inStream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            videoIndex = i;
            break;
        }
    }

    if (videoIndex == -1)
    {
        std::cout << "can't find video stream" << std::endl;
        return -1;
    }

    av_dump_format(spInputFormat.get(), 0, strInput.c_str(), 0);

    // init decoder
    if(!initDecoder(spInputFormat->streams[videoIndex]))
    {
        std::cout << "open decoder error" << std::endl;
        return -1;
    }
    if(!inieSwscontext())
    {
        std::cout << "init image convert error" << std::endl;
        return -1;
    }

    // SDL初始化
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER)) {
        printf( "Could not initialize SDL - %s\n", SDL_GetError());
        return -1;
    }

    SDL_Rect sdlRect;
    sdlRect.x = 0;
    sdlRect.y = 0;
    int width = 0;
    int height = 0;
    {
        float aspect_ratio;
        int x, y;
        auto pic_sar = spInputFormat->streams[videoIndex]->sample_aspect_ratio;
        if (pic_sar.num == 0)
            aspect_ratio = 0;
        else
            aspect_ratio = av_q2d(pic_sar);

        if (aspect_ratio <= 0.0)
            aspect_ratio = 1.0;
        aspect_ratio *= (float)spDecoderContext->width / (float)spDecoderContext->height;

        /* XXX: we suppose the screen has a 1.0 pixel ratio */
        height = spDecoderContext->height;
        width = lrint(height * aspect_ratio) & ~1;
        if (width > spDecoderContext->width) {
            width = spDecoderContext->width;
            height = lrint(width / aspect_ratio) & ~1;
        }
    }

    sdlRect.w = width;
    sdlRect.h = height;

    // 创建SDL窗体
    SDL_Window *screen = SDL_CreateWindow("Simple player's Window", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                          sdlRect.w, sdlRect.h, SDL_WINDOW_OPENGL);
    if(screen == nullptr)
    {
        std::cout << "could not create window" << std::endl;
        return -1;
    }
    // 为窗体创建渲染的上下文
    SDL_Renderer* sdlRenderer = SDL_CreateRenderer(screen, -1, 0);
    // 创建图像使用的纹理
    // IYUV === YUV420P
    SDL_Texture* sdlTexture = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, spDecoderContext->width, spDecoderContext->height);

    SDL_CreateThread(refreshThread, nullptr, nullptr);

    // process
    SDL_Event event;
    for (;;)
    {
        SDL_WaitEvent(&event);
        if(event.type == REFRESH_EVENT)
        {
            // get packet
            std::shared_ptr<AVPacket> spInPakcet(av_packet_alloc(), [&](AVPacket* pPacket){
                av_packet_free(&pPacket);
            });
            while(true)
            {
                int ret = av_read_frame(spInputFormat.get(), spInPakcet.get());
                if(ret < 0)
                {
                    std::cout << "demux over" << std::endl;
                    isFinished = true;
                    break;
                }
                if(spInPakcet->stream_index == videoIndex)
                    break;
            }
//            if(spInPakcet->flags != AV_PKT_FLAG_KEY)
//                continue;
            // decode
            auto spFrame = decodePacket(spInPakcet);
            if(spFrame == nullptr)
                continue;
//            if(spFrame->flags & AV_PICTURE_TYPE_I)
//                continue;
//            sws_scale(spImageConvert.get(), (const unsigned char* const*)spFrame->data, spFrame->linesize, 0, spDecoderContext->height, spFrameYUV->data, spFrameYUV->linesize);
            SDL_UpdateYUVTexture(sdlTexture, NULL, spFrame->data[0], spFrame->linesize[0],
                                                                   spFrame->data[1], spFrame->linesize[1],
                                                                   spFrame->data[2], spFrame->linesize[2]);
//            SDL_UpdateTexture(sdlTexture, NULL, spFrameYUV->data[0], spFrameYUV->linesize[0]);
            SDL_RenderClear(sdlRenderer );
            SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, NULL);
            SDL_RenderPresent(sdlRenderer);

        }
        else if(event.type == SDL_KEYDOWN)
        {
            // pause
            if(event.key.keysym.sym == SDLK_SPACE)
                isPaused = !isPaused;
            else if(event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q)
                isFinished = true;
        }
        else if(event.type == SDL_QUIT)
            isFinished = true;
        else if(event.type == BREAK_EVENT)
             break;
    }
    return 0;
}
