 extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

#include <iostream>
#include <string>
#include <memory>
#include <thread>

std::string strInput = "/Users/fengyifan/Desktop/videos/sei_test_riverrun_ios.flv";

std::shared_ptr<AVFormatContext> spInputFormat;
int videoIndex = -1;

bool initDemuxer()
{
    AVDictionary *m = nullptr;
    // init demuxer
    AVFormatContext *pInputTemp = nullptr;
    if(avformat_open_input(&pInputTemp, strInput.c_str(), nullptr, &m) < 0)
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
   // find video stream
    for(unsigned int i = 0; i < spInputFormat->nb_streams; i++)
    {
       if(spInputFormat->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
       {
           videoIndex = i;
           break;
       }
    }
    return true;
}

#define H264_SEI_TYPE_USER_DATA_UNREGISTERED 5

void parse_sei( uint8_t *data, int size) {
    if ((data[0] & 0x1F) != 6) {
        return;
    }
    // SEI
    int currentPos = 1;
    int seitype = 0;
    do {
        seitype += *(data+currentPos);
        currentPos++;
    } while(currentPos <= size && *(data+currentPos) == 0xFF);

    int seisize = 0;
    do {
        seisize += *(data+currentPos);
        currentPos++;
    } while(currentPos <= size && *(data+currentPos) == 0xFF);

    std::string sei((char*)data+currentPos, seisize);
    currentPos += seisize;
    std::cout << seitype << "   " << seisize << "    " << sei << std::endl;
    if (currentPos < size) {
        data += currentPos;
        for(int i = 0; i < size - currentPos; i++) {
            printf("%x ", data[i]);
        }
        printf("\n");
    }
    // 平台
//    std::string sei((char*)data+1, size-1);
//    std::cout << sei << std::endl;
}

int main()
{
    // init demuxer
    if(!initDemuxer())
        return -1;

    if (spInputFormat->streams[videoIndex]->codecpar->codec_id != AV_CODEC_ID_H264)
    {
        std::cout << "video codec is not h264: " <<  spInputFormat->streams[videoIndex]->codecpar->codec_id << std::endl;
        return -1;
    }

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
        if (spPakcet->stream_index != videoIndex)
        {
            continue;
        }
        parse_sei(spPakcet->data+4, spPakcet->size);
    }
    return 0;
}
