#include "MediaSession.h"

#include <QFileInfo>
#include <QMetaObject>
#include <QTimer>
#include <QUrl>
#include <QWindow>

#include <atomic>
#include <cstdlib>
#include <cwchar>

#include "PlayerController.h"
#include "WindowHwnd.h"
#include "core/Types.h"

#if defined(Q_OS_WIN)
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>
#include <eventtoken.h>
#endif

#ifdef MUYUN_SELFTES
#include <cstdio>
#define MSLOG(...) do { fprintf(stderr, "[smtc] " __VA_ARGS__); fflush(stderr); } while (0)
#else
#define MSLOG(...) do { } while (0)
#endif

namespace Muyun {

#if defined(Q_OS_WIN)

namespace {

// ===========================================================================
// WinRT ABI 声明（手写）
//
// 与 Windows SDK 的 windows.media.h / windows.storage.streams.h /
// windows.foundation.h 逐字节同构：WinRT 的**类/接口**都 IInspectable 派生、vtable 顺序
// 严格按 IDL 方法顺序排列；但**事件 handler（TypedEventHandler）是 IUnknown 派生**，
// 见下方 delegate 一节。GUID 直接取自系统 IDL（同一套 ABI 在 Win8~Win11 稳定）。
//
// 只声明到实际用到的最后一个方法为止：vtable 槽位对齐即可安全调用，
// 不用的尾部方法不声明（不引入任何 SDK 依赖）。
// ===========================================================================

/// WinRT `boolean` 是 1 字节（ABI），不是 C++ bool
using WinBool = unsigned char;

/// Windows.Foundation.TimeSpan：100ns 为单位
struct WinTimeSpan {
    INT64 Duration;
};

/// SMTC 播放状态（MediaPlaybackStatus）
enum : int {
    kPlaybackClosed = 0,
    kPlaybackChanging = 1,
    kPlaybackStopped = 2,
    kPlaybackPlaying = 3,
    kPlaybackPaused = 4
};

/// 媒体类型（MediaPlaybackType）
enum : int { kMediaTypeUnknown = 0, kMediaTypeMusic = 1, kMediaTypeVideo = 2, kMediaTypeImage = 3 };

/// 系统面板按钮（SystemMediaTransportControlsButton）
enum : int {
    kBtnPlay = 0,
    kBtnPause = 1,
    kBtnStop = 2,
    kBtnRecord = 3,
    kBtnFastForward = 4,
    kBtnRewind = 5,
    kBtnNext = 6,
    kBtnPrevious = 7,
    kBtnChannelUp = 8,
    kBtnChannelDown = 9
};

struct ISystemMediaTransportControls;
struct ISystemMediaTransportControlsDisplayUpdater;
struct IMusicDisplayProperties;
struct ISystemMediaTransportControlsButtonPressedEventArgs;
struct ISystemMediaTransportControlsPlaybackPositionChangeRequestedEventArgs;
struct ISystemMediaTransportControlsTimelineProperties;
struct ISystemMediaTransportControlsInterop;

/// 只借 Release() 用，方法体不必声明
struct IRandomAccessStreamReference : IInspectable {};
struct IUriRuntimeClass : IInspectable {};

/// ISystemMediaTransportControlsInterop：把 SMTC 绑到指定 HWND 的入口
/// IID {ddb0472d-c911-4a1f-86d9-dc3d71a95f5a}
struct ISystemMediaTransportControlsInterop : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE GetForWindow(HWND appWindow, REFIID riid,
                                                   void **mediaTransportControl) = 0;
};

/// ISystemMediaTransportControls
/// IID {99fa3ff4-1742-42a6-902e-087d41f965ec}
struct ISystemMediaTransportControls : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackStatus(int *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_PlaybackStatus(int value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DisplayUpdater(
        ISystemMediaTransportControlsDisplayUpdater **value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_SoundLevel(int *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsEnabled(WinBool *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsEnabled(WinBool value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPlayEnabled(WinBool *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsPlayEnabled(WinBool value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsStopEnabled(WinBool *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsStopEnabled(WinBool value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPauseEnabled(WinBool *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsPauseEnabled(WinBool value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRecordEnabled(WinBool *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsRecordEnabled(WinBool value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsFastForwardEnabled(WinBool *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsFastForwardEnabled(WinBool value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRewindEnabled(WinBool *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsRewindEnabled(WinBool value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPreviousEnabled(WinBool *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsPreviousEnabled(WinBool value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsNextEnabled(WinBool *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsNextEnabled(WinBool value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsChannelUpEnabled(WinBool *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsChannelUpEnabled(WinBool value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsChannelDownEnabled(WinBool *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsChannelDownEnabled(WinBool value) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_ButtonPressed(IUnknown *handler,
                                                       EventRegistrationToken *token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_ButtonPressed(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_PropertyChanged(IInspectable *handler,
                                                          EventRegistrationToken *token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_PropertyChanged(EventRegistrationToken token) = 0;
};

/// ISystemMediaTransportControlsDisplayUpdater
/// IID {8abbc53e-fa55-4ecf-ad8e-c984e5dd1550}
struct ISystemMediaTransportControlsDisplayUpdater : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Type(int *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Type(int value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AppMediaId(HSTRING *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AppMediaId(HSTRING value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Thumbnail(IRandomAccessStreamReference **value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Thumbnail(IRandomAccessStreamReference *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MusicProperties(IMusicDisplayProperties **value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_VideoProperties(IInspectable **value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ImageProperties(IInspectable **value) = 0;
    /// 未调用；占位保持槽位对齐
    virtual HRESULT STDMETHODCALLTYPE CopyFromFileAsync(int type, HSTRING path,
                                                        IInspectable **operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearAll() = 0;
    virtual HRESULT STDMETHODCALLTYPE Update() = 0;
};

/// IMusicDisplayProperties（v1：标题/专辑歌手/歌手）
/// IID {6bbf0c59-d0a0-4d26-92a0-f978e1d18e7b}
struct IMusicDisplayProperties : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Title(HSTRING *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Title(HSTRING value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumArtist(HSTRING *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AlbumArtist(HSTRING value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Artist(HSTRING *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Artist(HSTRING value) = 0;
};

/// ISystemMediaTransportControlsButtonPressedEventArgs
/// IID {b7f47116-a56f-4dc8-9e11-92031f4a87c2}
struct ISystemMediaTransportControlsButtonPressedEventArgs : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Button(int *value) = 0;
};

/// IPlaybackPositionChangeRequestedEventArgs
/// IID {b4493f88-eb28-4961-9c14-335e44f3e125}
struct ISystemMediaTransportControlsPlaybackPositionChangeRequestedEventArgs : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_RequestedPlaybackPosition(WinTimeSpan *value) = 0;
};

/// ISystemMediaTransportControlsTimelineProperties
/// IID {5125316a-c3a2-475b-8507-93534dc88f15}
struct ISystemMediaTransportControlsTimelineProperties : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_StartTime(WinTimeSpan *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_StartTime(WinTimeSpan value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_EndTime(WinTimeSpan *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_EndTime(WinTimeSpan value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MinSeekTime(WinTimeSpan *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_MinSeekTime(WinTimeSpan value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MaxSeekTime(WinTimeSpan *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_MaxSeekTime(WinTimeSpan value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Position(WinTimeSpan *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Position(WinTimeSpan value) = 0;
};

/// ISystemMediaTransportControls2（时间轴 + 拖动进度事件）
/// IID {ea98d2f6-7f3c-4af2-a586-72889808efb1}
struct ISystemMediaTransportControls2 : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_AutoRepeatMode(int *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AutoRepeatMode(int value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ShuffleEnabled(WinBool *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_ShuffleEnabled(WinBool value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackRate(double *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_PlaybackRate(double value) = 0;
    virtual HRESULT STDMETHODCALLTYPE UpdateTimelineProperties(
        ISystemMediaTransportControlsTimelineProperties *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_PlaybackPositionChangeRequested(
        IUnknown *handler, EventRegistrationToken *token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_PlaybackPositionChangeRequested(
        EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_PlaybackRateChangeRequested(IInspectable *handler,
                                                                     EventRegistrationToken *token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_PlaybackRateChangeRequested(
        EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_ShuffleEnabledChangeRequested(IInspectable *handler,
                                                                       EventRegistrationToken *token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_ShuffleEnabledChangeRequested(
        EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_AutoRepeatModeChangeRequested(IInspectable *handler,
                                                                       EventRegistrationToken *token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_AutoRepeatModeChangeRequested(
        EventRegistrationToken token) = 0;
};

/// IRandomAccessStreamReferenceStatics（封面：由 URI 直接给系统，省自己下图）
/// IID {857309dc-3fbf-4e7d-986f-ef3b1a07a964}
struct IRandomAccessStreamReferenceStatics : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE CreateFromFile(IInspectable *file,
                                                     IRandomAccessStreamReference **value) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateFromUri(IUriRuntimeClass *uri,
                                                    IRandomAccessStreamReference **value) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateFromStream(IInspectable *stream,
                                                       IRandomAccessStreamReference **value) = 0;
};

/// IUriRuntimeClassFactory（把 QString 的 URL 变成 WinRT Uri 对象）
/// IID {44a9796f-723e-4fdf-a218-033e75b0c084}
struct IUriRuntimeClassFactory : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE CreateUri(HSTRING uri, IUriRuntimeClass **instance) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateWithRelativeUri(HSTRING baseUri, HSTRING relativeUri,
                                                            IUriRuntimeClass **instance) = 0;
};

// ---------------------------------------------------------------------------
// 常量：IID / RuntimeClass 名
// ---------------------------------------------------------------------------

const GUID kIIDInterop = {0xddb0472d, 0xc911, 0x4a1f,
                          {0x86, 0xd9, 0xdc, 0x3d, 0x71, 0xa9, 0x5f, 0x5a}};
const GUID kIIDControls = {0x99fa3ff4, 0x1742, 0x42a6,
                           {0x90, 0x2e, 0x08, 0x7d, 0x41, 0xf9, 0x65, 0xec}};
const GUID kIIDControls2 = {0xea98d2f6, 0x7f3c, 0x4af2,
                            {0xa5, 0x86, 0x72, 0x88, 0x98, 0x08, 0xef, 0xb1}};
const GUID kIIDTimeline = {0x5125316a, 0xc3a2, 0x475b,
                           {0x85, 0x07, 0x93, 0x53, 0x4d, 0xc8, 0x8f, 0x15}};
const GUID kIIDRasStatics = {0x857309dc, 0x3fbf, 0x4e7d,
                             {0x98, 0x6f, 0xef, 0x3b, 0x1a, 0x07, 0xa9, 0x64}};
const GUID kIIDUriFactory = {0x44a9796f, 0x723e, 0x4fdf,
                             {0xa2, 0x18, 0x03, 0x3e, 0x75, 0xb0, 0xc0, 0x84}};
/// TypedEventHandler<SMTC, ButtonPressedEventArgs> 的 WinRT 生成 IID
const GUID kIIDButtonHandler = {0x0557e996, 0x7b23, 0x5bae,
                                {0xaa, 0x81, 0xea, 0x0d, 0x67, 0x11, 0x43, 0xa4}};
/// TypedEventHandler<SMTC, PlaybackPositionChangeRequestedEventArgs> 的 WinRT 生成 IID
const GUID kIIDSeekHandler = {0x44e34f15, 0xbdc0, 0x50a7,
                              {0xac, 0xe4, 0x39, 0xe9, 0x1f, 0xb7, 0x53, 0xf1}};
const GUID kIIDAgileObject = {0x94ea2b94, 0xe9cc, 0x49e0,
                              {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

const wchar_t *const kClassControls = L"Windows.Media.SystemMediaTransportControls";
const wchar_t *const kClassTimeline = L"Windows.Media.SystemMediaTransportControlsTimelineProperties";
const wchar_t *const kClassRasRef = L"Windows.Storage.Streams.RandomAccessStreamReference";
const wchar_t *const kClassUri = L"Windows.Foundation.Uri";

/// HSTRING 生命周期托管（WindowsCreateString + WindowsDeleteString）
class WinHstring
{
public:
    explicit WinHstring(const QString &s)
    {
        if (s.isEmpty()) return;
        WindowsCreateString(reinterpret_cast<const wchar_t *>(s.utf16()),
                            static_cast<UINT32>(s.size()), &m_h);
    }
    /// RuntimeClass 名之类的常量宽字符串（L"..."）
    explicit WinHstring(const wchar_t *s)
    {
        if (!s || !*s) return;
        WindowsCreateString(s, static_cast<UINT32>(wcslen(s)), &m_h);
    }
    ~WinHstring()
    {
        if (m_h) WindowsDeleteString(m_h);
    }
    WinHstring(const WinHstring &) = delete;
    WinHstring &operator=(const WinHstring &) = delete;
    HSTRING get() const { return m_h; }

private:
    HSTRING m_h = nullptr;
};

/// QML 层的 cover 可能是网络 URL / 本地文件 / qrc，统一成系统能取到的 URI
QString coverToUri(const QString &cover)
{
    if (cover.isEmpty()) return QString();
    if (cover.startsWith(QLatin1String("http://")) || cover.startsWith(QLatin1String("https://")))
        return cover;
    if (cover.startsWith(QLatin1String("file:")) || cover.startsWith(QLatin1String("qrc:")))
        return cover;
    if (QFileInfo::exists(cover)) return QUrl::fromLocalFile(cover).toString();
    return QString();
}

// ---------------------------------------------------------------------------
// 事件 Delegate：系统面板按钮 / 拖动进度
//
// ⚠ 最容易写错的一条：WinRT 的 TypedEventHandler<TSender,TArgs> 是 **IUnknown** 派生，
// 不是 IInspectable 派生。mingw 的 windows.foundation.h 里
// `__FITypedEventHandler_2_...Vtbl` 依次只有 QueryInterface / AddRef / Release / **Invoke（槽 3）**，
// 对象上根本不存在 GetIids / GetRuntimeClassName / GetTrustLevel 这三个方法。
//
// 早前误按 IInspectable 实现（Invoke 落在槽 6）的后果：MediaControl.dll 调槽 3 想调 Invoke，
// 实际打进我们的 GetIids，把 IID 数组指针写回它参数栈上的 sender/args 位置 →
// 崩在它自己体内（0xc0000409 / 0xc0000005，故障地址落在模块镜像末尾之外）；
// 让 GetIids 返回 E_NOTIMPL 时"只是没崩"，但 Invoke 永远等不到（面板按钮全失效）。
//
// Invoke 里只投递队列事件，绝不在系统线程上动播放器。
// ---------------------------------------------------------------------------

class ButtonPressedHandler : public IUnknown
{
public:
    explicit ButtonPressedHandler(MediaSession *owner) : m_owner(owner) {}
    /// 会话拆除时先摘掉 owner：系统侧可能还有一帧在飞的回调，别让它摸到已析构的会话
    void clearOwner() { m_owner = nullptr; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        // 只认 IUnknown / 本体 delegate IID / IAgileObject：声明敏捷，系统就不用做跨公寓封送
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, kIIDButtonHandler)
            || IsEqualIID(riid, kIIDAgileObject)) {
            *ppv = static_cast<IUnknown *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = --m_refs;
        if (left == 0) delete this;
        return left;
    }
    /// 槽位 3（IUnknown 之后第一格）：系统调这里通知按下了哪个面板按钮
    virtual HRESULT STDMETHODCALLTYPE Invoke(ISystemMediaTransportControls * /*sender*/,
                                             ISystemMediaTransportControlsButtonPressedEventArgs *args);

private:
    std::atomic<ULONG> m_refs{1};
    MediaSession *m_owner = nullptr;
};

class SeekHandler : public IUnknown
{
public:
    explicit SeekHandler(MediaSession *owner) : m_owner(owner) {}
    /// 同 ButtonPressedHandler：拆除时先摘掉 owner
    void clearOwner() { m_owner = nullptr; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, kIIDSeekHandler)
            || IsEqualIID(riid, kIIDAgileObject)) {
            *ppv = static_cast<IUnknown *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = --m_refs;
        if (left == 0) delete this;
        return left;
    }
    /// 槽位 3：系统调这里通知拖动/点击了进度条
    virtual HRESULT STDMETHODCALLTYPE Invoke(
        ISystemMediaTransportControls * /*sender*/,
        ISystemMediaTransportControlsPlaybackPositionChangeRequestedEventArgs *args);

private:
    std::atomic<ULONG> m_refs{1};
    MediaSession *m_owner = nullptr;
};

/// 进程内只初始化一次 WinRT 公寓；已有 STA（Qt 的拖放/OLE）时直接复用
bool ensureWinrtApartment()
{
    static const bool ok = []() {
        const HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
        if (SUCCEEDED(hr)) return true;
        // RPC_E_CHANGED_MODE：本线程已是 STA（Qt 的 ole 初始化），照用不误
        return hr == RPC_E_CHANGED_MODE;
    }();
    return ok;
}

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct MediaSession::Impl
{
    MediaSession *q = nullptr;
    PlayerController *player = nullptr;
    QPointer<QWindow> window;
    HWND hwnd = nullptr;
    HWND triedHwnd = nullptr; ///< 上次尝试建立的句柄（失败时别让定时器反复重试）
    QTimer *hwndTimer = nullptr;

    ISystemMediaTransportControls *smtc = nullptr;
    ISystemMediaTransportControls2 *smtc2 = nullptr;
    ISystemMediaTransportControlsDisplayUpdater *updater = nullptr;
    IMusicDisplayProperties *musicProps = nullptr;
    ISystemMediaTransportControlsTimelineProperties *timeline = nullptr;
    IInspectable *activationFactory = nullptr; ///< 激活工厂要一直持有：一放就把 smtc 带走了

    IRandomAccessStreamReferenceStatics *rasStatics = nullptr;
    IUriRuntimeClassFactory *uriFactory = nullptr;

    ButtonPressedHandler *buttonHandler = nullptr;
    SeekHandler *seekHandler = nullptr;
    EventRegistrationToken buttonToken{};
    EventRegistrationToken seekToken{};
    bool buttonSubscribed = false;
    bool seekSubscribed = false;

    QString pushedCover;        ///< 上次推给系统的封面 URI（相同就不重复推）
    qint64 lastTimelinePos = -1; ///< 时间轴节流（系统面板每 200ms 更新没必要）
    int buttonCalls = 0;         ///< 系统侧送来的按钮回调次数（自检用）
};

// ---------------------------------------------------------------------------
// Delegate::Invoke
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE ButtonPressedHandler::Invoke(
    ISystemMediaTransportControls *, ISystemMediaTransportControlsButtonPressedEventArgs *args)
{
    int button = -1;
    if (args) args->get_Button(&button);
    MSLOG("smTc button callback btn=%d\n", button);
    MediaSession *owner = m_owner;
    if (!owner) return S_OK;
    QMetaObject::invokeMethod(
        owner, [owner, button]() { owner->handleSystemButton(button); }, Qt::QueuedConnection);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SeekHandler::Invoke(
    ISystemMediaTransportControls *,
    ISystemMediaTransportControlsPlaybackPositionChangeRequestedEventArgs *args)
{
    WinTimeSpan span{0};
    if (args) args->get_RequestedPlaybackPosition(&span);
    const qint64 ms = span.Duration / 10000; // 100ns → ms
    MediaSession *owner = m_owner;
    if (!owner) return S_OK;
    QMetaObject::invokeMethod(
        owner, [owner, ms]() { owner->handleSystemSeek(ms); }, Qt::QueuedConnection);
    return S_OK;
}

// ---------------------------------------------------------------------------
// MediaSession
// ---------------------------------------------------------------------------

MediaSession::MediaSession(PlayerController *player, QObject *parent)
    : QObject(parent), d(new Impl)
{
    d->q = this;
    d->player = player;
    if (!player) return;

    // 切歌 / 播放状态 / 进度 / 时长：全部转成系统侧更新
    connect(player, &PlayerController::currentSongChanged, this, &MediaSession::onSongChanged);
    connect(player, &PlayerController::isPlayingChanged, this, &MediaSession::onPlayingChanged);
    connect(player, &PlayerController::isLoadingChanged, this, &MediaSession::onPlayingChanged);
    connect(player, &PlayerController::positionChanged, this, &MediaSession::onPositionChanged);
    connect(player, &PlayerController::durationChanged, this, &MediaSession::onDurationChanged);

    // HWND 兜底巡检：Qt 改 DPI / 换屏 / 重建原生窗都可能不触发我们挂的信号
    d->hwndTimer = new QTimer(this);
    d->hwndTimer->setInterval(4000);
    connect(d->hwndTimer, &QTimer::timeout, this, &MediaSession::syncHwnd);
    d->hwndTimer->start();
}

MediaSession::~MediaSession()
{
    releaseSession();
    delete d;
}

void MediaSession::attach(QWindow *window)
{
    d->window = window;
    if (!window) return;
    MSLOG("attach(%p)\n", reinterpret_cast<void *>(window));
    // 进出全屏时 Qt 会重建原生窗口（同 HotkeyManager 的教训）：句柄一换立刻重挂
    connect(window, &QWindow::visibilityChanged, this, [this]() {
        QTimer::singleShot(0, this, [this]() { syncHwnd(); });
    });
    syncHwnd();
}

bool MediaSession::sessionActive() const
{
    return d->smtc != nullptr;
}

void MediaSession::syncHwnd()
{
    if (!d->window) return;
    // 平台窗已销毁（退出关窗过程中）→ 直接摘掉会话，绝不能碰 winId()：
    // 那会把窗口重新创建出来（ui/WindowHwnd.h 记录了这条事故链）。
    HWND cur = hwndWithoutCreating(d->window);
    MSLOG("syncHwnd cur=%p 旧=%p\n", reinterpret_cast<void *>(cur),
          reinterpret_cast<void *>(d->hwnd));
    if (cur == d->hwnd) return;
    if (d->smtc) MSLOG("HWND 变化 %p → %p，重挂系统媒体会话\n",
                       reinterpret_cast<void *>(d->hwnd), reinterpret_cast<void *>(cur));
    releaseSession();
    d->hwnd = cur;
    if (!cur) return;
    ensureSession();
}

bool MediaSession::ensureSession()
{
    if (d->smtc) return true;
    if (!d->hwnd) return false;
    if (d->triedHwnd == d->hwnd) return false;
    d->triedHwnd = d->hwnd;
    MSLOG("ensureSession hwnd=%p\n", reinterpret_cast<void *>(d->hwnd));

    if (!ensureWinrtApartment()) {
        MSLOG("WinRT 公寓初始化失败\n");
        return false;
    }
    MSLOG("WinRT 公寓就绪\n");

    WinHstring cls(kClassControls);
    void *factory = nullptr;
    HRESULT hr = RoGetActivationFactory(cls.get(), kIIDInterop, &factory);
    MSLOG("RosGetActivationFactory hr=0x%08lx factory=%p\n", static_cast<unsigned long>(hr),
          factory);
    if (FAILED(hr) || !factory) {
        MSLOG("RoGetActivationFactory(SMTC) 失败 hr=0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }

    auto *interop = static_cast<ISystemMediaTransportControlsInterop *>(factory);
    hr = interop->GetForWindow(d->hwnd, kIIDControls, reinterpret_cast<void **>(&d->smtc));
    MSLOG("GetForWindow hr=0x%08lx smtc=%p\n", static_cast<unsigned long>(hr),
          reinterpret_cast<void *>(d->smtc));
    if (FAILED(hr) || !d->smtc) {
        MSLOG("GetForWindow 失败 hr=0x%08lx\n", static_cast<unsigned long>(hr));
        d->smtc = nullptr;
        interop->Release();
        return false;
    }
    // 工厂必须一直持有：GetForWindow 拿到的 smtc 由工厂内部缓存托管，工厂一释放就悬空
    d->activationFactory = static_cast<IInspectable *>(factory);

    // 会话可见 + 按钮可用（不启用 Stop，避免系统面板多出一个"停止"）
    d->smtc->put_IsEnabled(1);
    d->smtc->put_IsPlayEnabled(1);
    d->smtc->put_IsPauseEnabled(1);
    d->smtc->put_IsNextEnabled(1);
    d->smtc->put_IsPreviousEnabled(1);
    d->smtc->put_IsStopEnabled(0);

    if (FAILED(d->smtc->get_DisplayUpdater(&d->updater)) || !d->updater) {
        MSLOG("取 DisplayUpdater 失败\n");
        d->updater = nullptr;
    } else {
        d->updater->put_Type(kMediaTypeMusic);
        if (FAILED(d->updater->get_MusicProperties(&d->musicProps))) d->musicProps = nullptr;
        MSLOG("updater=%p musicProps=%p\n", reinterpret_cast<void *>(d->updater),
              reinterpret_cast<void *>(d->musicProps));
    }

    // 时间轴（系统面板的进度条）：拿不到也不影响"能识别正在播放"
    if (SUCCEEDED(d->smtc->QueryInterface(kIIDControls2, reinterpret_cast<void **>(&d->smtc2)))
        && d->smtc2) {
        WinHstring tlCls(kClassTimeline);
        IInspectable *inst = nullptr;
        if (SUCCEEDED(RoActivateInstance(tlCls.get(), &inst)) && inst) {
            if (FAILED(inst->QueryInterface(kIIDTimeline,
                                            reinterpret_cast<void **>(&d->timeline))))
                d->timeline = nullptr;
            inst->Release();
        }
        if (d->timeline) {
            d->seekHandler = new SeekHandler(this); // 初始引用计数 1（我们持有）
            if (FAILED(d->smtc2->add_PlaybackPositionChangeRequested(
                    d->seekHandler, &d->seekToken))) {
                d->seekHandler->Release();
                d->seekHandler = nullptr;
            } else {
                d->seekSubscribed = true;
            }
        }
    }

    // 按钮事件（播放/暂停/上一首/下一首）
    d->buttonHandler = new ButtonPressedHandler(this);
    if (FAILED(d->smtc->add_ButtonPressed(d->buttonHandler, &d->buttonToken))) {
        d->buttonHandler->Release();
        d->buttonHandler = nullptr;
        MSLOG("订阅 ButtonPressed 失败\n");
    } else {
        d->buttonSubscribed = true;
    }

    MSLOG("系统媒体会话已建立（hwnd=%p）\n", reinterpret_cast<void *>(d->hwnd));

    // 把当前状态补推一遍（窗口重建后系统侧是空的）
    pushMetadata();
    pushButtons();
    pushPlaybackStatus();
    pushTimeline(true);
    return true;
}

void MediaSession::releaseSession()
{
    if (d->smtc) {
        if (d->buttonSubscribed) d->smtc->remove_ButtonPressed(d->buttonToken);
        if (d->seekSubscribed && d->smtc2)
            d->smtc2->remove_PlaybackPositionChangeRequested(d->seekToken);
    }
    d->buttonSubscribed = false;
    d->seekSubscribed = false;

    // 先把 owner 摘掉再退订阅：系统线程上可能还有一帧在飞的回调
    if (d->buttonHandler) d->buttonHandler->clearOwner();
    if (d->seekHandler) d->seekHandler->clearOwner();

    if (d->buttonHandler) {
        d->buttonHandler->Release();
        d->buttonHandler = nullptr;
    }
    if (d->seekHandler) {
        d->seekHandler->Release();
        d->seekHandler = nullptr;
    }
    if (d->updater) {
        d->updater->ClearAll();
        d->updater->Release();
        d->updater = nullptr;
    }
    if (d->musicProps) {
        d->musicProps->Release();
        d->musicProps = nullptr;
    }
    if (d->timeline) {
        d->timeline->Release();
        d->timeline = nullptr;
    }
    if (d->smtc2) {
        d->smtc2->Release();
        d->smtc2 = nullptr;
    }
    if (d->smtc) {
        d->smtc->put_PlaybackStatus(kPlaybackClosed);
        d->smtc->put_IsEnabled(0);
        d->smtc->Release();
        d->smtc = nullptr;
    }
    if (d->rasStatics) {
        d->rasStatics->Release();
        d->rasStatics = nullptr;
    }
    if (d->uriFactory) {
        d->uriFactory->Release();
        d->uriFactory = nullptr;
    }
    if (d->activationFactory) {
        d->activationFactory->Release();
        d->activationFactory = nullptr;
    }
    d->pushedCover.clear();
    d->lastTimelinePos = -1;
    d->triedHwnd = nullptr;
}

void MediaSession::onSongChanged()
{
    pushMetadata();
    pushButtons();
    pushPlaybackStatus();
    pushTimeline(true);
}

void MediaSession::onPlayingChanged()
{
    pushPlaybackStatus();
}

void MediaSession::onDurationChanged()
{
    pushTimeline(true);
}

void MediaSession::onPositionChanged()
{
    pushTimeline(false);
}

void MediaSession::pushMetadata()
{
    if (!d->updater || !d->player) return;
    const Song song = d->player->currentSong();
    if (song.name.isEmpty() && song.id.isEmpty()) {
        // 没有当前曲：清空元数据，系统面板不再显示残留的上一首
        d->updater->ClearAll();
        d->pushedCover.clear();
        return;
    }

    d->updater->put_Type(kMediaTypeMusic);
    if (d->musicProps) {
        const WinHstring title(song.name);
        const WinHstring artist(song.artist);
        d->musicProps->put_Title(title.get());
        d->musicProps->put_Artist(artist.get());
        d->musicProps->put_AlbumArtist(artist.get());
    }

    // 封面：URL 交给系统自己取（本机缓存目录里的本地文件同样支持）
    const QString cover = coverToUri(song.cover);
    if (cover != d->pushedCover) {
        d->pushedCover = cover;
        if (cover.isEmpty()) {
            d->updater->put_Thumbnail(nullptr);
        } else {
            IRandomAccessStreamReference *ref = nullptr;
            if (!d->rasStatics || !d->uriFactory) {
                WinHstring rasCls(kClassRasRef);
                WinHstring uriCls(kClassUri);
                void *fac = nullptr;
                if (!d->rasStatics
                    && SUCCEEDED(RoGetActivationFactory(rasCls.get(), kIIDRasStatics, &fac))) {
                    d->rasStatics = static_cast<IRandomAccessStreamReferenceStatics *>(fac);
                }
                fac = nullptr;
                if (!d->uriFactory
                    && SUCCEEDED(RoGetActivationFactory(uriCls.get(), kIIDUriFactory, &fac))) {
                    d->uriFactory = static_cast<IUriRuntimeClassFactory *>(fac);
                }
            }
            IUriRuntimeClass *wuri = nullptr;
            if (d->uriFactory && d->rasStatics) {
                const WinHstring uriStr(cover);
                if (SUCCEEDED(d->uriFactory->CreateUri(uriStr.get(), &wuri)) && wuri) {
                    const HRESULT hr = d->rasStatics->CreateFromUri(wuri, &ref);
                    wuri->Release();
                    if (FAILED(hr)) ref = nullptr;
                }
            }
            if (ref) {
                d->updater->put_Thumbnail(ref);
                ref->Release();
            } else {
                MSLOG("封面推给系统失败（%s）\n", qPrintable(cover));
            }
        }
    }
    d->updater->Update();
}

void MediaSession::pushButtons()
{
    if (!d->smtc) return;
    d->smtc->put_IsPlayEnabled(1);
    d->smtc->put_IsPauseEnabled(1);
    d->smtc->put_IsNextEnabled(1);
    d->smtc->put_IsPreviousEnabled(1);
}

void MediaSession::pushPlaybackStatus()
{
    if (!d->smtc || !d->player) return;
    int status = kPlaybackClosed;
    const Song cur = d->player->currentSong();
    if (!cur.id.isEmpty() || !cur.name.isEmpty()) {
        // 缓冲中别报"正在播放"（系统面板会显示成在响而实际没声）
        if (d->player->isLoading()) status = kPlaybackPaused;
        else status = d->player->isPlaying() ? kPlaybackPlaying : kPlaybackPaused;
    }
    d->smtc->put_PlaybackStatus(status);
}

void MediaSession::pushTimeline(bool force)
{
    if (!d->smtc2 || !d->timeline || !d->player) return;
    const qint64 dur = d->player->duration();
    if (dur <= 0) return;
    const qint64 pos = qBound<qint64>(0, d->player->position(), dur);
    // 系统面板刷新很快，没必要 200ms 一次的 positionChanged 全都推过去
    if (!force && d->lastTimelinePos >= 0 && qAbs(pos - d->lastTimelinePos) < 900) return;
    d->lastTimelinePos = pos;

    const WinTimeSpan zero{0};
    const WinTimeSpan end{dur * 10000};
    const WinTimeSpan at{pos * 10000};
    d->timeline->put_StartTime(zero);
    d->timeline->put_EndTime(end);
    d->timeline->put_MinSeekTime(zero);
    d->timeline->put_MaxSeekTime(end);
    d->timeline->put_Position(at);
    d->smtc2->UpdateTimelineProperties(d->timeline);
}

void MediaSession::handleSystemButton(int button)
{
    if (!d->player) return;
    ++d->buttonCalls;
    switch (button) {
    case kBtnPlay:
        if (!d->player->isPlaying()) d->player->togglePlay();
        break;
    case kBtnPause:
        if (d->player->isPlaying()) d->player->pause();
        break;
    case kBtnStop:
        d->player->stop();
        break;
    case kBtnNext:
        d->player->next();
        break;
    case kBtnPrevious:
        d->player->previous();
        break;
    default:
        // 快进/快退/录音等：本程序没有对应功能，忽略
        break;
    }
}

void MediaSession::handleSystemSeek(qint64 ms)
{
    if (!d->player) return;
    d->player->seek(ms);
}

#ifdef MUYUN_SELFTES
QString MediaSession::testPushAndVerify(const QVariantMap &songMap, bool playing)
{
    if (!ensureSession()) return QStringLiteral("系统媒体会话没建立（GetForWindow 失败）");
    if (!d->updater) return QStringLiteral("DisplayUpdater 不可用");

    const Song song = Song::fromMap(songMap);
    // 用合成曲目直接推元数据（不经过播放器，自检不依赖音频文件）
    d->updater->put_Type(kMediaTypeMusic);
    if (d->musicProps) {
        const WinHstring title(song.name);
        const WinHstring artist(song.artist);
        d->musicProps->put_Title(title.get());
        d->musicProps->put_Artist(artist.get());
        d->musicProps->put_AlbumArtist(artist.get());
    }
    d->updater->Update();
    d->smtc->put_PlaybackStatus(playing ? kPlaybackPlaying : kPlaybackPaused);
    d->smtc->put_IsPlayEnabled(1);
    d->smtc->put_IsPauseEnabled(1);
    d->smtc->put_IsNextEnabled(1);
    d->smtc->put_IsPreviousEnabled(1);

    // 时间轴能力探测（拿不到只是没有进度条，不算失败）
    if (d->smtc2 && d->timeline) {
        const WinTimeSpan zero{0};
        const WinTimeSpan total{30000LL * 10000};
        const WinTimeSpan at{5000LL * 10000};
        d->timeline->put_StartTime(zero);
        d->timeline->put_EndTime(total);
        d->timeline->put_MinSeekTime(zero);
        d->timeline->put_MaxSeekTime(total);
        d->timeline->put_Position(at);
        const HRESULT hr = d->smtc2->UpdateTimelineProperties(d->timeline);
        MSLOG("时间轴推送 hr=0x%08lx\n", static_cast<unsigned long>(hr));
    } else {
        MSLOG("时间轴不可用（系统面板不会有进度条）\n");
    }

    // 回读校验：标题与播放状态必须与刚推的一致，否则说明系统侧根本没接住
    if (!d->musicProps) return QStringLiteral("MusicProperties 不可用");
    HSTRING raw = nullptr;
    if (FAILED(d->musicProps->get_Title(&raw)))
        return QStringLiteral("回读标题失败");
    QString gotTitle;
    if (raw) {
        UINT32 len = 0;
        const wchar_t *buf = WindowsGetStringRawBuffer(raw, &len);
        gotTitle = QString::fromWCharArray(buf, static_cast<int>(len));
        WindowsDeleteString(raw);
    }
    if (gotTitle != song.name)
        return QStringLiteral("标题回读不一致：系统侧=%1 期望=%2").arg(gotTitle, song.name);

    int status = -1;
    if (FAILED(d->smtc->get_PlaybackStatus(&status)))
        return QStringLiteral("回读播放状态失败");
    const int want = playing ? kPlaybackPlaying : kPlaybackPaused;
    if (status != want)
        return QStringLiteral("播放状态回读不一致：系统侧=%1 期望=%2").arg(status).arg(want);

    // 按钮可用位：系统面板上的播放/暂停/上一首/下一首必须真的可点
    WinBool isEnabled = 0, playOn = 0, pauseOn = 0, prevOn = 0, nextOn = 0;
    d->smtc->get_IsEnabled(&isEnabled);
    d->smtc->get_IsPlayEnabled(&playOn);
    d->smtc->get_IsPauseEnabled(&pauseOn);
    d->smtc->get_IsPreviousEnabled(&prevOn);
    d->smtc->get_IsNextEnabled(&nextOn);
    if (!isEnabled || !playOn || !pauseOn || !prevOn || !nextOn)
        return QStringLiteral("按钮可用位不对：enabled=%1 play=%2 pause=%3 prev=%4 next=%5")
            .arg(int(isEnabled)).arg(int(playOn)).arg(int(pauseOn)).arg(int(prevOn)).arg(int(nextOn));

    // 再切一次状态：单向推送有可能只成功一次，来回都要跟得上
    d->smtc->put_PlaybackStatus(playing ? kPlaybackPaused : kPlaybackPlaying);
    int status2 = -1;
    if (FAILED(d->smtc->get_PlaybackStatus(&status2))
        || status2 != (playing ? kPlaybackPaused : kPlaybackPlaying))
        return QStringLiteral("第二次状态回读不一致：系统侧=%1").arg(status2);
    d->smtc->put_PlaybackStatus(want); // 复原

    MSLOG("标题=%s 状态=%d\n", qPrintable(gotTitle), status);
    return QString();
}

int MediaSession::testSystemButtonCount() const
{
    return d ? d->buttonCalls : 0;
}
#endif // MUYUN_SELFTES

#else // 非 Windows：没有系统媒体控制，退化成空实现

struct MediaSession::Impl {};

MediaSession::MediaSession(PlayerController *, QObject *parent) : QObject(parent), d(new Impl) {}
MediaSession::~MediaSession() { delete d; }
void MediaSession::attach(QWindow *) {}
bool MediaSession::sessionActive() const { return false; }
void MediaSession::handleSystemButton(int) {}
void MediaSession::handleSystemSeek(qint64) {}
void MediaSession::onSongChanged() {}
void MediaSession::onPlayingChanged() {}
void MediaSession::onPositionChanged() {}
void MediaSession::onDurationChanged() {}
void MediaSession::syncHwnd() {}
bool MediaSession::ensureSession() { return false; }
void MediaSession::releaseSession() {}
void MediaSession::pushMetadata() {}
void MediaSession::pushButtons() {}
void MediaSession::pushPlaybackStatus() {}
void MediaSession::pushTimeline(bool) {}
#ifdef MUYUN_SELFTES
QString MediaSession::testPushAndVerify(const QVariantMap &, bool) { return QStringLiteral("非 Windows 平台"); }
int MediaSession::testSystemButtonCount() const { return 0; }
#endif

#endif // Q_OS_WIN

} // namespace Muyun
