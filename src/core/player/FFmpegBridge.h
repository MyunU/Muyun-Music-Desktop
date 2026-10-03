#pragma once

#include <QString>

// FFmpeg 公共头（已按最小闭包 vendored 到 src/vendor/ffmpeg/include）
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

namespace Muyun {

/**
 * @brief Qt 自带 FFmpeg 的运行时桥（动态解析符号，不链接导入库）
 *
 * 为什么这么干：
 *  - Qt 的多媒体后端就是 FFmpeg，所以 **avformat/avcodec/avutil/swresample 这几个 dll
 *    已经随我们的程序一起发布**（windeployqt 拷到 exe 同级目录），零打包成本；
 *  - Qt 的 MinGW 发行版**不给导入库**（只有 .dll），所以不能隐式链接；
 *  - 隐式链接还得改打包脚本，动态 LoadLibrary + GetProcAddress 反而最省事，
 *    并且天然支持"没有 FFmpeg 就退到 minimp3 / 退到 QMediaPlayer"。
 *
 * ⚠ 安全前提（本类的核心价值）：我们直接读 FFmpeg 公开结构体的字段
 *  （AVFrame::data/nb_samples/pts、AVCodecContext::sample_rate/sample_fmt/ch_layout、
 *   AVFormatContext::duration/streams、AVPacket::stream_index），
 *  一旦头文件描述的布局和运行时 dll 不一致，就是内存踩踏而不是"报错"。
 *  所以这里在加载后**逐个比对主版本号**（编译期 LIBAV*_VERSION_MAJOR vs 运行期 av*_version()），
 *  任一不匹配就把 available() 置 false，让上层完全绕开 FFmpeg 路径。
 */
class FFmpegBridge
{
public:
    /// 懒加载（首次需要解码时才 LoadLibrary；线程安全）
    static FFmpegBridge &instance();

    /// 全部必需符号齐备且 ABI 主版本匹配
    bool available() const { return m_ok; }
    QString error() const { return m_err; }
    /// 形如 "avcodec 61 / avformat 61 / avutil 59 / swresample 5"，供 UI/自检核对
    QString versionText() const { return m_verText; }

    // ---- 用到的函数（available() 为 false 时一律为 null） ----
    // avformat
    int (*format_open_input)(AVFormatContext **, const char *, AVInputFormat *, AVDictionary **) = nullptr;
    void (*format_close_input)(AVFormatContext **) = nullptr;
    int (*format_find_stream_info)(AVFormatContext *, AVDictionary **) = nullptr;
    int (*find_best_stream)(AVFormatContext *, enum AVMediaType, int, int, const AVCodec **, unsigned) = nullptr;
    int (*read_frame)(AVFormatContext *, AVPacket *) = nullptr;
    int (*seek_frame)(AVFormatContext *, int, int64_t, int) = nullptr;
    // avcodec
    const AVCodec *(*find_decoder)(enum AVCodecID) = nullptr;
    AVCodecContext *(*codec_alloc_context3)(const AVCodec *) = nullptr;
    void (*codec_free_context)(AVCodecContext **) = nullptr;
    int (*codec_parameters_to_context)(AVCodecContext *, const AVCodecParameters *) = nullptr;
    int (*codec_open2)(AVCodecContext *, const AVCodec *, AVDictionary **) = nullptr;
    int (*codec_send_packet)(AVCodecContext *, const AVPacket *) = nullptr;
    int (*codec_receive_frame)(AVCodecContext *, AVFrame *) = nullptr;
    void (*codec_flush_buffers)(AVCodecContext *) = nullptr;
    AVPacket *(*packet_alloc)() = nullptr;
    void (*packet_free)(AVPacket **) = nullptr;
    void (*packet_unref)(AVPacket *) = nullptr;
    // avutil
    AVFrame *(*frame_alloc)() = nullptr;
    void (*frame_free)(AVFrame **) = nullptr;
    void (*frame_unref)(AVFrame *) = nullptr;
    int (*strerror)(int, char *, size_t) = nullptr;
    // swresample
    int (*swr_alloc_set_opts2)(SwrContext **, const AVChannelLayout *, enum AVSampleFormat, int,
                               const AVChannelLayout *, enum AVSampleFormat, int, int, void *) = nullptr;
    int (*swr_init)(SwrContext *) = nullptr;
    int (*swr_convert)(SwrContext *, uint8_t **, int, const uint8_t **, int) = nullptr;
    void (*swr_free)(SwrContext **) = nullptr;

    /// 把 FFmpeg 错误码翻成可读文本（失败时给日志用）
    QString errMsg(int avErr) const;

private:
    FFmpegBridge();
    void resolve();

    bool m_ok = false;
    QString m_err;
    QString m_verText;
    void *m_mods[4] = { nullptr, nullptr, nullptr, nullptr };
};

inline FFmpegBridge &FF() { return FFmpegBridge::instance(); }

} // namespace Muyun
