extern "C"
{
#include <libavformat/avformat.h>
}

#include <iostream>
#include <string>
#include <memory>
#include <thread>

std::string strInput = "http://1251021019.vod2.myqcloud.com/f98a4078vodcq1251021019/61978299387702292910682981/f0.mp4";
// std::string strInput = "/Users/fengyifan/Desktop/project/go/hlsloader/file/134048_20ba0_396790f897ad4ba4aadf7beb5551e98f.m3u8";
// std::string strInput = "/Users/fengyifan/Desktop/project/go/hlsloader/jl/mtly_20211209.mp4";
// std::string strInput = "/Users/fengyifan/Desktop/project/go/hlsloader/jl/mtly_20211209_mp4box.mp4";
// std::string strInput = "/Users/fengyifan/Desktop/project/ffmpeg_build/FFmpeg/mtly_20211209_out.mp4";
std::string strOutFmt = "mp4";
std::string strOutput = "/Users/fengyifan/Desktop/project/C++/ffmpeg_example/remuxer_example/test.mp4";
bool isLive = false; // 设置为true，将模拟从实时流推送。

std::shared_ptr<AVFormatContext> spInputFormat;
std::shared_ptr<AVFormatContext> spOutputFormat;
int videoIndex = -1;

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
        AVStream *outStream = avformat_new_stream(spOutputFormat.get(), nullptr);
        AVStream *inStream = spInputFormat->streams[i];
        avcodec_parameters_copy(outStream->codecpar, inStream->codecpar);
        if(inStream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            std::cout << "video instream: " << inStream->time_base.den << "/" << inStream->time_base.num << std::endl;
            videoIndex = i;
        } else {
            std::cout << "audio stream: " << inStream->time_base.den << "/" << inStream->time_base.num << std::endl;
        }
        outStream->time_base = inStream->time_base;
        // outStream->time_base.num = 1000;  
        // outStream->time_base.den = 1;
        outStream->codecpar->codec_tag = 0;      
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

int main()
{
    // init demuxer
    if(!initDemuxer())
        return -1;

    // init muxer
    if(!initMuxer())
        return -1;

    int64_t lastDts = 0;

    int64_t firstVideoDts = -1;
    int64_t firstAudioDts = -1;
    // process
    for (;;)
    {
        std::shared_ptr<AVPacket> spPakcet(av_packet_alloc(), [&](AVPacket* pPacket){
            av_packet_free(&pPacket);
        });
        int ret = av_read_frame(spInputFormat.get(), spPakcet.get());
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
        int64_t dts = spPakcet->dts * av_q2d(spInputFormat->streams[spPakcet->stream_index]->time_base) * 1000;
        if (videoIndex == spPakcet->stream_index) {
            if (firstVideoDts == -1) {
                firstVideoDts = dts;
            }
            std::cout << "video dts: " << dts - firstVideoDts << std::endl;
        } else {
            if (firstAudioDts == -1) {
                firstAudioDts = dts;
            }
            std::cout << "audio dts: " << dts - firstAudioDts << std::endl;
        }
 
        // 如果是直播流(推送文件模拟直播流)，等待到时
        if(isLive && spPakcet->dts != AV_NOPTS_VALUE && videoIndex == spPakcet->stream_index)
        {
            int64_t dts = spPakcet->dts * av_q2d(spInputFormat->streams[videoIndex]->time_base) * 1000;
            if(lastDts != 0 && dts - lastDts > 0)
                msleep(dts-lastDts);
            lastDts = dts;
        }

        // if(av_write_frame(spOutputFormat.get(), spPakcet.get()) < 0)
        // {
        //     std::cout << "mux error" << std::endl;
        //     break;
        // }
    }

    // write trailer
    av_write_trailer(spOutputFormat.get());
    if(!(spOutputFormat->oformat->flags & AVFMT_NOFILE)) {
        avio_close(spOutputFormat->pb);
    }
    std::cout << "remux over" << std::endl;
    return 0;
}
