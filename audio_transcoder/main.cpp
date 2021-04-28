 extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
 #include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <iostream>
#include <string>
#include <memory>
#include <thread>

std::string strInput = "/Users/fengyifan/Desktop/videos/test_h264_acc.flv";
std::string strOutFmt = "flv";
//std::string strOutput = "/Users/fengyifan/Desktop/videos/out.flv";
std::string strOutput = "rtmp://127.0.0.1/live/testlive";
bool isLive = true; // 设置为true，将模拟从实时流推送。

std::string strDecoderName = "aac"; // 优先使用的解码器名称，未找到使用默认的解码器

std::string strEncoderName = "libfdk-aac"; // 优先使用的编码器名称，未找到使用默认的编码器
enum AVCodecID encoderID = AV_CODEC_ID_AAC; // 编码器类型
int bitrate = 1024 * 100;   // 输出的视频比特率
int channels = 2;
int sampleRate = 48000;
AVSampleFormat sampleFmt = AV_SAMPLE_FMT_FLTP;

std::shared_ptr<AVFormatContext> spInputFormat;
std::shared_ptr<AVFormatContext> spOutputFormat;

std::shared_ptr<AVCodecContext> spDecoderContext;
std::shared_ptr<AVCodecContext> spEncoderContext;
int audioIndex = -1; // 例程中只处理视频编码，记录视频流的流索引

std::shared_ptr<SwrContext> spSwrContext;
std::shared_ptr<AVAudioFifo> spFifo;
// encoder codec par
std::shared_ptr<AVCodecParameters> spOutCodecPar; // 编码器参数，有编码器传入到复用器中


// resample value
std::shared_ptr<uint8_t*> spData;
uint8_t **origindata;
int dst_linesize;
int max_dst_nb_samples, dst_nb_samples;

void msleep(int msec)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(msec));
}


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
    for(int i = 0; i < spInputFormat->nb_streams; i++)
    {
        AVStream *inStream = spInputFormat->streams[i];
        AVStream *outStream = avformat_new_stream(spOutputFormat.get(), nullptr);
        if(i == audioIndex)
            avcodec_parameters_from_context(outStream->codecpar, spEncoderContext.get()); // 使用编码器参数信息初始化
        else
            avcodec_parameters_copy(outStream->codecpar, inStream->codecpar); // 视频流，使用输入流信息初始化
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

bool initDecoder(AVStream *stream)
{
    AVCodec *pCodec = nullptr;
    pCodec = avcodec_find_decoder_by_name(strDecoderName.c_str());
    if(pCodec == nullptr)
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
    spOutCodecPar->level = -1;
    if(spOutCodecPar->extradata != nullptr)
    {
        av_free(spOutCodecPar->extradata);
        spOutCodecPar->extradata = nullptr;
        spOutCodecPar->extradata_size = 0;
    }

    AVCodecContext* codecContext = avcodec_alloc_context3(pCodec);
    avcodec_parameters_to_context(codecContext, spOutCodecPar.get());

    codecContext->time_base = stream->time_base;
    codecContext->channels = channels;
    codecContext->sample_rate = sampleRate;
    codecContext->channel_layout = av_get_default_channel_layout(channels);
    codecContext->sample_fmt = sampleFmt;
    codecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;


    if(avcodec_open2(codecContext, nullptr, nullptr) != 0)
    {
        // 打开解码器失败
        spEncoderContext.reset(codecContext, [](AVCodecContext *pContext){
            avcodec_free_context(&pContext);
        });
        std::cout << "open encoder error" << std::endl;
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

bool initResampler(AVStream *audioStream) {
//    auto swrCtx = swr_alloc_set_opts(nullptr,
//                                        av_get_default_channel_layout(channels), (enum AVSampleFormat)sampleFmt, sampleRate,
//                                        av_get_default_channel_layout(audioStream->codecpar->channels), (enum AVSampleFormat)audioStream->codecpar->format, audioStream->codecpar->sample_rate,
//                                        0, nullptr);
//    if (swrCtx == nullptr)
//        return false;

    auto swrCtx = swr_alloc();
    swrCtx = swr_alloc();
    if (!swrCtx) {
        return false;
    }

    /* set options */
    av_opt_set_sample_fmt(swrCtx, "in_sample_fmt",       (enum AVSampleFormat)audioStream->codecpar->format,       0);
    av_opt_set_int       (swrCtx, "in_sample_rate",     audioStream->codecpar->sample_rate,         0);
    av_opt_set_int       (swrCtx, "in_channel_count",   audioStream->codecpar->channels,           0);
    av_opt_set_sample_fmt(swrCtx, "out_sample_fmt",     sampleFmt,    0);
    av_opt_set_int       (swrCtx, "out_sample_rate",    sampleRate,      0);
    av_opt_set_int       (swrCtx, "out_channel_count",  channels,        0);

    spSwrContext.reset(swrCtx, [](SwrContext *context){
        swr_free(&context);
    });

    swr_init(spSwrContext.get());

    auto fifo = av_audio_fifo_alloc((AVSampleFormat)sampleFmt, channels, 1);
    if(fifo == nullptr)
        return false;
    spFifo.reset(fifo, [](AVAudioFifo * fifo){
        av_audio_fifo_free(fifo);
    });

    return true;
}

bool writePacketToMuxer(std::shared_ptr<AVPacket> &spPacket)
{
    if(av_interleaved_write_frame(spOutputFormat.get(), spPacket.get()) < 0)
    {
        std::cout << "mux error" << std::endl;
        return false;
    }
    return true;
}

bool cacheFrame(std::shared_ptr<AVFrame> &spFrame) {
    if (spFrame == nullptr) {
        return true;
    }
    // resample audio
    if(spData == nullptr)
    {
        max_dst_nb_samples = dst_nb_samples =
            av_rescale_rnd(spFrame->nb_samples, sampleRate, spFrame->sample_rate, AV_ROUND_UP);
        int ret = av_samples_alloc_array_and_samples(&origindata, &dst_linesize, channels,
                                                dst_nb_samples, sampleFmt, 0);
        if (ret < 0)
        {
            return false;
        }
        spData.reset(origindata, [](uint8_t**data){
            av_freep(&(data)[0]);
        });
    }
    dst_nb_samples = av_rescale_rnd(swr_get_delay(spSwrContext.get(), spFrame->sample_rate) +
                                    spFrame->nb_samples, sampleRate, spFrame->sample_rate, AV_ROUND_UP);

    if(dst_nb_samples > max_dst_nb_samples) {
        int ret = av_samples_alloc_array_and_samples(&origindata, &dst_linesize, channels,
                                                dst_nb_samples, sampleFmt, 1);
        if (ret < 0)
        {
            return false;
        }
        spData.reset(origindata, [](uint8_t**data){
            av_freep(&(data)[0]);
        });
        max_dst_nb_samples = dst_nb_samples;
    }

    int real_nb_samples = swr_convert(spSwrContext.get(), origindata, dst_nb_samples, (const uint8_t**)spFrame->extended_data, spFrame->nb_samples);

    if(real_nb_samples < 0)
    {
        std::cout << "resampl audio error" << std::endl;
        return false;
    }

    if(av_audio_fifo_write(spFifo.get(), (void**)origindata, real_nb_samples) < real_nb_samples)
    {
        std::cout << "write to fifo error" << std::endl;
        return false;
    }
    return true;
}

std::shared_ptr<AVFrame> getFrame(std::shared_ptr<AVFrame> &spFrame) {
    if (spFrame == nullptr) {
        return nullptr;
    }
    if(av_audio_fifo_size(spFifo.get()) < spEncoderContext->frame_size) {
        return nullptr;
    }



    std::shared_ptr<AVFrame> outframe(av_frame_alloc(), [](AVFrame *frame){
        av_frame_free(&frame);
    });

    outframe->nb_samples = spEncoderContext->frame_size;
    outframe->channels = channels;
    outframe->sample_rate = sampleRate;
    outframe->format = sampleFmt;
    outframe->channel_layout = av_get_default_channel_layout(channels);
    av_frame_get_buffer(outframe.get(), 0);

    av_audio_fifo_read(spFifo.get(), (void**)outframe->data, spEncoderContext->frame_size);

    static double currentpts = 0.0;
    double duration = (double)outframe->nb_samples * 1000 / sampleRate;

    outframe->pts = currentpts;
    outframe->pkt_dts = currentpts;
    outframe->pkt_dts = currentpts;
    currentpts += duration;

    return outframe;
}

bool encodeAndMuxFrame(std::shared_ptr<AVFrame> &spFrame)
{
    // resample and wait
    if(!cacheFrame(spFrame)) {
        std::cout << "cache frame error" << std::endl;;
        return false;
    }
    auto frame = getFrame(spFrame);
    while(frame != nullptr || spFrame == nullptr) {
        int ret = avcodec_send_frame(spEncoderContext.get(), frame.get());
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

            AVRational framerate = spEncoderContext->framerate;
            AVRational ffmpegTimebase = spEncoderContext->time_base;
            if(spPacket->duration <= 0 && framerate.num != 0 && framerate.den != 0)
            {
                int64_t nCalcDuration = (double)AV_TIME_BASE / (framerate.num/framerate.den);

                int64_t duration = nCalcDuration / (double)(av_q2d(ffmpegTimebase)*AV_TIME_BASE);
                spPacket->duration = duration;
            }

             // 根据流逝时间计算pts
//            static auto startTime = std::chrono::system_clock::now();
//            auto now = std::chrono::system_clock::now();
//            uint64_t dis_millseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()
//                    - std::chrono::duration_cast<std::chrono::seconds>(startTime.time_since_epoch()).count() * 1000;
//            spPacket->pts = dis_millseconds / av_q2d(spInputFormat->streams[audioIndex]->time_base) / 1000;
//            spPacket->dts = spPacket->pts;

            // 根据每包音频时长累积计算pts
//            double duration = (double)frame->nb_samples * 1000 / sampleRate;
//            std::cout << duration << std::endl;

//            spPacket->duration = duration / av_q2d(spInputFormat->streams[audioIndex]->time_base) / 1000;
//            static int64_t lastPts = 0;
//            spPacket->pts = lastPts + spPacket->duration;
//            spPacket->dts = spPacket->pts;
//            lastPts = spPacket->pts;


            int64_t dts = spPacket->pts * av_q2d(spInputFormat->streams[audioIndex]->time_base) * 1000;
            std::cout << "audio pts: " << dts << std::endl;
            spPacket->stream_index = audioIndex;
            if(!writePacketToMuxer(spPacket))
                return false;
        }

        frame = getFrame(spFrame);
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
        if(!encodeAndMuxFrame(spFrame))
            return false;
    }
    return true;
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
        if(inStream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            audioIndex = i;
            break;
        }
    }

    if (audioIndex == -1)
    {
        std::cout << "can't find video stream" << std::endl;
        return -1;
    }

    // init decoder
    if(!initDecoder(spInputFormat->streams[audioIndex]))
    {
        std::cout << "open decoder error" << std::endl;
        return -1;
    }

    if(!initEncoder(spInputFormat->streams[audioIndex]))
    {
        std::cout << "open encoder error" << std::endl;
        return -1;
    }

    if(!initMuxer())
        return -1;

    if(!initResampler(spInputFormat->streams[audioIndex]))
    {
        std::cout << "init resampler error" << std::endl;
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
        if(spInPakcet->stream_index != audioIndex)
            writePacketToMuxer(spInPakcet);
        else
        {
            // 如果是直播流(推送文件模拟直播流)，等待到时
            if(isLive && spInPakcet->dts != AV_NOPTS_VALUE)
            {
                int64_t dts = spInPakcet->dts * av_q2d(spInputFormat->streams[audioIndex]->time_base) * 1000;
                static auto firstFrame = std::chrono::system_clock::now();
                auto now = std::chrono::system_clock::now();
                uint64_t dis_millseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()
                        - std::chrono::duration_cast<std::chrono::seconds>(firstFrame.time_since_epoch()).count() * 1000;
                int64_t waittime = dts - dis_millseconds;
                // 每100ms发送一次数据
//                if (waittime > 100)
//                {
//                    waittime -= 5;
//                    msleep(waittime - 30);
//                }

                // 根据流逝时间对比pts发送数据
                msleep(waittime);

                // 根据pts间隔发送数据
//                std::cout << dts - lastDts << std::endl;
//                if(lastDts != 0 && dts - lastDts > 0)
//                    msleep(dts-lastDts);
//                lastDts = dts;
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

