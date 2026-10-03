#include "FFmpegBridge.h"

// 顺序要紧：先 FFmpeg 头，再 windows.h（并关掉 min/max 宏，免得污染 FFmpeg 头里的内联代码）
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <windows.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfoList>

namespace Muyun {

namespace {

// 从 exe 同目录（含 plugins/multimedia 等子目录）里找某个 FFmpeg 模块，
// 找不到再交给系统搜索路径。返回已加载模块句柄（未加载到返回 null）。
void *loadModule(const QString &stem)
{
    const QString exeDir = QCoreApplication::applicationDirPath();
    QStringList candidates;
    if (!exeDir.isEmpty()) {
        QDir dir(exeDir);
        const QStringList subDirs = { QString(), QStringLiteral("/plugins"),
                                      QStringLiteral("/plugins/multimedia") };
        for (const QString &sub : subDirs) {
            QDir d(dir.absolutePath() + sub);
            if (!d.exists()) continue;
            const QList<QFileInfo> hits = d.entryInfoList(
                { stem + QStringLiteral("-*.dll") }, QDir::Files);
            for (const QFileInfo &fi : hits)
                candidates << fi.absoluteFilePath();
        }
    }
    // 兜底：Qt 6.8 的固定名字（万一没随包部署，让系统搜索路径试一次）
    candidates << (stem + QStringLiteral("-61.dll"))
               << (stem + QStringLiteral("-60.dll"))
               << (stem + QStringLiteral("-59.dll"))
               << (stem + QStringLiteral("-58.dll"))
               << (stem + QStringLiteral("-5.dll"))
               << (stem + QStringLiteral("-4.dll"));

    for (const QString &c : candidates) {
        HMODULE h = LoadLibraryW(reinterpret_cast<const wchar_t *>(c.utf16()));
        if (h) return reinterpret_cast<void *>(h);
    }
    return nullptr;
}

template <typename T>
T getSym(void *mod, const char *name)
{
    if (!mod) return nullptr;
    return reinterpret_cast<T>(reinterpret_cast<FARPROC>(
        GetProcAddress(static_cast<HMODULE>(mod), name)));
}

} // namespace

FFmpegBridge &FFmpegBridge::instance()
{
    static FFmpegBridge self;      // C++11 起局部静态初始化线程安全
    return self;
}

FFmpegBridge::FFmpegBridge()
{
    resolve();
}

void FFmpegBridge::resolve()
{
    m_ok = false;

    m_mods[0] = loadModule(QStringLiteral("avformat"));
    m_mods[1] = loadModule(QStringLiteral("avcodec"));
    m_mods[2] = loadModule(QStringLiteral("avutil"));
    m_mods[3] = loadModule(QStringLiteral("swresample"));
    if (!m_mods[0] || !m_mods[1] || !m_mods[2]) {
        m_err = QStringLiteral("未找到 FFmpeg 运行库（avformat/avcodec/avutil）");
        return;
    }

    // ---- ABI 护栏：读结构体字段前必须确认头与 dll 主版本一致 ----
    auto avformatVer  = getSym<unsigned (*)()>(m_mods[0], "avformat_version");
    auto avcodecVer   = getSym<unsigned (*)()>(m_mods[1], "avcodec_version");
    auto avutilVer    = getSym<unsigned (*)()>(m_mods[2], "avutil_version");
    auto swresVer     = getSym<unsigned (*)()>(m_mods[3], "swresample_version");
    if (!avformatVer || !avcodecVer || !avutilVer) {
        m_err = QStringLiteral("FFmpeg 缺少版本查询函数");
        return;
    }
    // FFmpeg 版本宏是 AV_VERSION_INT(major,minor,micro) = major<<16 | minor<<8 | micro
    // （不是 <<24！按错位移会把主版本读成 0，于是永远判"版本不一致"而静默禁用整条 FFmpeg 路）
    const int majFmt = int(avformatVer() >> 16);
    const int majCod = int(avcodecVer() >> 16);
    const int majUtl = int(avutilVer() >> 16);
    const int majSwr = swresVer ? int(swresVer() >> 16) : 0;
    m_verText = QStringLiteral("avcodec %1 / avformat %2 / avutil %3 / swresample %4")
                    .arg(majCod).arg(majFmt).arg(majUtl).arg(majSwr);

    if (majFmt != LIBAVFORMAT_VERSION_MAJOR || majCod != LIBAVCODEC_VERSION_MAJOR
        || majUtl != LIBAVUTIL_VERSION_MAJOR
        || (swresVer && majSwr != LIBSWRESAMPLE_VERSION_MAJOR)) {
        m_err = QStringLiteral("FFmpeg 版本与编译期头不一致（运行 %1，编译 %2/%3/%4）→ 不走 FFmpeg")
                    .arg(m_verText)
                    .arg(LIBAVCODEC_VERSION_MAJOR).arg(LIBAVFORMAT_VERSION_MAJOR)
                    .arg(LIBAVUTIL_VERSION_MAJOR);
        return;
    }

    // ---- 解析符号 ----
    format_open_input       = getSym<decltype(format_open_input)>(m_mods[0], "avformat_open_input");
    format_close_input      = getSym<decltype(format_close_input)>(m_mods[0], "avformat_close_input");
    format_find_stream_info = getSym<decltype(format_find_stream_info)>(m_mods[0], "avformat_find_stream_info");
    find_best_stream        = getSym<decltype(find_best_stream)>(m_mods[0], "av_find_best_stream");
    read_frame              = getSym<decltype(read_frame)>(m_mods[0], "av_read_frame");
    seek_frame              = getSym<decltype(seek_frame)>(m_mods[0], "av_seek_frame");

    find_decoder            = getSym<decltype(find_decoder)>(m_mods[1], "avcodec_find_decoder");
    codec_alloc_context3    = getSym<decltype(codec_alloc_context3)>(m_mods[1], "avcodec_alloc_context3");
    codec_free_context      = getSym<decltype(codec_free_context)>(m_mods[1], "avcodec_free_context");
    codec_parameters_to_context =
        getSym<decltype(codec_parameters_to_context)>(m_mods[1], "avcodec_parameters_to_context");
    codec_open2             = getSym<decltype(codec_open2)>(m_mods[1], "avcodec_open2");
    codec_send_packet       = getSym<decltype(codec_send_packet)>(m_mods[1], "avcodec_send_packet");
    codec_receive_frame     = getSym<decltype(codec_receive_frame)>(m_mods[1], "avcodec_receive_frame");
    codec_flush_buffers     = getSym<decltype(codec_flush_buffers)>(m_mods[1], "avcodec_flush_buffers");
    packet_alloc            = getSym<decltype(packet_alloc)>(m_mods[1], "av_packet_alloc");
    packet_free             = getSym<decltype(packet_free)>(m_mods[1], "av_packet_free");
    packet_unref            = getSym<decltype(packet_unref)>(m_mods[1], "av_packet_unref");

    frame_alloc             = getSym<decltype(frame_alloc)>(m_mods[2], "av_frame_alloc");
    frame_free              = getSym<decltype(frame_free)>(m_mods[2], "av_frame_free");
    frame_unref             = getSym<decltype(frame_unref)>(m_mods[2], "av_frame_unref");
    strerror                = getSym<decltype(strerror)>(m_mods[2], "av_strerror");

    swr_alloc_set_opts2     = getSym<decltype(swr_alloc_set_opts2)>(m_mods[3], "swr_alloc_set_opts2");
    swr_init                = getSym<decltype(swr_init)>(m_mods[3], "swr_init");
    swr_convert             = getSym<decltype(swr_convert)>(m_mods[3], "swr_convert");
    swr_free                = getSym<decltype(swr_free)>(m_mods[3], "swr_free");

    // 少一个符号就不能用（宁可整条路关掉，也不要在半套 API 上跑）
    QStringList lack;
    if (!format_open_input) lack << QStringLiteral("avformat_open_input");
    if (!format_close_input) lack << QStringLiteral("avformat_close_input");
    if (!format_find_stream_info) lack << QStringLiteral("avformat_find_stream_info");
    if (!find_best_stream) lack << QStringLiteral("av_find_best_stream");
    if (!read_frame) lack << QStringLiteral("av_read_frame");
    if (!seek_frame) lack << QStringLiteral("av_seek_frame");
    if (!find_decoder) lack << QStringLiteral("avcodec_find_decoder");
    if (!codec_alloc_context3) lack << QStringLiteral("avcodec_alloc_context3");
    if (!codec_free_context) lack << QStringLiteral("avcodec_free_context");
    if (!codec_parameters_to_context) lack << QStringLiteral("avcodec_parameters_to_context");
    if (!codec_open2) lack << QStringLiteral("avcodec_open2");
    if (!codec_send_packet) lack << QStringLiteral("avcodec_send_packet");
    if (!codec_receive_frame) lack << QStringLiteral("avcodec_receive_frame");
    if (!codec_flush_buffers) lack << QStringLiteral("avcodec_flush_buffers");
    if (!packet_alloc) lack << QStringLiteral("av_packet_alloc");
    if (!packet_free) lack << QStringLiteral("av_packet_free");
    if (!packet_unref) lack << QStringLiteral("av_packet_unref");
    if (!frame_alloc) lack << QStringLiteral("av_frame_alloc");
    if (!frame_free) lack << QStringLiteral("av_frame_free");
    if (!frame_unref) lack << QStringLiteral("av_frame_unref");
    if (!swr_alloc_set_opts2) lack << QStringLiteral("swr_alloc_set_opts2");
    if (!swr_init) lack << QStringLiteral("swr_init");
    if (!swr_convert) lack << QStringLiteral("swr_convert");
    if (!swr_free) lack << QStringLiteral("swr_free");

    if (!lack.isEmpty()) {
        m_err = QStringLiteral("FFmpeg 缺少符号：%1").arg(lack.join(QStringLiteral(", ")));
        return;
    }

    // FFmpeg 默认往 stderr 打日志（"moov atom not found" 之类），
    // 会污染我们扫 QML 报错的日志 → 只留致命错误
    if (auto setLevel = getSym<void (*)(int)>(m_mods[2], "av_log_set_level"))
        setLevel(AV_LOG_FATAL);

    m_ok = true;
}

QString FFmpegBridge::errMsg(int avErr) const
{
    if (strerror) {
        char buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
        if (strerror(avErr, buf, sizeof(buf)) == 0)
            return QString::fromUtf8(buf);
    }
    return QStringLiteral("FFmpeg 错误 %1").arg(avErr);
}

} // namespace Muyun
