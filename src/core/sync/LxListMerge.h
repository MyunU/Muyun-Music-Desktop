#pragma once

#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>

namespace Muyun {

/**
 * @brief 洛雪 list 模块的数据运算（纯函数，操作 ListData JSON）。
 *
 * ListData 结构（LX.Sync.List.ListData）：
 * {
 *   "defaultList": [MusicInfo...],          // 试听列表
 *   "loveList":    [MusicInfo...],          // 我喜欢
 *   "userList":    [ {id,name,source,sourceListId,locationUpdateTime,
 *                     list:[MusicInfo...]} ] // 自建歌单
 * }
 * MusicInfo 视为不透明对象，仅用其 "id" 字段判重。
 *
 * 合并算法逐段移植自 lx-music-desktop
 * src/main/modules/sync/server/modules/list/sync/sync.ts。
 * addMusicLocationType 固定取洛雪默认值 'bottom'（新数据排后）。
 */
namespace LxListMerge {

/// ListData 三个列表是否全空
bool isEmpty(const QJsonObject &listData);

/// 合并：source 在前 + target 在后，按歌曲 id 去重
QJsonArray mergeMusicList(const QJsonArray &source, const QJsonArray &target);

/// 并集合并（mode: merge_local_remote 时 source=local）
QJsonObject mergeData(const QJsonObject &source, const QJsonObject &target);

/// 覆盖合并：source 为主，target 里 source 没有的歌单按位置并入
QJsonObject overwriteData(const QJsonObject &source, const QJsonObject &target);

/// 三方合并（快照基线）：local/remote 各自相对 snapshot 的删除会被尊重
QJsonObject mergeFromSnapshot(const QJsonObject &local, const QJsonObject &remote,
                              const QJsonObject &snapshot);

/// 把一条 lx list 同步 action（LX.Sync.List.ActionList）应用到 data。
/// 返回 false = 未知 action。list_data_overwrite 会整体替换。
bool applyAction(QJsonObject &data, const QJsonObject &action);

/// 稳定序列化（键序确定，用于快照 md5）
QByteArray canonicalJson(const QJsonObject &listData);

} // namespace LxListMerge
} // namespace Muyun
