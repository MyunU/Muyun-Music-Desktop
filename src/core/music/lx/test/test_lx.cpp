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
    const QString quality = (argc > 4) ? QString::fromUtf8(argv[4]) : QStringLiteral("320k");

    LOG("[1] 创建引擎\n");
    Muyun::LxScriptEngine engine;

    LOG("[2] 加载脚本: %s\n", scriptPath.toUtf8().constData());
    QString err;
    // 模拟 1.1.5 的异步加载：MUYUN_SMOKE_WORKER_LOAD=1 时在 worker 线程执行 loadScript
    // （QuickJS runtime 在主线程构造、worker 线程里 Free+重建 = 跨线程 UB 验证）
    const bool workerLoad = qEnvironmentVariableIsSet("MUYUN_SMOKE_WORKER_LOAD");
    if (workerLoad) {
        LOG("[2w] worker 线程加载（模拟 1.1.5 异步）\n");
        // 用独立线程池强制在非主线程执行（与后续 musicUrl 线程池分离，确保不同线程）
        QThreadPool poolA;
        poolA.setMaxThreadCount(1);
        auto fut = QtConcurrent::run(&poolA, [&engine, scriptPath]() {
            QString e;
            return engine.loadScript(scriptPath, &e) ? QString() : e;
        });
        err = fut.result();
        if (!err.isEmpty()) {
            LOG("[FAIL] 脚本加载失败: %s\n", err.toUtf8().constData());
            return 1;
        }
    } else {
        if (!engine.loadScript(scriptPath, &err)) {
            LOG("[FAIL] 脚本加载失败: %s\n", err.toUtf8().constData());
            return 1;
        }
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
        LOG("[3c] 过滤后 sources=%s tx.qualitys=%s kw.qualitys=%s\n",
            QStringList(sources.keys()).join(QLatin1Char(',')).toUtf8().constData(),
            engine.declaredQualitys(QStringLiteral("tx")).join(QLatin1Char(',')).toUtf8().constData(),
            engine.declaredQualitys(QStringLiteral("kw")).join(QLatin1Char(',')).toUtf8().constData());
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
    // 可选第 5 参数 hash（kg 源必须）
    if (argc > 5)
        info[QStringLiteral("hash")] = QString::fromUtf8(argv[5]);

    // 跨线程验证：MUYUN_SMOKE_CROSS_THREAD=1 时，musicUrl 在**主线程**调用
    // （loadScript 已在 worker 线程执行）→ QuickJS runtime 跨线程使用（真实 UB 场景）。
    // 默认在工作线程调用（与 loadScript 可能同/异线程，碰运气）。
    const bool crossThread = qEnvironmentVariableIsSet("MUYUN_SMOKE_CROSS_THREAD");
    LOG("[4] %s线程调用 musicUrl(%s, %s, %s) —— 模拟播放线程\n",
        crossThread ? "主" : "工作",
        source.toUtf8().constData(), songmid.toUtf8().constData(),
        quality.toUtf8().constData());
    QString u, e;
    if (crossThread) {
        u = engine.musicUrl(source, info, quality, &e);
    } else {
        // 独立线程池执行 musicUrl：与 loadScript 的 poolA 必然不同线程（真实 1.1.5 场景）
        QThreadPool poolB;
        poolB.setMaxThreadCount(1);
        auto fut = QtConcurrent::run(&poolB, [&engine, source, info, quality]() {
            QString e2;
            QString u2 = engine.musicUrl(source, info, quality, &e2);
            return qMakePair(u2, e2);
        });
        const auto r = fut.result();
        u = r.first;
        e = r.second;
    }
    LOG("[5] musicUrl 返回 url='%s' err='%s'\n",
        u.toUtf8().constData(), e.toUtf8().constData());

    if (u.isEmpty()) { LOG("[FAIL] 空链接\n"); return 2; }
    LOG("[PASS] url=%s\n", u.toUtf8().constData());
    if (protoProblems > 0) { LOG("[FAIL] 协议断言未通过 %d 项\n", protoProblems); return 3; }
    return 0;
}
