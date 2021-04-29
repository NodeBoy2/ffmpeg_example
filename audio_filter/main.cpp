extern "C"
{
#include <libavformat/avformat.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/avfilter.h>
#include <libavutil/opt.h>
}

#include <iostream>
#include <string>
#include <memory>
#include <thread>
#include <list>
#include <mutex>
#include <condition_variable>

std::string strInput = "rtmp://127.0.0.1/live/testlive1";
std::string strOutFmt = "flv";
//std::string strOutput = "/Users/fengyifan/Desktop/videos/out.flv";
std::string strOutput = "rtmp://127.0.0.1/live/testlive";
bool isLive = false; // 设置为true，将模拟从实时流推送。

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

// encoder codec par
std::shared_ptr<AVCodecParameters> spOutCodecPar; // 编码器参数，有编码器传入到复用器中

AVFilterContext* pSinkFilterContext;
AVFilterContext* pSrcFilterContext;
std::shared_ptr<AVFilterGraph> spFilterGraph;
std::string strFiltersDescr = "sample_fmts=%s:sample_rates=%d:channel_layouts=0x%";

std::list<std::shared_ptr<AVPacket>> spPakcetList;
std::mutex packetMuxtex;



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

void createFilterGraph()
{
   spFilterGraph.reset(avfilter_graph_alloc(), [](AVFilterGraph* graph){
       avfilter_graph_free(&graph);
   });
}

bool initAudioFilter(AVStream *audioStream) {
   // 创建 src filter，将数据传入此 filter
   const AVFilter *buffersrc = avfilter_get_by_name("abuffer");
   pSrcFilterContext = avfilter_graph_alloc_filter(spFilterGraph.get(), buffersrc, "src_buffer");
   if(!pSrcFilterContext) {
       return false;
   }

   /** 设置输入滤镜的相关参数(采样率，采样格式，时间基，声道类型，声道数)并初始化,这里的参数要跟实际输入的音频数据参数保持一致;这是第一种初始化滤镜参数的方式
        *  av_opt_setxxx()系列函数最后一个参数的含义：
        *  0：只在对象的AVClass的option属性里面查找是否有对应的参数赋值，如果没有那么设置将无效；
        *  AV_OPT_SEARCH_CHILDREN：先在对象的AVClass的child_next指向的AVClass的option属性查找是否有对应的参数，然后在对象的AVClass的option属性查找对应参数
        *  AV_OPT_SEARCH_FAKE_OBJ：先在对象的AVClass的child_class_next指向的AVClass的option属性查找是否有对应的参数，然后在对象的AVClass的option属性查找对应参数
        *  遇到问题：avfilter_init_str()函数返回失败
        *  分析原因：调用av_opt_set_xx()系列函数给src_flt_ctx设置参数时无效，因为之前最后一个参数传的为0，AVFilterContext的option属性是不含有这些参数的(它的option属性
        *  的child_next指向的AVClass的option属性才有这些参数)，所以最后一个参数应该为AV_OPT_SEARCH_CHILDREN
        *  解决方案：将最后一个参数设置为AV_OPT_SEARCH_CHILDREN
   */
   // 在libavfilter/buffersrc.c文件中可以找到定义
   char ch_layout[64];
   av_get_channel_layout_string(ch_layout, sizeof(ch_layout), audioStream->codecpar->channels, audioStream->codecpar->channel_layout);
   av_opt_set(pSrcFilterContext, "channel_layout", ch_layout, AV_OPT_SEARCH_CHILDREN);
   av_opt_set_sample_fmt(pSrcFilterContext, "sample_fmt", (enum AVSampleFormat)audioStream->codecpar->format, AV_OPT_SEARCH_CHILDREN);
   av_opt_set_q(pSrcFilterContext, "time_base", audioStream->time_base, AV_OPT_SEARCH_CHILDREN);
   av_opt_set_int(pSrcFilterContext, "sample_rate", audioStream->codecpar->sample_rate, AV_OPT_SEARCH_CHILDREN);
   av_opt_set_int(pSrcFilterContext, "channels", audioStream->codecpar->channels, AV_OPT_SEARCH_CHILDREN);
   // 因为前面已经通过av_opt_set()函数给src_flt_ctx设置了对应参数的值，所以这里初始化的时候不需要再传递任何参数了
   if (avfilter_init_dict(pSrcFilterContext, NULL) < 0) {    // 这里换成avfilter_init_str()函数也是可以的
       return false;
   }


   // 格式转换滤镜
   const AVFilter *aformat = avfilter_get_by_name("aformat");
   if (!aformat) {
       return false;
   }
   AVFilterContext *formatFilterContext = avfilter_graph_alloc_filter(spFilterGraph.get(), aformat,"format");
   if (!formatFilterContext) {
       return false;
   }
   // 设置格式转换滤镜参数并初始化;具体参数的名字见libavfilter/af_aformat.c文件的aformat_options变量
   // 这是第三种设置滤镜参数的方式，采用key1=value1:key2=value2....的字符串形式组织各个参数的值，avfilter_init_str()函数内部会自己解析
   char format_opts[128];
   sprintf(format_opts, "sample_fmts=%s:sample_rates=%d:channel_layouts=0x%" PRIx64,av_get_sample_fmt_name(sampleFmt), sampleRate, av_get_default_channel_layout(channels));
   printf("val = %s \n",format_opts);
   if (avfilter_init_str(formatFilterContext, format_opts) < 0) {
       return false;
   }


   // 输出滤镜
   const AVFilter *sink = avfilter_get_by_name("abuffersink");
   if (!sink) {
       return false;
   }

   // 创建输出滤镜的上下文
   pSinkFilterContext = avfilter_graph_alloc_filter(spFilterGraph.get(), sink, "sink");
   if (!pSinkFilterContext) {
       return false;
   }

   // 初始化输出滤镜;由于输出滤镜是接受最后一个滤镜的数据，只是做一个中转，所以不需要设置任何参数
   if (avfilter_init_str(pSinkFilterContext, NULL) < 0) {
       return false;
   }


   // 连接各个滤镜
   if(avfilter_link(pSrcFilterContext, 0, formatFilterContext, 0) < 0) {
       std::cout << "link srs->format error" << std::endl;
       return false;
   }
   if(avfilter_link(formatFilterContext, 0, pSinkFilterContext, 0) < 0) {
       std::cout << "link format->sink error" << std::endl;
       return false;
   }

   if (avfilter_graph_config(spFilterGraph.get(),NULL) < 0) {
       std::cout << "graph config error" << std::endl;
       return false;
   }

   /** 遇到问题：重新编码时提示"[libmp3lame @ 0x10380b800] more samples than frame size (avcodec_encode_audio2)"
    *  分析原因：因为编码器上下文中设置的frame_size的大小为1152，而通过滤镜管道的av_buffersink_get_frame()函数获取的AVFrame的nb_samples的大小为1254 >=1152
    *  解决方案：通过av_buffersink_set_frame_size()给输出滤镜设置输出固定的AVFrame的nb_samples的大小
    *
    *  分析：av_buffersink_set_frame_size()函数内部实际上是将AVFilterLink的min_samples，max_samples，partial_buf_size都设置为了这个值。那么当调用av_buffersink_get_frame()函数时
    *  每次输出的AVFrame大小将为这个值
    *  备注：av_buffersink_set_frame_size()的调用必须再avfilter_link()连接之后，否则会崩溃
    */
   av_buffersink_set_frame_size(pSinkFilterContext, spEncoderContext->frame_size);

   return true;
}

bool cacheFrame(std::shared_ptr<AVFrame> &spFrame) {
   int ret = av_buffersrc_add_frame_flags(pSrcFilterContext, spFrame.get(), AV_BUFFERSRC_FLAG_KEEP_REF);
   if(ret < 0)
   {
       std::cout << "add frame to buffersrc error" << std::endl;
       return false;
   }
   return true;
}

std::shared_ptr<AVFrame> getFrame(std::shared_ptr<AVFrame> &spFrame) {
   if (spFrame == nullptr) {
       return nullptr;
   }
   std::shared_ptr<AVFrame> spOutFrame(av_frame_alloc(), [](AVFrame *frame){
       av_frame_free(&frame);
   });
   int ret = av_buffersink_get_frame(pSinkFilterContext, spOutFrame.get());
   if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
       return nullptr;
   if (ret < 0)
       return nullptr;

   static double currentpts = 0.0;
   double duration = (double)spOutFrame->nb_samples * 1000 / sampleRate;

   spOutFrame->pts = currentpts;
   spOutFrame->pkt_dts = currentpts;
   spOutFrame->pkt_dts = currentpts;
   currentpts += duration;
   return spOutFrame;
}

bool writePacketToMuxer(std::shared_ptr<AVPacket> &spPacket)
{
    int64_t dts = spPacket->pts * av_q2d(spInputFormat->streams[spPacket->stream_index]->time_base) * 1000;
    std::cout << (spPacket->stream_index == audioIndex ? "audio: " : "video: ") << dts << std::endl;

    if(spPacket->stream_index == audioIndex && isLive) {
           int64_t dts = spPacket->dts * av_q2d(spOutputFormat->streams[0]->time_base) * 1000;
           static auto firstFrame = std::chrono::system_clock::now();
           auto now = std::chrono::system_clock::now();
           uint64_t dis_millseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()
                   - std::chrono::duration_cast<std::chrono::seconds>(firstFrame.time_since_epoch()).count() * 1000;
           int64_t waittime = dts - dis_millseconds;
           msleep(waittime);
    }

   if(av_interleaved_write_frame(spOutputFormat.get(), spPacket.get()) < 0)
   {
       std::cout << "mux error" << std::endl;
       return false;
   }
   return true;
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
               break;
           else if (ret < 0)
           {
              std::cout << "encode frame error" << std::endl;
              break;
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

   createFilterGraph();
   if(!initAudioFilter(spInputFormat->streams[audioIndex]))
   {
       std::cout << "init resampler error" << std::endl;
       return -1;
   }

   int64_t lastDts = 0;

   std::thread processThread([]{
       bool first = true;
       std::shared_ptr<AVPacket> spInPakcet;
        for(;;) {
            packetMuxtex.lock();
            if ((spPakcetList.size() < 30 && first)||(spPakcetList.size() > 0)) {
                spInPakcet = spPakcetList.front();
                spPakcetList.pop_front();
                first = false;
                std::cout << spPakcetList.size() << std::endl;
            }
            else {
                packetMuxtex.unlock();
                msleep(5);
                continue;
            }
            packetMuxtex.unlock();

            // 不是视频数据，直接复用
            if(spInPakcet->stream_index != audioIndex)
                writePacketToMuxer(spInPakcet);
            else
            {
                if(!decodeAndEncodePacket(spInPakcet))
                    std::cout << "decode and encode packet error" << std::endl;
            }
        }
   });

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
       packetMuxtex.lock();
       spPakcetList.push_back(spInPakcet);
       packetMuxtex.unlock();

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

