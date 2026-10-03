// LX 脚本引擎测试（控制台程序，Qt Core/Network）
// 用法：lx_smoke_test [脚本路径] [source] [songmid]
#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QtConcurrent>
#include <cstdio>

#include "core/music/lx/LxScriptEngine.h"

#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while(0)

int main(int argc, char *argv[])
{
    setvbuf(stderr, nullptr, _IONBF, 0);
    QCoreApplication app(argc, argv);

    QString scriptPath = (argc > 1) ? QString::fromUtf8(argv[1])
                                    : QStringLiteral("lx_smoke.js");
    if (!QFileInfo::exists(scriptPath))
        scriptPath = QCoreApplication::applicationDirPath()
                     + QStringLiteral("/lx_smoke.js");

    const QString source = (argc > 2) ? QString::fromUtf8(argv[2]) : QStringLiteral("tx");
    const QString songmid = (argc > 3) ? QString::fromUtf8(argv[3]) : QStringLiteral("003Qui1q2u1Zho");

    LOG("[1] 创建引擎\n");
    Muyun::LxScriptEngine engine;

    LOG("[2] 加载脚本: %s\n", scriptPath.toUtf8().constData());
    QString err;
    if (!engine.loadScript(scriptPath, &err)) {
        LOG("[FAIL] 脚本加载失败: %s\n", err.toUtf8().constData());
        return 1;
    }
    LOG("[3] 加载成功 inited=%d name=%s\n", engine.inited() ? 1 : 0,
        engine.scriptInfo().value("name").toString().toUtf8().constData());

    // 协议一致性脚本会把断言结果塞进 inited 的 __protocolReport，这里原样打出来
    int protoProblems = 0;
    {
        const QVariantMap info = engine.scriptInfo();
        const QVariantList problems = info.value(QStringLiteral("__protocolReport")).toList();
        const int checks = info.value(QStringLiteral("__protocolChecks")).toInt();
        if (checks > 0) {
            protoProblems = int(problems.size());
            LOG("[3a] 协议断言 %d 项，未通过 %d 项\n", checks, int(problems.size()));
            for (const QVariant &p : problems)
                LOG("     [FAIL] %s\n", p.toString().toUtf8().constData());
        }
        const QVariantMap sources = info.value(QStringLiteral("sources")).toMap();
        LOG("[3c] 过滤后 sources=%s tx.qualitys=%s\n",
            QStringList(sources.keys()).join(QLatin1Char(',')).toUtf8().constData(),
            engine.declaredQualitys(QStringLiteral("tx")).join(QLatin1Char(',')).toUtf8().constData());
    }

    // 二次加载同一脚本（模拟用户切换音源再切回——修复前会报 redeclaration）
    if (!engine.loadScript(scriptPath, &err)) {
        LOG("[FAIL] 二次加载失败: %s\n", err.toUtf8().constData());
        return 1;
    }
    LOG("[3b] 二次加载成功（无 redeclaration）\n");

    QVariantMap info;
    info[QStringLiteral("source")] = source;
    info[QStringLiteral("songmid")] = songmid;
    info[QStringLiteral("songId")] = songmid;
    info[QStringLiteral("strMediaMid")] = songmid;

    LOG("[4] 在工作线程调用 musicUrl(%s, %s, 320k) —— 模拟播放线程\n",
        source.toUtf8().constData(), songmid.toUtf8().constData());
    auto fut = QtConcurrent::run([&engine, source, info, songmid]() {
        QString e;
        QString u = engine.musicUrl(source, info, QStringLiteral("320k"), &e);
        return qMakePair(u, e);
    });
    const auto r = fut.result();
    LOG("[5] musicUrl 返回 url='%s' err='%s'\n",
        r.first.toUtf8().constData(), r.second.toUtf8().constData());

    if (r.first.isEmpty()) { LOG("[FAIL] 空链接\n"); return 2; }
    LOG("[PASS] url=%s\n", r.first.toUtf8().constData());
    if (protoProblems > 0) { LOG("[FAIL] 协议断言未通过 %d 项\n", protoProblems); return 3; }
    return 0;
}
