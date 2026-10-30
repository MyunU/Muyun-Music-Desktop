#pragma once

// 取窗口 HWND 的安全封装 —— **绝不在 nativeEventFilter 里用 QWindow::winId() 裸调用**。
//
// 背景（v1.4.0 实测事故）：
// QWindow::winId() 在"平台窗已经被销毁"时会把窗口重新创建出来（Qt 内部走
// QGuiApplication::platformIntegration()->createPlatformWindow）。而创建过程本身要
// SetWindowLongPtr 改样式 → 同步派发 WM_STYLECHANGING / WM_NCCALCSIZE 回到我们自己装的
// QAbstractNativeEventFilter → 过滤器里再取一次 winId() → 再次创建 → 无限递归。
//
// 退出关窗时（主窗已销毁、事件循环还在跑）正好踩中这条：26 秒内重建了 2651 次原生窗口，
// 用户看到的就是"退出要老半天"；最后异常终止，WER 报
//   c000041d STATUS_FATAL_USER_CALLBACK_EXCEPTION
// 弹窗 "MuyunMusic has stopped working / Error launching CrashSender.exe"。
//
// 所以过滤器/定时器里取 HWND 一律用这里：先看 handle()（只读，不创建），没有平台窗就直接
// 返回 nullptr，让调用方按"不是我的窗口"放行。

#include <QWindow>

#ifdef Q_OS_WIN
#include <windows.h>

namespace Muyun {

/// 当前 HWND；平台窗不存在时返回 nullptr，**不会顺手把窗口创建出来**。
inline HWND hwndWithoutCreating(QWindow *w)
{
    if (!w) return nullptr;
    if (!w->handle()) return nullptr;   // 平台窗已销毁 → 绝不能碰 winId()
    return reinterpret_cast<HWND>(w->winId());
}

} // namespace Muyun

#endif // Q_OS_WIN
