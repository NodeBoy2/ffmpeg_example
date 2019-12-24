 extern "C"
{
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
}

#include <iostream>
#include <string>
#include <memory>
#include <thread>

std::string strInput = "/Users/fengyifan/Desktop/videos/3001.flv";
std::string strOutFmt = "flv";
std::string strOutput = "/Users/fengyifan/Desktop/videos/out.flv";
bool isLive = false; // 设置为true，将模拟从实时流推送。

std::string strDecoderName = "h264"; // 优先使用的解码器名称，未找到使用默认的解码器

std::string strEncoderName = "libx264"; // 优先使用的编码器名称，未找到使用默认的编码器
enum AVCodecID encoderID = AV_CODEC_ID_H264; // 编码器类型
int bitrate = 1024 * 1024;   // 输出的视频比特率
int gopsize = 100;  // 输出的视频gop大小

std::shared_ptr<AVFormatContext> spInputFormat;
std::shared_ptr<AVFormatContext> spOutputFormat;

std::shared_ptr<AVCodecContext> spDecoderContext;
std::shared_ptr<AVCodecContext> spEncoderContext;
int videoIndex = -1; // 例程中只处理视频编码，记录视频流的流索引

std::string strInputMin = "/Users/fengyifan/Desktop/videos/3001.flv"; // 小画面输入源
std::shared_ptr<AVFormatContext> spInputMinFormat; // 小画面解复用器
std::shared_ptr<AVCodecContext> spDecoderMinContext; // 小画面解码器
int minVideoIndex = -1;

// encoder codec par
std::shared_ptr<AVCodecParameters> spOutCodecPar; // 编码器参数，有编码器传入到复用器中

AVFilterContext* pBuffersinkContext;
AVFilterContext* pBuffersrcContext;
std::shared_ptr<AVFilterGraph> spFilterGraph;
std::string strFiltersDescr = "scale2ref=oh*mdar:ih/5[min][main];[main][min]overlay=10:10";
AVFilterInOut* pOutputs = nullptr;
AVFilterInOut* pInputs = nullptr;

AVFilterContext* pBuffersrcMinContext; // 小画面过滤器输入
std::shared_ptr<AVFrame> gCurMinFrame; // 当前处理的小画面帧
bool gMinInputFinished = false; // 小画面文件是否读取完成

void msleep(int msec)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(msec));
}

bool createFilterGraph(std::string fileterDescr)
{
    spFilterGraph.reset(avfilter_graph_alloc(), [](AVFilterGraph* graph){
        avfilter_inout_free(&pInputs);
        avfilter_inout_free(&pOutputs);
        avfilter_graph_free(&graph);
    });

    if(avfilter_graph_parse_ptr(spFilterGraph.get(), fileterDescr.c_str(), &pInputs, &pOutputs, nullptr) < 0)
    {
        std::cout << "filter graph parse ptr error" << std::endl;
        return false;
    }
    return true;
}

AVFilterContext* createAndLinkInputFilter(AVCodecContext *pDecoder, AVRational time_base, AVFilterInOut* input, std::string name)
{
    AVFilterContext *pBufferContext = nullptr;
    // 创建 src filter，将数据传入此 filter
    const AVFilter *buffersrc = avfilter_get_by_name("buffer");
    char args[512];
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             pDecoder->width, pDecoder->height, pDecoder->pix_fmt,
             time_base.num, time_base.den,
             pDecoder->sample_aspect_ratio.num, pDecoder->sample_aspect_ratio.den);
    int ret = avfilter_graph_create_filter(&pBufferContext, buffersrc, name.c_str(), args, nullptr, spFilterGraph.get());
    if(ret < 0)
    {
        std::cout << "create input filter error" << std::endl;
        return nullptr;
    }
    // 连接 src filter 和 filter graph 的 input
    ret = avfilter_link(pBufferContext, 0, input->filter_ctx, input->pad_idx);
    if(ret < 0)
    {
        std::cout << "link input error" << std::endl;
        return nullptr;
    }
    return pBufferContext;
}

AVFilterContext* createAndLinkOutputFilter(AVFilterInOut* output, std::string name)
{
    AVFilterContext *pBufferContext = nullptr;
    // 创建 sink filter，从此 filter 接收数据
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };
    std::shared_ptr<AVBufferSinkParams> spBuffersinkParams(av_buffersink_params_alloc(),  [](AVBufferSinkParams* param){
        av_free(param);
    });
    spBuffersinkParams->pixel_fmts = pix_fmts;
    int ret = avfilter_graph_create_filter(&pBufferContext, buffersink, name.c_str(), nullptr, spBuffersinkParams.get(), spFilterGraph.get());
    if(ret < 0)
    {
        std::cout << "create output filter error" << std::endl;
        return nullptr;
    }
    // 连接 src filter 和 filter graph 的 output
    ret = avfilter_link(output->filter_ctx, output->pad_idx, pBufferContext, 0);
    if(ret < 0)
    {
        std::cout << "link out error" << std::endl;
        return nullptr;
    }
    return pBufferContext;
}

bool initDemuxer(std::shared_ptr<AVFormatContext> &spFormat, std::string input)
{
    // init demuxer
    AVFormatContext *pInputTemp = nullptr;
    if(avformat_open_input(&pInputTemp, input.c_str(), nullptr, nullptr) < 0)
    {
        std::cout << "open input format error" << std::endl;
        return false;
    }
    spFormat.reset(pInputTemp, [](AVFormatContext *fmt) {
        avformat_close_input(&fmt);
    });
    if(avformat_find_stream_info(spFormat.get(), 0) < 0)
    {
        std::cout << "find stream info error" << std::endl;
        return false;
    }
    return true;
}

bool initMuxer()
{
    // init muxer
    AVFormatContext *pOutputTemp = nullptr;
    if(avformat_alloc_output_context2(&pOutputTemp, nullptr, strOutFmt.c_str(), strOutput.c_str()) < 0)
    {
        std::cout << "alloc output format error" << std::endl;
        return false;
    }
    spOutputFormat.reset(pOutputTemp, [&](AVFormatContext *fmt){
        avformat_free_context(fmt);
    });

    // add stream
    for(unsigned int i = 0; i < spInputFormat->nb_streams; i++)
    {
        AVStream *inStream = spInputFormat->streams[i];
        AVStream *outStream = avformat_new_stream(spOutputFormat.get(), nullptr);
        if(inStream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            avcodec_parameters_copy(outStream->codecpar, spOutCodecPar.get()); // 使用编码器参数信息初始化
        else
            avcodec_parameters_copy(outStream->codecpar, inStream->codecpar); // 非视频流，使用输入流信息初始化
    }

    // write header
    if(!(spOutputFormat->oformat->flags & AVFMT_NOFILE))
        if(avio_open2(&spOutputFormat->pb, spOutputFormat->url, AVIO_FLAG_WRITE, nullptr, nullptr) < 0)
            return false;
    if(avformat_write_header(spOutputFormat.get(), nullptr) < 0)
    {
        std::cout << "write header error" << std::endl;
        return false;
    }
    return true;
}

bool initDecoder(std::shared_ptr<AVCodecContext> &spContext, AVStream *stream)
{
    AVCodec *pCodec = nullptr;
    pCodec = avcodec_find_decoder_by_name(strDecoderName.c_str());
    if(pCodec == nullptr || pCodec->id != stream->codecpar->codec_id)
        pCodec = avcodec_find_decoder(stream->codecpar->codec_id);
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
        spContext.reset(codecContext, [](AVCodecContext *pContext){
            avcodec_free_context(&pContext);
        });
        std::cout << "open decoder error" << std::endl;
        return  false;
    }
    spContext.reset(codecContext, [](AVCodecContext *pContext){
        avcodec_close(pContext);
        avcodec_free_context(&pContext);
    });

    return true;
}

bool initEncoder(AVStream *stream)
{
    spOutCodecPar.reset(avcodec_parameters_alloc(), [](AVCodecParameters* par){
        avcodec_parameters_free(&par);
    });
    avcodec_parameters_copy(spOutCodecPar.get(), stream->codecpar);

    AVCodec *pCodec = nullptr;
    pCodec = avcodec_find_encoder_by_name(strEncoderName.c_str());
    if(pCodec == nullptr)
        pCodec = avcodec_find_encoder(encoderID);
    if(pCodec == nullptr)
    {
        std::cout << "can't find encoder" << std::endl;
        return false;
    }

    spOutCodecPar->codec_id = encoderID;
    spOutCodecPar->bit_rate = bitrate;
    if(spOutCodecPar->extradata != nullptr)
    {
        av_free(spOutCodecPar->extradata);
        spOutCodecPar->extradata = nullptr;
        spOutCodecPar->extradata_size = 0;
    }

    AVCodecContext* codecContext = avcodec_alloc_context3(pCodec);
    avcodec_parameters_to_context(codecContext, spOutCodecPar.get());

    codecContext->time_base = stream->time_base;
    codecContext->gop_size = gopsize;
    codecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;


    if(avcodec_open2(codecContext, nullptr, nullptr) != 0)
    {
        // 打开解码器失败
        spEncoderContext.reset(codecContext, [](AVCodecContext *pContext){
            avcodec_free_context(&pContext);
        });
        std::cout << "open decoder error" << std::endl;
        return false;
    }
    spEncoderContext.reset(codecContext, [](AVCodecContext *pContext){
        avcodec_close(pContext);
        avcodec_free_context(&pContext);
    });

    // 重新设置输出的extradata（打开编码器时会更改一些数据）
    if(spOutCodecPar->extradata)
        av_free(spOutCodecPar->extradata);
    spOutCodecPar->extradata = (uint8_t*)av_mallocz(spEncoderContext->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(spOutCodecPar->extradata, spEncoderContext->extradata, spEncoderContext->extradata_size);
    spOutCodecPar->extradata_size = spEncoderContext->extradata_size;

    return true;
}



bool writePacketToMuxer(std::shared_ptr<AVPacket> &spPacket)
{
    if(av_write_frame(spOutputFormat.get(), spPacket.get()) < 0)
    {
        std::cout << "mux error" << std::endl;
        return false;
    }
    return true;
}

bool encodeAndMuxFrame(std::shared_ptr<AVFrame> &spFrame)
{
    int ret = avcodec_send_frame(spEncoderContext.get(), spFrame.get());
    if(ret == AVERROR(EINVAL) || ret == AVERROR(ENOMEM))
    {
        std::cout << "send frame to encoder error" << std::endl;
        return false;
    }
    if(ret == AVERROR_EOF)
        return true;

    std::shared_ptr<AVPacket> spPacket(av_packet_alloc(), [](AVPacket *packet){
        av_packet_free(&packet);
    });
    while(ret >= 0)
    {
        ret = avcodec_receive_packet(spEncoderContext.get(), spPacket.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return true;
        else if (ret < 0)
        {
           std::cout << "encode frame error" << std::endl;
           return false;
        }
        if(!writePacketToMuxer(spPacket))
            return false;
    }
    return true;
}

void nextMinFrame()
{
    if(gMinInputFinished)
    {
        return;
    }
    while(true)
    {
        std::shared_ptr<AVPacket> spInPakcet(av_packet_alloc(), [](AVPacket* pPacket){
            av_packet_free(&pPacket);
        });
        int ret = av_read_frame(spInputMinFormat.get(), spInPakcet.get());
        if(ret == AVERROR_EOF && avio_feof(spInputMinFormat->pb))
        {
            gMinInputFinished = true;
            break;
        }
        else if(ret < 0)
        {
            std::cout << "demux error" << std::endl;
            continue;
        }
        if(spInPakcet->stream_index != minVideoIndex)
            continue;
        ret = avcodec_send_packet(spDecoderMinContext.get(), spInPakcet.get());
        if(ret < 0)
        {
            std::cout << "send packet to decoder error" << std::endl;
            continue;
        }
        gCurMinFrame.reset(av_frame_alloc(), [](AVFrame *frame){
            av_frame_free(&frame);
        });
        ret = avcodec_receive_frame(spDecoderMinContext.get(), gCurMinFrame.get());
        if (ret < 0)
            continue;
        break;
    }
    return;
}

bool filterAndEncodeFrame(std::shared_ptr<AVFrame> &spFrame)
{
    int64_t mainFramePts = spFrame->pts; // 传入过滤器之后，frame数据失效，记录pts之后使用
    int ret = av_buffersrc_add_frame_flags(pBuffersrcContext, spFrame.get(), AV_BUFFERSRC_FLAG_PUSH);
    if(ret < 0)
    {
        std::cout << "add frame to buffersrc error" << std::endl;
        return false;
    }
    if(gCurMinFrame == nullptr)
        nextMinFrame();
    if(gCurMinFrame != nullptr)
    {
        // 将pts从min的时间基转换到主画面时间基，之后直接赋值pts给传入过滤器的frame
        int64_t minFramePts = av_rescale_q_rnd(gCurMinFrame->pts, spInputMinFormat->streams[minVideoIndex]->time_base,
                                          spInputFormat->streams[videoIndex]->time_base, (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
        if(minFramePts < mainFramePts)  // 简单-根据pts先后顺序处理叠加帧
            nextMinFrame();

        if(gCurMinFrame != nullptr)
        {
            // 传入过滤器之后，frame数据失效。当前帧可能需要多次使用，将副本传入过滤器
            std::shared_ptr<AVFrame> minFrame(av_frame_clone(gCurMinFrame.get()), [](AVFrame *frame){
                av_frame_unref(frame);
                av_frame_free(&frame);
            });
            minFrame->pts = mainFramePts;
            ret = av_buffersrc_add_frame_flags(pBuffersrcMinContext, minFrame.get(), AV_BUFFERSRC_FLAG_PUSH);
        }
        if(ret < 0)
        {
            std::cout << "add frame to bufferminsrc error" << std::endl;
            return false;
        }
    }

    while(ret >= 0)
    {
        std::shared_ptr<AVFrame> spOutFrame(av_frame_alloc(), [](AVFrame *frame){
            av_frame_free(&frame);
        });
        ret = av_buffersink_get_frame_flags(pBuffersinkContext, spOutFrame.get(), AV_BUFFERSINK_FLAG_NO_REQUEST);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0)
            return false;
        if(!encodeAndMuxFrame(spOutFrame))
            return false;
    }
    return true;
}

bool decodeAndEncodePacket(std::shared_ptr<AVPacket> &spPacket)
{
    int ret = avcodec_send_packet(spDecoderContext.get(), spPacket.get());
    if(ret == AVERROR(EINVAL) || ret == AVERROR(ENOMEM))
    {
        std::cout << "send packet to decoder error" << std::endl;
        return false;
    }
    if(ret == AVERROR_EOF)
        return true;
    std::shared_ptr<AVFrame> spFrame(av_frame_alloc(), [](AVFrame *frame){
        av_frame_free(&frame);
    });
    while(ret >= 0)
    {
        ret = avcodec_receive_frame(spDecoderContext.get(), spFrame.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return true;
        else if (ret < 0)
        {
           std::cout << "decode packet error" << std::endl;
           return false;
        }
        if(!filterAndEncodeFrame(spFrame))
            return false;
    }
    return true;
}

bool initMainVideo()
{
    // init main demuxer
    if(!initDemuxer(spInputFormat, strInput))
        return false;
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
        return false;
    }

    // init main decoder
    if(!initDecoder(spDecoderContext, spInputFormat->streams[videoIndex]))
    {
        std::cout << "open main decoder error" << std::endl;
        return false;
    }

    if(!initEncoder(spInputFormat->streams[videoIndex]))
    {
        std::cout << "open encoder error" << std::endl;
        return false;
    }

    if(!initMuxer())
        return false;
    return true;

}

bool initMinVideo()
{
    // init min demuxer
    if(!initDemuxer(spInputMinFormat, strInputMin))
        return false;

    // find min video stream index
    for(unsigned int i = 0; i < spInputMinFormat->nb_streams; i++)
    {
        AVStream *inStream = spInputMinFormat->streams[i];
        if(inStream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            minVideoIndex = i;
            break;
        }
    }

    if (minVideoIndex == -1)
    {
        std::cout << "can't find min video stream" << std::endl;
        return false;
    }

    // init min decoder
    if(!initDecoder(spDecoderMinContext, spInputMinFormat->streams[minVideoIndex]))
    {
        std::cout << "open min decoder error" << std::endl;
        return false;
    }
    return true;
}

int main()
{
    if(!initMainVideo())
        return -1;
    if(!initMinVideo())
        return -1;

    if(!createFilterGraph(strFiltersDescr))
        return -1;
    // 将主画面放入 scaleref 第二个输出（input->next），第一个输入会缩放
    pBuffersrcContext = createAndLinkInputFilter(spDecoderContext.get(), spInputFormat->streams[videoIndex]->time_base, pInputs->next, "main");
    if(pBuffersrcContext == nullptr)
        return -1;
    // 此处小画面输入过滤器时间基设置为主视频时间处理，之后过滤处理时，将frame的pts统一到主画面时间基
    pBuffersrcMinContext = createAndLinkInputFilter(spDecoderMinContext.get(), spInputFormat->streams[videoIndex]->time_base, pInputs, "min");
    if(pBuffersrcMinContext == nullptr)
        return -1;
    pBuffersinkContext = createAndLinkOutputFilter(pOutputs, "Output");
    if(pBuffersinkContext == nullptr)
        return -1;
    if(avfilter_graph_config(spFilterGraph.get(), nullptr) < 0)
    {
        std::cout << "filter graph confit error" << std::endl;
        return -1;
    }

    int64_t lastDts = 0;

    // process
    for (;;)
    {
        std::shared_ptr<AVPacket> spInPakcet(av_packet_alloc(), [&](AVPacket* pPacket){
            av_packet_free(&pPacket);
        });
        int ret = av_read_frame(spInputFormat.get(), spInPakcet.get());
        if(ret == AVERROR_EOF && avio_feof(spInputFormat->pb))
        {
            std::cout << "demux over" << std::endl;
            break;
        }
        else if(ret < 0)
        {
            std::cout << "demux error" << std::endl;
            break;
        }

        // 不是视频数据，直接复用
        if(spInPakcet->stream_index != videoIndex)
            writePacketToMuxer(spInPakcet);
        else
        {
            // 如果是直播流(推送文件模拟直播流)，等待到时
            if(isLive && spInPakcet->dts != AV_NOPTS_VALUE)
            {
                int64_t dts = spInPakcet->dts * av_q2d(spInputFormat->streams[videoIndex]->time_base) * 1000;
                if(lastDts != 0 && dts - lastDts > 0)
                    msleep(dts-lastDts);
                lastDts = dts;
            }
            if(!decodeAndEncodePacket(spInPakcet))
                break;
        }
    }

    // 清空解码器
    std::shared_ptr<AVPacket> spFlushPacket;
    decodeAndEncodePacket(spFlushPacket);

    // 清空编码器
    std::shared_ptr<AVFrame> spFlushFrame;
    encodeAndMuxFrame(spFlushFrame);

    // write trailer
    av_write_trailer(spOutputFormat.get());
    if(!(spOutputFormat->oformat->flags & AVFMT_NOFILE)) {
        avio_close(spOutputFormat->pb);
    }
    std::cout << "remux over" << std::endl;
    return 0;
}
