#pragma once

#include <QObject>
#include <QVector>
#include <QString>

#include "core/Types.h"

namespace Muyun {

/// 预缓存（v1.1.3）：当前歌播放后，把播放列表中接下来的歌静默下载到音频缓存目录，
/// 让下一首/下几首几乎零等待起播。
///   - 串行处理：等上一首缓存完（无论成败）再处理下一首，到歌单尽头停；
///   - 每次切歌重新从新当前曲之后开始（restart）；
///   - 跳过本地歌与已缓存歌；取不到链接/下载失败就静默跳过该首；
///   - 缓存文件不主动清理——由用户手动清除或启动时 3 天自动清扫
///     （PlayerEngine::sweepAudioCache）。
class AudioPreloader : public QObject
{
    Q_OBJECT
public:
    explicit AudioPreloader(QObject *parent = nullptr);

    /// 切歌/歌单变化时调用：重置预缓存，从 currentIndex+1 起逐首缓存
    void restart(const QVector<Song> &playlist, int currentIndex, AudioQuality quality);
    /// 停止在途任务（退出/清空列表时；restart 内部也会作废旧任务）
    void stop();

private:
    void pump();               ///< 取下一个待缓存索引并启动解析
    void onResolved(int idx, const QString &url, AudioQuality actual);
    void onDownloaded(int idx, bool ok);
    void advance();            ///< 处理完一首（无论成败）→ 推进队列

    QVector<Song> m_playlist;
    int m_currentIndex = -1;
    AudioQuality m_quality = AudioQuality::Master;
    int m_nextIndex = -1;      ///< 下一个待缓存的列表索引
    int m_activeIndex = -1;    ///< 正在解析/下载的列表索引（-1 = 空闲）
    quint64 m_epoch = 0;       ///< 代际：restart/stop 递增，作废在途回调
};

} // namespace Muyun
