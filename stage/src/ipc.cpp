#include "ipc.h"
#include <cstdint>
#include <cstring>

namespace muyun {

// 归一化管道名：允许传短名，自动补 \\.\pipe\ 前缀
static std::wstring PipeFull(const std::wstring& name) {
    if (name.rfind(L"\\\\.\\pipe\\", 0) == 0) return name;
    return L"\\\\.\\pipe\\" + name;
}

bool ReadFrame(HANDLE h, std::string& out) {
    uint32_t len = 0;
    DWORD got = 0;
    if (!ReadFile(h, &len, 4, &got, nullptr) || got != 4) return false;
    if (len == 0 || len > 32u * 1024u * 1024u) return false;
    out.resize(len);
    size_t done = 0;
    while (done < len) {
        DWORD n = 0;
        if (!ReadFile(h, out.data() + done, (DWORD)(len - done), &n, nullptr) || n == 0)
            return false;
        done += n;
    }
    return true;
}

bool WriteFrameRaw(HANDLE h, const std::string& data) {
    if (h == INVALID_HANDLE_VALUE) return false;
    uint32_t len = (uint32_t)data.size();
    DWORD n = 0;
    if (!WriteFile(h, &len, 4, &n, nullptr) || n != 4) return false;
    size_t done = 0;
    while (done < data.size()) {
        if (!WriteFile(h, data.data() + done, (DWORD)(data.size() - done), &n, nullptr) || n == 0)
            return false;
        done += n;
    }
    return true;
}

DWORD CmdPipe::s_lastErr = 0;
std::wstring CmdPipe::s_lastName;

DWORD CmdPipe::lastErr() { return s_lastErr; }
const std::wstring& CmdPipe::lastName() { return s_lastName; }

bool CmdPipe::listen(const std::wstring& name) {
    const std::wstring full = PipeFull(name);
    s_lastName = full;
    m_pipe = CreateNamedPipeW(full.c_str(),
        PIPE_ACCESS_DUPLEX,   // QLocalSocket 客户端以读写打开，需 duplex
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 64 * 1024, 64 * 1024, 0, nullptr);
    if (m_pipe == INVALID_HANDLE_VALUE) { s_lastErr = GetLastError(); return false; }
    for (;;) {
        if (ConnectNamedPipe(m_pipe, nullptr)) return true;
        const DWORD err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED) return true;   // 客户端已先到
        // 被 CancelSynchronousIo 中断 / 句柄失效 → 退出（进程正在关闭）
        if (err == ERROR_OPERATION_ABORTED || err == ERROR_INVALID_HANDLE ||
            err == ERROR_NO_DATA || err == ERROR_BAD_PIPE || err == ERROR_PIPE_NOT_CONNECTED) {
            s_lastErr = err;   // 诊断：connect 等待期被打断的具体错误码
            return false;
        }
        Sleep(100);
    }
}

bool CmdPipe::read(std::string& out) {
    return ReadFrame(m_pipe, out);
}

void CmdPipe::close() {
    if (m_pipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(m_pipe);
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
}

bool EvPipe::connect(const std::wstring& name, DWORD retryMs, int retries) {
    const std::wstring full = PipeFull(name);
    for (int i = 0; i < retries; ++i) {
        m_h = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr,
                          OPEN_EXISTING, 0, nullptr);
        if (m_h != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_BYTE;
            SetNamedPipeHandleState(m_h, &mode, nullptr, nullptr);
            return true;
        }
        Sleep(retryMs);
    }
    return false;
}

bool EvPipe::write(const std::string& json) {
    EnterCriticalSection(&m_cs);
    const bool ok = WriteFrameRaw(m_h, json);
    LeaveCriticalSection(&m_cs);
    return ok;
}

void EvPipe::close() {
    EnterCriticalSection(&m_cs);
    if (m_h != INVALID_HANDLE_VALUE) { CloseHandle(m_h); m_h = INVALID_HANDLE_VALUE; }
    LeaveCriticalSection(&m_cs);
}

// ---- 极简 JSON 顶层字段提取（值不含转义引号的简单场景；够用） ----
static const char* findKey(const std::string& j, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    size_t p = j.find(pat);
    while (p != std::string::npos) {
        size_t q = p + pat.size();
        while (q < j.size() && (j[q] == ' ' || j[q] == '\t')) ++q;
        if (q < j.size() && j[q] == ':') return j.c_str() + q + 1;
        p = j.find(pat, p + pat.size());
    }
    return nullptr;
}

bool jsonGetString(const std::string& json, const std::string& key, std::string& outVal) {
    const char* v = findKey(json, key);
    if (!v) return false;
    while (*v == ' ' || *v == '\t') ++v;
    if (*v != '"') return false;
    ++v;
    std::string s;
    while (*v && *v != '"') {
        if (*v == '\\' && v[1]) { ++v; }
        s += *v++;
    }
    outVal = s;
    return true;
}

bool jsonGetNumber(const std::string& json, const std::string& key, double& outVal) {
    const char* v = findKey(json, key);
    if (!v) return false;
    while (*v == ' ' || *v == '\t') ++v;
    char* endp = nullptr;
    const double d = strtod(v, &endp);
    if (endp == v) return false;
    outVal = d;
    return true;
}

} // namespace muyun
