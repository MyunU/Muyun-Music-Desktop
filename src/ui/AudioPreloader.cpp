#include "AudioPreloader.h"

#include "core/localmusic/TagReader.h"
#include "core/music/MusicSdk.h"
#include "core/network/HttpClient.h"
#include "core/player/PlayerEngine.h"
#include "core/utils/AudioUrl.h"

#include <QtConcurrent>

namespace Muyun {

AudioPreloader::AudioPreloader(QObject *parent) : QObject(parent) {}

void AudioPreloader::restart(const QVector<Song> &playlist, int currentIndex, AudioQuality quality)
{
    ++m_epoch;                          // 作废在途解析/下载回调
    m_playlist = playlist;
    m_currentIndex = currentIndex;
    m_quality = quality;
    m_nextIndex = currentIndex + 1;
    m_activeIndex = -1;
    pump();
}

void AudioPreloader::stop()
{
    ++m_epoch;
    m_activeIndex = -1;
}

void AudioPreloader::pump()
{
    if (m_activeIndex >= 0) return;     // 有在途任务，等它完成再推进（串行）
    const int n = m_playlist.size();
    while (m_nextIndex < n) {
        const int idx = m_nextIndex++;
        const Song &s = m_playlist.at(idx);
        if (s.isLocal()) continue;      // 本地歌无需缓存
        const QString key = s.identityKey() + QLatin1Char('@') + qualityId(m_quality);
        if (!PlayerEngine::cachedAudioFile(key, s.duration).isEmpty()) continue;  // 已缓存（且校验有效）
        // 这首真要缓存：在工作线程解析取源，回调回主线程
        const quint64 epoch = m_epoch;
        m_activeIndex = idx;
        const Song song = s;
        const AudioQuality q = m_quality;
        QtConcurrent::run([this, epoch, idx, song, q]() {
            MusicSdk *sdk = MusicSdk::instance();
            AudioQuality actual = q;
            const QString url = sdk->resolveUrl(song, q, &actual);
            QMetaObject::invokeMethod(this, [this, epoch, idx, url, actual]() {
                if (epoch != m_epoch) return;      // 已作废
                onResolved(idx, url, actual);
            }, Qt::QueuedConnection);
        });
        return;
    }
    // 队列尽头：本轮预缓存结束，等下次切歌 restart
    m_activeIndex = -1;
}

void AudioPreloader::onResolved(int idx, const QString &url, AudioQuality actual)
{
    if (idx != m_activeIndex) return;
    if (url.isEmpty()) { advance(); return; }     // 取不到链接：跳过该首
    const Song &s = m_playlist.at(idx);
    const QString key = s.identityKey() + QLatin1Char('@') + qualityId(actual);
    const QString savePath = PlayerEngine::cachePathForKey(key);
    HttpOptions opt;
    opt.referer = refererForAudioUrl(url);
    const quint64 epoch = m_epoch;
    auto *self = this;
    HttpClient::instance()->downloadFile(url, savePath, opt, nullptr,
        [self, epoch, idx, savePath](bool ok, const QString &) {
            bool good = ok;
            if (good && !TagReader::isAudioFile(savePath)) {
                QFile::remove(savePath);           // 无效响应（JSON 等）不进缓存
                good = false;
            } else if (!good) {
                QFile::remove(savePath);           // 下载失败 → 清半截文件，防命中坏缓存
            }
            QMetaObject::invokeMethod(self, [self, epoch, idx, good]() {
                if (epoch != self->m_epoch) return;
                self->onDownloaded(idx, good);
            }, Qt::QueuedConnection);
        });
}

void AudioPreloader::onDownloaded(int idx, bool ok)
{
    Q_UNUSED(ok)
    if (idx != m_activeIndex) return;
    m_activeIndex = -1;
    pump();                                       // 下一首（串行：等这首完）
}

void AudioPreloader::advance()
{
    if (m_activeIndex >= 0) m_activeIndex = -1;
    pump();
}

} // namespace Muyun
