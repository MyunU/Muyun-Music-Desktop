#include "SearchController.h"

#include "core/music/MusicSdk.h"
#include "core/storage/DocumentStore.h"

#include <QtConcurrent>
#include <QFutureWatcher>
#include <algorithm>

namespace Muyun {

namespace {
struct SearchOutcome {
    QVector<Song> songs;
    QVector<PlaylistSummary> playlists;
    QVector<AlbumInfo> albums;
    int total = 0;
    bool hasMore = false;
};
} // namespace

SearchController::SearchController(QObject *parent) : QObject(parent)
{
    loadHistory();
    // 恢复上次选择的平台（空=全部）
    m_platform = DocumentStore::instance()->readSync(QStringLiteral("feature"),
                                                     QStringLiteral("searchPlatform")).toString();
    // 默认热搜兜底（异步从音源加载真实热搜后覆盖）
    m_hotWords = {QStringLiteral("周杰伦"), QStringLiteral("陈奕迅"),
                  QStringLiteral("邓紫棋"), QStringLiteral("薛之谦"),
                  QStringLiteral("告五人"), QStringLiteral("五月天"),
                  QStringLiteral("李荣浩"), QStringLiteral("毛不易")};
    loadHotSongs();
    loadTopArtists();
}

void SearchController::loadHotSongs()
{
    MusicSdk *sdk = MusicSdk::instance();
    auto *watcher = new QFutureWatcher<QVariantList>(this);
    connect(watcher, &QFutureWatcher<QVariantList>::finished, this,
            [this, watcher]() {
                const QVariantList songs = watcher->result();
                watcher->deleteLater();
                if (songs.isEmpty()) return;
                m_hotSongs = songs;
                emit hotSongsChanged();
                // 纯词列表由热搜歌曲派生（保持旧接口兼容）
                QStringList words;
                for (const auto &item : songs) {
                    const QVariantMap m = item.toMap();
                    const QString w = m.value(QStringLiteral("word")).toString();
                    if (!w.isEmpty()) words.append(w);
                }
                if (!words.isEmpty()) {
                    m_hotWords = words;
                    emit hotWordsChanged();
                }
            });
    watcher->setFuture(QtConcurrent::run([sdk]() {
        if (auto *src = sdk->source(QStringLiteral("wy"))) {
            QVariantList v = src->hotSearchSongs();
            if (v.isEmpty()) {
                // 接口无歌曲信息时退化为纯词
                const QStringList words = src->hotSearchWords();
                for (const auto &w : words) {
                    QVariantMap m;
                    m[QStringLiteral("word")] = w;
                    v.append(m);
                }
            }
            return v;
        }
        return QVariantList();
    }));
}

void SearchController::loadTopArtists()
{
    MusicSdk *sdk = MusicSdk::instance();
    auto *watcher = new QFutureWatcher<QVariantList>(this);
    connect(watcher, &QFutureWatcher<QVariantList>::finished, this,
            [this, watcher]() {
                const QVariantList artists = watcher->result();
                watcher->deleteLater();
                if (!artists.isEmpty()) {
                    m_topArtists = artists;
                    emit topArtistsChanged();
                }
            });
    watcher->setFuture(QtConcurrent::run([sdk]() {
        if (auto *src = sdk->source(QStringLiteral("wy")))
            return src->topArtists(10);
        return QVariantList();
    }));
}

QVariantList SearchController::results() const
{
    QVariantList out;
    for (const auto &s : m_songs) out.append(s.toMap());
    return out;
}

QVariantList SearchController::playlists() const
{
    QVariantList out;
    for (const auto &p : m_playlists) out.append(p.toMap());
    return out;
}

QVariantList SearchController::albums() const
{
    QVariantList out;
    for (const auto &a : m_albums) out.append(a.toMap());
    return out;
}

QStringList SearchController::hotWords() const { return m_hotWords; }

void SearchController::setPlatform(const QString &platform)
{
    if (m_platform == platform) return;
    m_platform = platform;
    // 实时记录用户选的播放/搜索平台（重启后恢复）
    DocumentStore::instance()->write(QStringLiteral("feature"),
                                     QStringLiteral("searchPlatform"), platform);
    emit platformChanged();

    // 切平台后清空旧结果，若当前有关键词则按新平台重新搜索，
    // 否则会残留上一平台（或全平台）的混合结果。
    const QString kw = m_keyword;
    m_songs.clear();
    m_playlists.clear();
    m_albums.clear();
    m_total = 0;
    m_hasMore = false;
    m_page = 1;
    emit resultsChanged();

    if (!kw.isEmpty())
        search(kw, 1);
}

QString SearchController::platformName(const QString &code) const
{
    if (code.isEmpty() || code == QStringLiteral("all")) return QStringLiteral("全部平台");
    auto *src = MusicSdk::instance()->source(code);
    return src ? src->name() : code;
}

QVariantList SearchController::platformOptions() const
{
    QVariantList out;
    QVariantMap all;
    all[QStringLiteral("id")] = QStringLiteral("all");
    all[QStringLiteral("name")] = QStringLiteral("全部平台");
    out.append(all);
    // sourceInfos 用 "code" 键，统一转成 "id"（QML 端按 id 取值）
    for (const auto &item : MusicSdk::instance()->sourceInfos()) {
        const QVariantMap info = item.toMap();
        QVariantMap m;
        m[QStringLiteral("id")] = info.value(QStringLiteral("code"));
        m[QStringLiteral("name")] = info.value(QStringLiteral("name"));
        out.append(m);
    }
    return out;
}

void SearchController::search(const QString &keyword, int page)
{
    const QString kw = keyword.trimmed();
    if (kw.isEmpty()) { clear(); return; }

    if (m_keyword != kw) { m_keyword = kw; emit keywordChanged(); }
    m_page = page;
    m_searching = true;
    emit searchingChanged();

    // 记录搜索历史
    if (m_history.contains(kw)) m_history.removeAll(kw);
    m_history.prepend(kw);
    while (m_history.size() > 20) m_history.removeLast();
    saveHistory();
    emit searchHistoryChanged();

    const bool allPlatform = m_platform.isEmpty() || m_platform == QStringLiteral("all");
    const QString platform = m_platform;

    auto *watcher = new QFutureWatcher<SearchOutcome>(this);
    connect(watcher, &QFutureWatcher<SearchOutcome>::finished, this,
            [this, watcher, kw]() {
                const SearchOutcome oc = watcher->result();
                watcher->deleteLater();
                m_searching = false;
                emit searchingChanged();
                if (m_keyword != kw) return; // 结果已过期

                m_songs = oc.songs;
                m_playlists = oc.playlists;
                m_albums = oc.albums;
                m_total = oc.total;
                m_hasMore = oc.hasMore;
                emit resultsChanged();
            });

    MusicSdk *sdk = MusicSdk::instance();
    watcher->setFuture(QtConcurrent::run([sdk, kw, platform, allPlatform, page]() {
        SearchOutcome oc;
        if (allPlatform) {
            const SearchResult r = sdk->searchAll(kw, page, 30);
            oc.songs = r.songs;
            oc.total = r.total;
            oc.hasMore = r.hasMore;
        } else {
            const SearchResult r = sdk->search(platform, kw, page, 30);
            oc.songs = r.songs;
            oc.total = r.total;
            oc.hasMore = r.hasMore;
            auto *src = sdk->source(platform);
            if (src) {
                oc.playlists = src->searchPlaylists(kw, page, 20);
                oc.albums = src->searchAlbums(kw, page, 20);
            }
        }
        return oc;
    }));
}

void SearchController::suggest(const QString &keyword)
{
    const QString kw = keyword.trimmed();
    if (kw.isEmpty()) {
        if (!m_suggestions.isEmpty()) {
            m_suggestions.clear();
            emit suggestionsChanged();
        }
        return;
    }

    MusicSdk *sdk = MusicSdk::instance();
    const quint64 seq = ++m_suggestSeq;
    auto *watcher = new QFutureWatcher<QStringList>(this);
    connect(watcher, &QFutureWatcher<QStringList>::finished, this,
            [this, watcher, seq]() {
                const QStringList words = watcher->result();
                watcher->deleteLater();
                // 结果过期（用户已继续输入，后续请求已发起）则丢弃
                if (seq != m_suggestSeq) return;
                m_suggestions = words;
                emit suggestionsChanged();
            });
    watcher->setFuture(QtConcurrent::run([sdk, kw]() {
        // 跨平台聚合做联想（与搜索结果页同一数据路径 searchAll）：
        // 单靠 wy 不行——周杰伦等版权缺失歌手在 wy 结果里根本不出现，
        // 而 searchAll 聚合 kw/kg/tx/mg 后热度原唱自然排前。
        QStringList out;
        QSet<QString> seen;
        const SearchResult r = sdk->searchAll(kw, 1, 10);
        for (const Song &s : r.songs) {
            if (s.name.isEmpty()) continue;
            const QString term = s.artist.isEmpty()
                                   ? s.name : (s.name + QLatin1Char(' ') + s.artist);
            if (seen.contains(term)) continue;
            seen.insert(term);
            out.append(term);
            if (out.size() >= 10) break;
        }
        return out;
    }));
}

void SearchController::loadMore()
{
    if (m_keyword.isEmpty() || !m_hasMore) return;
    const int nextPage = m_page + 1;
    const bool allPlatform = m_platform.isEmpty() || m_platform == QStringLiteral("all");
    const QString platform = m_platform;
    const QString kw = m_keyword;

    m_searching = true;
    emit searchingChanged();

    auto *watcher = new QFutureWatcher<SearchResult>(this);
    connect(watcher, &QFutureWatcher<SearchResult>::finished, this,
            [this, watcher, nextPage]() {
                const SearchResult r = watcher->result();
                watcher->deleteLater();
                m_searching = false;
                emit searchingChanged();
                m_songs.append(r.songs);
                m_page = nextPage;
                m_total = r.total;
                m_hasMore = r.hasMore;
                emit resultsChanged();
            });

    MusicSdk *sdk = MusicSdk::instance();
    watcher->setFuture(QtConcurrent::run([sdk, kw, platform, allPlatform, nextPage]() {
        return allPlatform ? sdk->searchAll(kw, nextPage, 30)
                           : sdk->search(platform, kw, nextPage, 30);
    }));
}

void SearchController::clear()
{
    m_songs.clear();
    m_playlists.clear();
    m_albums.clear();
    m_keyword.clear();
    m_page = 1;
    m_total = 0;
    m_hasMore = false;
    emit resultsChanged();
    emit keywordChanged();
}

void SearchController::loadHistory()
{
    auto *store = DocumentStore::instance();
    m_history = store->readSync(QStringLiteral("feature"),
                                QStringLiteral("searchHistory")).toStringList();
}

void SearchController::saveHistory()
{
    auto *store = DocumentStore::instance();
    store->write(QStringLiteral("feature"), QStringLiteral("searchHistory"), m_history);
}

void SearchController::removeHistoryItem(const QString &word)
{
    m_history.removeAll(word);
    saveHistory();
    emit searchHistoryChanged();
}

void SearchController::clearHistory()
{
    m_history.clear();
    saveHistory();
    emit searchHistoryChanged();
}

} // namespace Muyun
