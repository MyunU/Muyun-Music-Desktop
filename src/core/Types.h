#pragma once

#include <QString>
#include <QVariantMap>
#include <QVariantList>
#include <QVector>
#include <QJsonObject>
#include <QJsonArray>

namespace Muyun {

/// 诊断输出编译开关：发布构建（未定义 MUYUN_SELFTES）为 false，
/// 相关 printf 诊断块被死代码消除，二进制不含调试输出
#ifdef MUYUN_SELFTES
inline constexpr bool kDiag = true;
#else
inline constexpr bool kDiag = false;
#endif


// ---------------------------------------------------------------------------
// 基础枚举
// ---------------------------------------------------------------------------

/// 音乐平台。对外显示名：小芸/小秋/小枸/小蜗/小蜜
enum class Platform {
    Netease,   ///< 小芸音乐（网易云）    音源码 wy
    QQ,        ///< 小秋音乐（QQ音乐）    音源码 tx
    Kugou,     ///< 小枸音乐（酷狗）      音源码 kg
    Kuwo,      ///< 小蜗音乐（酷我）      音源码 kw
    Migu,      ///< 小蜜音乐（咪咕）      音源码 mg
    Local      ///< 本地音乐
};

/// 音质档次（升序）
enum class AudioQuality {
    K128,      ///< 标准 128kbps
    K320,      ///< 高品质 320kbps
    Flac,      ///< 无损 FLAC
    Flac24Bit, ///< 24Bit FLAC
    HiRes,     ///< Hi-Res 高解析度无损
    Atmos,     ///< Atmos 空间音频
    Master     ///< Master 母带音质
};

/// 播放模式
enum class PlayMode {
    Sequence,  ///< 顺序播放
    Loop,      ///< 列表循环
    Single,    ///< 单曲循环
    Shuffle    ///< 随机播放
};

/// 播放界面模式
enum class LyricsPlayerMode {
    Classic,   ///< 经典
    Amll,      ///< Apple Music 风格
    Mineradio  ///< Mineradio
};

// 枚举与字符串的互转（平台内部 id 名）
QString platformId(Platform p);
QString platformName(Platform p);       // 对外显示中文名
QString platformSourceCode(Platform p); // LX 音源码 wy/tx/kg/kw/mg
Platform platformFromId(const QString &id);
Platform platformFromSourceCode(const QString &code);

QString qualityId(AudioQuality q);
QString qualityName(AudioQuality q);
QString qualityDesc(AudioQuality q);
AudioQuality qualityFromId(const QString &id, bool *ok = nullptr);
QVector<AudioQuality> allQualitiesAsc();
QVector<AudioQuality> allQualitiesDesc();
/// 音质降级链：从目标音质沿降序向下回退
QVector<AudioQuality> qualityFallbackChain(AudioQuality q);

QString playModeId(PlayMode m);
QString playModeName(PlayMode m);
PlayMode playModeFromId(const QString &id);

// ---------------------------------------------------------------------------
// 歌曲
// ---------------------------------------------------------------------------

/// 单个音质的元信息（来自 LX 音源 types）
struct LxSongQualityMeta {
    QString type;   // 音质 id，如 "flac"
    QString size;   // 文件大小字符串
    QString hash;
    QVariantMap toMap() const;
    static LxSongQualityMeta fromMap(const QVariantMap &m);
};

/// LX 音源解析所需的原始元信息
struct LxSongMeta {
    QString source;        // wy / tx / kw / kg / mg
    QString songmid;
    QString songId;
    QString albumId;
    QString albumMid;
    QString strMediaMid;
    QString hash;
    QString copyrightId;
    QString lrcUrl;
    QString mrcUrl;
    QString trcUrl;
    QString interval;
    QString albumName;
    QString img;
    QVector<LxSongQualityMeta> types;
    QVariantMap toMap() const;
    static LxSongMeta fromMap(const QVariantMap &m);
};

/// 本地文件的 ReplayGain 标签
struct ReplayGain {
    double trackGainDb = 0.0;
    double trackPeak = 0.0;
    double albumGainDb = 0.0;
    double albumPeak = 0.0;
    bool hasTrackGain = false;
    QVariantMap toMap() const;
    static ReplayGain fromMap(const QVariantMap &m);
};

struct Song {
    QString id;
    QString name;
    QString artist;
    QString album;
    QString albumId;
    QString cover;
    QString url;
    double duration = 0.0; // 秒
    Platform platform = Platform::Netease;
    AudioQuality quality = AudioQuality::K128;
    bool hasQuality = false;

    LxSongMeta lx;          // LX 音源元信息
    bool hasLx = false;

    // 本地歌曲专属
    QString localPath;
    QString localFolder;
    qint64 localFileSize = 0;
    QString localModifiedAt;
    int localTrackNo = 0;
    int localDiscNo = 0;
    ReplayGain replayGain;

    // ---- 序列化 ----
    QVariantMap toMap() const;
    static Song fromMap(const QVariantMap &m);

    // ---- 辅助 ----
    bool isLocal() const { return platform == Platform::Local; }
    /// 生成稳定的身份 key（用于去重/收藏比对）
    QString identityKey() const;
    QString sourceCode() const { return platformSourceCode(platform); }
};

// ---------------------------------------------------------------------------
// 歌词
// ---------------------------------------------------------------------------

struct LyricWord {
    double startTime = 0.0;
    double endTime = 0.0;
    QString text;
    QVariantMap toMap() const;
};

struct LyricLine {
    double time = 0.0;
    QString text;
    QString translation;
    QString roman;
    QVector<LyricWord> words; // 逐字歌词
    QVariantMap toMap() const;
};

struct SongLyric {
    QVector<LyricLine> lines;
    QString rawLrc;
    QString rawTranslation;
    QString rawRoman;
    bool hasTranslation = false;
    bool hasRoman = false;
    bool hasWordByWord = false;
    QVariantList toList() const;
};

// ---------------------------------------------------------------------------
// 歌单 / 专辑 / 歌手 / 榜单 / 评论
// ---------------------------------------------------------------------------

struct Playlist {
    QString id;
    QString name;
    QString description;
    QString cover;
    QString creator;
    QVector<Song> songs;
    QString createdAt;
    QString updatedAt;
    bool isPublic = false;
    Platform platform = Platform::Netease;
    // 在线导入歌单
    QString sourceId;
    QString externalType;
    bool autoUpdate = false;
    QString importedAt;
    QString lastSyncedAt;
    QString lastSyncError;
    bool isOnlineImported = false;

    QVariantMap toMap() const;
    static Playlist fromMap(const QVariantMap &m);
    int songCount() const { return songs.size(); }
};

struct PlaylistSummary {
    QString id;
    QString name;
    QString creator;
    QString cover;
    int trackCount = 0;
    qint64 playCount = 0;
    bool hasPlayCount = false;
    Platform platform = Platform::Netease;
    QVariantMap toMap() const;
    static PlaylistSummary fromMap(const QVariantMap &m);
};

struct AlbumInfo {
    QString id;
    QString name;
    QString artist;
    QString artistId;
    QString cover;
    QString releaseDate;
    QString description;
    QString company;
    Platform platform = Platform::Netease;
    QVector<Song> songs;
    QVariantMap toMap() const;
    static AlbumInfo fromMap(const QVariantMap &m);
};

struct ArtistInfo {
    QString id;
    QString name;
    QString avatar;
    QString alias;
    QString identities;
    QString briefDesc;
    Platform platform = Platform::Netease;
    QVariantMap toMap() const;
};

struct ToplistInfo {
    QString id;
    QString name;
    QString description;
    QString cover;
    QString updateTime;
    Platform platform = Platform::Netease;
    QVariantMap toMap() const;
};

struct Comment {
    QString id;
    QString text;
    qint64 time = 0;
    QString timeStr;
    QString location;
    QStringList images;
    int likedCount = 0;
    bool liked = false;
    int replyNum = 0;
    QString userName;
    QString userAvatar;
    QString userId;
    QString replyText;   // 被引用的回复内容
    QString replyUser;
    QVariantMap toMap() const;
};

struct CommentPage {
    QVector<Comment> comments;
    int total = 0;
    int page = 1;
    int maxPage = 1;
};

struct SearchResult {
    QVector<Song> songs;
    QVector<PlaylistSummary> playlists;
    QVector<AlbumInfo> albums;
    bool hasMore = false;
    int total = 0;
};

} // namespace Muyun
