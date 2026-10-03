#pragma once

#include "core/Types.h"

#include <QObject>
#include <QVariantList>

namespace Muyun {

/**
 * @brief 搜索控制器（暴露给 QML）
 *
 * 支持单平台与全平台搜索、搜索历史、热门搜索。
 */
class SearchController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QVariantList results READ results NOTIFY resultsChanged)
    Q_PROPERTY(QVariantList playlists READ playlists NOTIFY resultsChanged)
    Q_PROPERTY(QVariantList albums READ albums NOTIFY resultsChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY searchingChanged)
    Q_PROPERTY(QString keyword READ keyword NOTIFY keywordChanged)
    Q_PROPERTY(QString platform READ platform WRITE setPlatform NOTIFY platformChanged)
    Q_PROPERTY(QStringList searchHistory READ searchHistory NOTIFY searchHistoryChanged)
    Q_PROPERTY(QStringList hotWords READ hotWords NOTIFY hotWordsChanged)
    Q_PROPERTY(QVariantList hotSongs READ hotSongs NOTIFY hotSongsChanged)
    Q_PROPERTY(QVariantList topArtists READ topArtists NOTIFY topArtistsChanged)
    Q_PROPERTY(QStringList suggestions READ suggestions NOTIFY suggestionsChanged)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY resultsChanged)
    Q_PROPERTY(int total READ total NOTIFY resultsChanged)

public:
    explicit SearchController(QObject *parent = nullptr);

    QVariantList results() const;
    QVariantList playlists() const;
    QVariantList albums() const;
    bool searching() const { return m_searching; }
    QString keyword() const { return m_keyword; }
    QString platform() const { return m_platform; }
    QStringList searchHistory() const { return m_history; }
    QStringList hotWords() const;
    QVariantList hotSongs() const { return m_hotSongs; }
    QVariantList topArtists() const { return m_topArtists; }
    QStringList suggestions() const { return m_suggestions; }
    bool hasMore() const { return m_hasMore; }
    int total() const { return m_total; }

    void setPlatform(const QString &platform);

    /// 执行搜索（platform 为 empty/"all" 时聚合全平台）
    Q_INVOKABLE void search(const QString &keyword, int page = 1);
    /// 搜索联想（输入时实时提示）
    Q_INVOKABLE void suggest(const QString &keyword);
    /// 加载更多（下一页追加）
    Q_INVOKABLE void loadMore();
    /// 清空结果
    Q_INVOKABLE void clear();
    /// 历史记录
    Q_INVOKABLE void removeHistoryItem(const QString &word);
    Q_INVOKABLE void clearHistory();
    /// 平台显示名
    Q_INVOKABLE QString platformName(const QString &code) const;
    /// 可选平台列表（含"全部"）
    Q_INVOKABLE QVariantList platformOptions() const;

signals:
    void resultsChanged();
    void searchingChanged();
    void keywordChanged();
    void platformChanged();
    void searchHistoryChanged();
    void hotWordsChanged();
    void hotSongsChanged();
    void topArtistsChanged();
    void suggestionsChanged();
    void searchFailed(const QString &message);

private:
    void loadHistory();
    void saveHistory();
    void loadHotSongs();
    void loadTopArtists();

    QVector<Song> m_songs;
    QVector<PlaylistSummary> m_playlists;
    QVector<AlbumInfo> m_albums;
    QString m_keyword;
    QString m_platform; // 空表示全部
    int m_page = 1;
    int m_total = 0;
    bool m_hasMore = false;
    bool m_searching = false;
    QStringList m_history;
    QStringList m_hotWords;
    QVariantList m_hotSongs;
    QVariantList m_topArtists;
    QStringList m_suggestions;
    quint64 m_suggestSeq = 0;   ///< 联想请求序号：过期结果直接丢弃
};

} // namespace Muyun
