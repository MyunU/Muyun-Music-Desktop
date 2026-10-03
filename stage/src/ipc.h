// MuyunStage IPC：双命名管道
//  - 命令通道（stage 为服务端，主程序为客户端写入）：主程序 → stage → 页面
//  - 事件通道（主程序为服务端，stage 为客户端写入）：页面 → stage → 主程序
// 帧格式：4 字节小端长度 + UTF-8 JSON
#pragma once
#include <windows.h>
#include <string>

namespace muyun {

// 从 handle 读取一整帧（阻塞）。失败返回 false（对端断开/超时）
bool ReadFrame(HANDLE h, std::string& out);
bool WriteFrameRaw(HANDLE h, const std::string& data);

// 命令管道服务端：Create 后等待连接，暴露一个可阻塞读句柄
class CmdPipe {
public:
    bool listen(const std::wstring& name);          // 创建并等待首连接
    bool read(std::string& out);                    // 阻塞读一帧；断开/取消返回 false
    void close();
    HANDLE handle() const { return m_pipe; }
    // 诊断：最近一次 listen 失败的名字与错误码（快速开关竞态排查）
    static DWORD lastErr();
    static const std::wstring& lastName();
private:
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    static DWORD s_lastErr;
    static std::wstring s_lastName;
};

// 事件管道客户端：连接主程序的 \\.\pipe\MuyunStageEv.<pid>
class EvPipe {
public:
    EvPipe() { InitializeCriticalSection(&m_cs); }
    ~EvPipe() { DeleteCriticalSection(&m_cs); }
    bool connect(const std::wstring& name, DWORD retryMs = 500, int retries = 40);
    bool write(const std::string& json);            // 线程安全；未连接时静默丢弃
    void close();
private:
    HANDLE m_h = INVALID_HANDLE_VALUE;
    CRITICAL_SECTION m_cs;
};

// 极简 JSON 字段提取（仅用于 stage 外壳识别控制命令；页面侧才是完整解析者）
// 支持 "cmd":"bounds" 与 "x":123 之类的顶层数字/字符串。
bool jsonGetString(const std::string& json, const std::string& key, std::string& outVal);
bool jsonGetNumber(const std::string& json, const std::string& key, double& outVal);

} // namespace muyun
