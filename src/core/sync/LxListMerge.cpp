#include "LxListMerge.h"

#include <QJsonDocument>
#include <QSet>
#include <QHash>

namespace Muyun {
namespace LxListMerge {

namespace {

QJsonArray arr(const QJsonObject &o, const QString &k)
{
    return o.value(k).toArray();
}

QString musicId(const QJsonValue &v, bool *ok = nullptr)
{
    const QString id = v.toObject().value(QStringLiteral("id")).toString();
    if (ok) *ok = !id.isEmpty();
    return id;
}

QString listId(const QJsonValue &v)
{
    return v.toObject().value(QStringLiteral("id")).toString();
}

/// 并集（bottom 语义）：source 在前 + target 在后，按 id 去重（先到者胜）
QJsonArray unionLists(const QJsonArray &source, const QJsonArray &target)
{
    QJsonArray out;
    QSet<QString> seen;
    for (const QJsonValue &v : source) {
        const QString id = musicId(v);
        if (!seen.contains(id)) { seen.insert(id); out.append(v); }
    }
    for (const QJsonValue &v : target) {
        const QString id = musicId(v);
        if (!seen.contains(id)) { seen.insert(id); out.append(v); }
    }
    return out;
}

/// 三方合并歌曲数组：snapshot 有而 local/remote 任一侧没有 → 视为被删除
QJsonArray unionWithSnapshot(const QJsonArray &local, const QJsonArray &remote,
                             const QJsonArray &snapshot)
{
    QSet<QString> localIds, remoteIds, removed;
    for (const QJsonValue &v : local) localIds.insert(musicId(v));
    for (const QJsonValue &v : remote) remoteIds.insert(musicId(v));
    for (const QJsonValue &v : snapshot) {
        const QString id = musicId(v);
        if (!localIds.contains(id) || !remoteIds.contains(id)) removed.insert(id);
    }
    QJsonArray out;
    QSet<QString> seen;
    for (const QJsonValue &v : unionLists(local, remote)) {
        const QString id = musicId(v);
        if (removed.contains(id) || seen.contains(id)) continue;
        seen.insert(id);
        out.append(v);
    }
    return out;
}

QJsonObject buildUserList(const QJsonValue &v, const QJsonArray *listOverride = nullptr)
{
    QJsonObject o = v.toObject();
    if (listOverride) o[QStringLiteral("list")] = *listOverride;
    if (!o.contains(QStringLiteral("source"))) o[QStringLiteral("source")] = QString();
    if (!o.contains(QStringLiteral("sourceListId"))) o[QStringLiteral("sourceListId")] = QString();
    if (!o.contains(QStringLiteral("locationUpdateTime"))) o[QStringLiteral("locationUpdateTime")] = 0;
    return o;
}

QHash<QString, int> indexById(const QJsonArray &userList)
{
    QHash<QString, int> idx;
    for (int i = 0; i < userList.size(); ++i) idx.insert(listId(userList.at(i)), i);
    return idx;
}

qint64 locUpdateTime(const QJsonValue &v)
{
    return qint64(v.toObject().value(QStringLiteral("locationUpdateTime")).toDouble());
}

QJsonValue selectData(const QJsonValue &snapshot, const QJsonValue &local, const QJsonValue &remote)
{
    return snapshot == local ? remote : local;
}

} // namespace

bool isEmpty(const QJsonObject &listData)
{
    return arr(listData, QStringLiteral("defaultList")).isEmpty()
        && arr(listData, QStringLiteral("loveList")).isEmpty()
        && arr(listData, QStringLiteral("userList")).isEmpty();
}

QJsonArray mergeMusicList(const QJsonArray &source, const QJsonArray &target)
{
    return unionLists(source, target);
}

QJsonObject mergeData(const QJsonObject &source, const QJsonObject &target)
{
    QJsonObject out;
    out[QStringLiteral("defaultList")] =
        mergeMusicList(arr(source, QStringLiteral("defaultList")),
                       arr(target, QStringLiteral("defaultList")));
    out[QStringLiteral("loveList")] =
        mergeMusicList(arr(source, QStringLiteral("loveList")),
                       arr(target, QStringLiteral("loveList")));

    QJsonArray userList = source.value(QStringLiteral("userList")).toArray();
    for (const QJsonValue &tList : target.value(QStringLiteral("userList")).toArray()) {
        const QString tid = listId(tList);
        const int selfIdx = indexById(userList).value(tid, -1);
        if (selfIdx >= 0) {
            // 同 id 歌单：歌曲并集；仅当 target 位置更新更早才换位（洛雪语义）
            QJsonObject merged = buildUserList(userList.at(selfIdx));
            merged[QStringLiteral("list")] = mergeMusicList(
                arr(userList.at(selfIdx).toObject(), QStringLiteral("list")),
                arr(tList.toObject(), QStringLiteral("list")));
            const qint64 tUpd = locUpdateTime(tList);
            const qint64 sUpd = locUpdateTime(userList.at(selfIdx));
            if (tUpd < sUpd) {
                merged[QStringLiteral("locationUpdateTime")] = double(tUpd);
                userList.removeAt(selfIdx);
                userList.append(merged);
            } else {
                userList.replace(selfIdx, merged);
            }
        } else if (locUpdateTime(tList)) {
            userList.append(buildUserList(tList));
        } else {
            userList.append(buildUserList(tList));
        }
    }
    out[QStringLiteral("userList")] = userList;
    return out;
}

QJsonObject overwriteData(const QJsonObject &source, const QJsonObject &target)
{
    QJsonObject out;
    out[QStringLiteral("defaultList")] = arr(source, QStringLiteral("defaultList"));
    out[QStringLiteral("loveList")] = arr(source, QStringLiteral("loveList"));

    QJsonArray userList = source.value(QStringLiteral("userList")).toArray();
    QSet<QString> sourceIds;
    for (const QJsonValue &v : userList) sourceIds.insert(listId(v));
    for (const QJsonValue &tList : target.value(QStringLiteral("userList")).toArray()) {
        if (sourceIds.contains(listId(tList))) continue;
        userList.append(buildUserList(tList));
    }
    out[QStringLiteral("userList")] = userList;
    return out;
}

QJsonObject mergeFromSnapshot(const QJsonObject &local, const QJsonObject &remote,
                              const QJsonObject &snapshot)
{
    QJsonObject out;
    out[QStringLiteral("defaultList")] = unionWithSnapshot(
        arr(local, QStringLiteral("defaultList")),
        arr(remote, QStringLiteral("defaultList")),
        arr(snapshot, QStringLiteral("defaultList")));
    out[QStringLiteral("loveList")] = unionWithSnapshot(
        arr(local, QStringLiteral("loveList")),
        arr(remote, QStringLiteral("loveList")),
        arr(snapshot, QStringLiteral("loveList")));

    QHash<QString, int> localIdx = indexById(arr(local, QStringLiteral("userList")));
    QHash<QString, int> remoteIdx = indexById(arr(remote, QStringLiteral("userList")));
    QJsonArray snapUser = arr(snapshot, QStringLiteral("userList"));
    QHash<QString, int> snapIdx = indexById(snapUser);

    QSet<QString> removedLists;
    for (const QJsonValue &v : snapUser) {
        const QString id = listId(v);
        if (!localIdx.contains(id) || !remoteIdx.contains(id)) removedLists.insert(id);
    }

    QJsonArray newUserList;
    QHash<QString, int> newIdx;
    for (const QJsonValue &lv : arr(local, QStringLiteral("userList"))) {
        const QString lid = listId(lv);
        if (removedLists.contains(lid)) continue;
        const int rIdx = remoteIdx.value(lid, -1);
        if (rIdx < 0) {
            newIdx.insert(lid, newUserList.size());
            newUserList.append(buildUserList(lv));
            continue;
        }
        const QJsonValue rv = remote.value(QStringLiteral("userList")).toArray().at(rIdx);
        const int sIdx = snapIdx.value(lid, -1);
        const QJsonValue sv = sIdx >= 0 ? snapUser.at(sIdx) : QJsonValue();
        const QJsonArray snapMusics = sIdx >= 0 ? arr(sv.toObject(), QStringLiteral("list")) : QJsonArray();
        QJsonObject nl = buildUserList(lv);
        nl[QStringLiteral("name")] = selectData(
            sIdx >= 0 ? sv.toObject().value(QStringLiteral("name")) : QJsonValue(),
            lv.toObject().value(QStringLiteral("name")),
            rv.toObject().value(QStringLiteral("name")));
        nl[QStringLiteral("source")] = selectData(
            sIdx >= 0 ? sv.toObject().value(QStringLiteral("source")) : QJsonValue(),
            lv.toObject().value(QStringLiteral("source")),
            rv.toObject().value(QStringLiteral("source")));
        nl[QStringLiteral("sourceListId")] = selectData(
            sIdx >= 0 ? sv.toObject().value(QStringLiteral("sourceListId")) : QJsonValue(),
            lv.toObject().value(QStringLiteral("sourceListId")),
            rv.toObject().value(QStringLiteral("sourceListId")));
        nl[QStringLiteral("list")] = unionWithSnapshot(
            arr(lv.toObject(), QStringLiteral("list")),
            arr(rv.toObject(), QStringLiteral("list")),
            snapMusics);
        newIdx.insert(lid, newUserList.size());
        newUserList.append(nl);
    }
    const QJsonArray remoteUser = arr(remote, QStringLiteral("userList"));
    for (int i = 0; i < remoteUser.size(); ++i) {
        const QJsonValue rv = remoteUser.at(i);
        const QString rid = listId(rv);
        if (removedLists.contains(rid)) continue;
        const qint64 rUpd = locUpdateTime(rv);
        if (newIdx.contains(rid)) {
            const qint64 lUpd = locUpdateTime(localIdx.contains(rid)
                                                  ? arr(local, QStringLiteral("userList")).at(localIdx.value(rid))
                                                  : QJsonValue());
            if (lUpd >= rUpd) continue;
            // target 更新更早 → 移到该位置（洛雪语义；暮云侧多为 0，很少触发）
            const int cur = newIdx.value(rid);
            QJsonValue moved = newUserList.at(cur);
            newUserList.removeAt(cur);
            newUserList.insert(qMin(i, newUserList.size()), moved);
        } else {
            newUserList.insert(rUpd ? qMin(i, newUserList.size()) : newUserList.size(),
                               buildUserList(rv));
        }
    }
    out[QStringLiteral("userList")] = newUserList;
    return out;
}

// ---------------------------------------------------------------------------
// action 应用（handleRemoteListAction 的 JSON 版）
// ---------------------------------------------------------------------------

namespace {

/// 找到 listId 对应的歌曲数组容器；返回 userList 下标（-1=default/love 特殊处理）
struct ListRef { int userIndex = -1; QString special; bool found = true; };

ListRef resolveList(QJsonObject &data, const QString &lid)
{
    if (lid == QLatin1String("default")) return {-1, QStringLiteral("defaultList"), true};
    if (lid == QLatin1String("love"))    return {-1, QStringLiteral("loveList"), true};
    QJsonArray ul = arr(data, QStringLiteral("userList"));
    for (int i = 0; i < ul.size(); ++i)
        if (listId(ul.at(i)) == lid) return {i, QString(), true};
    return {-1, QString(), false};
}

QJsonArray getMusics(const QJsonObject &data, const ListRef &ref)
{
    if (ref.userIndex < 0) return arr(data, ref.special);
    return arr(data.value(QStringLiteral("userList")).toArray().at(ref.userIndex).toObject(),
               QStringLiteral("list"));
}

void setMusics(QJsonObject &data, const ListRef &ref, const QJsonArray &musics)
{
    if (ref.userIndex < 0) { data[ref.special] = musics; return; }
    QJsonArray ul = data.value(QStringLiteral("userList")).toArray();
    QJsonObject l = ul.at(ref.userIndex).toObject();
    l[QStringLiteral("list")] = musics;
    ul.replace(ref.userIndex, l);
    data[QStringLiteral("userList")] = ul;
}

void removeMusicsById(QJsonArray &musics, const QSet<QString> &ids)
{
    QJsonArray kept;
    for (const QJsonValue &v : musics)
        if (!ids.contains(musicId(v))) kept.append(v);
    musics = kept;
}

} // namespace

bool applyAction(QJsonObject &data, const QJsonObject &action)
{
    const QString type = action.value(QStringLiteral("action")).toString();
    const QJsonValue payload = action.value(QStringLiteral("data"));

    if (type == QLatin1String("list_data_overwrite")) {
        const QJsonObject d = payload.toObject();
        data[QStringLiteral("defaultList")] = arr(d, QStringLiteral("defaultList"));
        data[QStringLiteral("loveList")] = arr(d, QStringLiteral("loveList"));
        QJsonArray ul = d.value(QStringLiteral("userList")).toArray();
        QJsonArray fixed;
        for (const QJsonValue &v : ul) fixed.append(buildUserList(v));
        data[QStringLiteral("userList")] = fixed;
        return true;
    }
    if (type == QLatin1String("list_create")) {
        const QJsonObject d = payload.toObject();
        QJsonArray ul = arr(data, QStringLiteral("userList"));
        int pos = qBound(0, d.value(QStringLiteral("position")).toInt(), ul.size());
        for (const QJsonValue &info : d.value(QStringLiteral("listInfos")).toArray()) {
            QJsonObject nl = buildUserList(info);
            nl[QStringLiteral("list")] = QJsonArray();
            ul.insert(pos++, nl);
        }
        data[QStringLiteral("userList")] = ul;
        return true;
    }
    if (type == QLatin1String("list_remove")) {
        QSet<QString> ids;
        for (const QJsonValue &v : payload.toArray()) ids.insert(v.toString());
        QJsonArray ul = arr(data, QStringLiteral("userList")), kept;
        for (const QJsonValue &v : ul)
            if (!ids.contains(listId(v))) kept.append(v);
        data[QStringLiteral("userList")] = kept;
        return true;
    }
    if (type == QLatin1String("list_update")) {
        QJsonArray ul = arr(data, QStringLiteral("userList"));
        for (const QJsonValue &u : payload.toArray()) {
            const QString uid = listId(u);
            for (int i = 0; i < ul.size(); ++i) {
                if (listId(ul.at(i)) != uid) continue;
                QJsonObject old = ul.at(i).toObject();
                QJsonObject nu = u.toObject();
                nu[QStringLiteral("list")] = arr(old, QStringLiteral("list"));
                ul.replace(i, nu);
                break;
            }
        }
        data[QStringLiteral("userList")] = ul;
        return true;
    }
    if (type == QLatin1String("list_update_position")) {
        const QJsonObject d = payload.toObject();
        const int pos = d.value(QStringLiteral("position")).toInt();
        QJsonArray ids;
        for (const QJsonValue &v : d.value(QStringLiteral("ids")).toArray()) ids.append(v);
        QJsonArray ul = arr(data, QStringLiteral("userList"));
        for (int i = ids.size() - 1; i >= 0; --i) {
            const QString id = ids.at(i).toString();
            for (int j = 0; j < ul.size(); ++j) {
                if (listId(ul.at(j)) != id) continue;
                QJsonValue mv = ul.takeAt(j);
                ul.insert(qBound(0, pos, ul.size()), mv);
                break;
            }
        }
        data[QStringLiteral("userList")] = ul;
        return true;
    }
    if (type == QLatin1String("list_music_overwrite")) {
        const QJsonObject d = payload.toObject();
        const ListRef ref = resolveList(data, d.value(QStringLiteral("listId")).toString());
        if (!ref.found) return true;   // 目标不存在（如 tempList）→ 静默跳过
        setMusics(data, ref, d.value(QStringLiteral("musicInfos")).toArray());
        return true;
    }
    if (type == QLatin1String("list_music_add")) {
        const QJsonObject d = payload.toObject();
        const ListRef ref = resolveList(data, d.value(QStringLiteral("id")).toString());
        if (!ref.found) return true;
        const bool top = d.value(QStringLiteral("addMusicLocationType")).toString() == QLatin1String("top");
        QJsonArray musics = getMusics(data, ref);
        QSet<QString> existing;
        for (const QJsonValue &v : musics) existing.insert(musicId(v));
        const QJsonArray src = d.value(QStringLiteral("musicInfos")).toArray();
        if (top) {
            for (int i = src.size() - 1; i >= 0; --i) {
                const QJsonValue v = src.at(i);
                const QString id = musicId(v);
                if (id.isEmpty() || existing.contains(id)) continue;
                existing.insert(id);
                musics.prepend(v);
            }
        } else {
            for (const QJsonValue &v : src) {
                const QString id = musicId(v);
                if (id.isEmpty() || existing.contains(id)) continue;
                existing.insert(id);
                musics.append(v);
            }
        }
        setMusics(data, ref, musics);
        return true;
    }
    if (type == QLatin1String("list_music_move")) {
        const QJsonObject d = payload.toObject();
        QSet<QString> moving;
        for (const QJsonValue &v : d.value(QStringLiteral("musicInfos")).toArray())
            moving.insert(musicId(v));
        const ListRef from = resolveList(data, d.value(QStringLiteral("fromId")).toString());
        if (from.found) {
            QJsonArray musics = getMusics(data, from);
            removeMusicsById(musics, moving);
            setMusics(data, from, musics);
        }
        const ListRef to = resolveList(data, d.value(QStringLiteral("toId")).toString());
        if (to.found) {
            const bool top = d.value(QStringLiteral("addMusicLocationType")).toString() == QLatin1String("top");
            QJsonArray musics = getMusics(data, to);
            QJsonArray add;
            for (const QJsonValue &v : d.value(QStringLiteral("musicInfos")).toArray())
                add.append(v);
            musics = top ? (add + musics) : (musics + add);
            setMusics(data, to, musics);
        }
        return true;
    }
    if (type == QLatin1String("list_music_remove")) {
        const QJsonObject d = payload.toObject();
        const ListRef ref = resolveList(data, d.value(QStringLiteral("listId")).toString());
        if (!ref.found) return true;
        QJsonArray musics = getMusics(data, ref);
        QSet<QString> ids;
        for (const QJsonValue &v : d.value(QStringLiteral("ids")).toArray()) ids.insert(v.toString());
        removeMusicsById(musics, ids);
        setMusics(data, ref, musics);
        return true;
    }
    if (type == QLatin1String("list_music_update")) {
        for (const QJsonValue &item : payload.toArray()) {
            const QJsonObject d = item.toObject();
            const ListRef ref = resolveList(data, d.value(QStringLiteral("id")).toString());
            if (!ref.found) continue;
            const QJsonValue mi = d.value(QStringLiteral("musicInfo"));
            const QString mid = musicId(mi);
            QJsonArray musics = getMusics(data, ref);
            for (int i = 0; i < musics.size(); ++i) {
                if (musicId(musics.at(i)) == mid) { musics.replace(i, mi); break; }
            }
            setMusics(data, ref, musics);
        }
        return true;
    }
    if (type == QLatin1String("list_music_update_position")) {
        const QJsonObject d = payload.toObject();
        const ListRef ref = resolveList(data, d.value(QStringLiteral("listId")).toString());
        if (!ref.found) return true;
        QJsonArray musics = getMusics(data, ref);
        for (const QJsonValue &idv : d.value(QStringLiteral("ids")).toArray()) {
            const QString id = idv.toString();
            for (int j = 0; j < musics.size(); ++j) {
                if (musicId(musics.at(j)) != id) continue;
                QJsonValue mv = musics.takeAt(j);
                musics.insert(qBound(0, d.value(QStringLiteral("position")).toInt(), musics.size()), mv);
                break;
            }
        }
        setMusics(data, ref, musics);
        return true;
    }
    if (type == QLatin1String("list_music_clear")) {
        QSet<QString> ids;
        for (const QJsonValue &v : payload.toArray()) ids.insert(v.toString());
        auto clean = [&](const QString &key) {
            QJsonArray m = arr(data, key);
            removeMusicsById(m, ids);
            data[key] = m;
        };
        clean(QStringLiteral("defaultList"));
        clean(QStringLiteral("loveList"));
        QJsonArray ul = arr(data, QStringLiteral("userList"));
        for (int i = 0; i < ul.size(); ++i) {
            QJsonObject l = ul.at(i).toObject();
            QJsonArray m = arr(l, QStringLiteral("list"));
            removeMusicsById(m, ids);
            l[QStringLiteral("list")] = m;
            ul.replace(i, l);
        }
        data[QStringLiteral("userList")] = ul;
        return true;
    }
    return false;   // 未知 action
}

QByteArray canonicalJson(const QJsonObject &listData)
{
    return QJsonDocument(listData).toJson(QJsonDocument::Compact);
}

} // namespace LxListMerge
} // namespace Muyun
