#include "common.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace {

wchar_t g_dir[MAX_PATH];   // with a trailing backslash

void ModuleDir(HMODULE self)
{
    GetModuleFileNameW(self, g_dir, MAX_PATH);
    wchar_t* slash = wcsrchr(g_dir, L'\\');
    if (slash) slash[1] = 0;
}

int64_t g_qpcFreq  = 1;
int64_t g_qpcStart = 0;

struct Sink
{
    std::mutex  m;
    std::string buf;
    HANDLE      file = INVALID_HANDLE_VALUE;
};
Sink g_log, g_events;

void Append(Sink& s, const char* fmt, va_list ap, bool timestamp)
{
    char line[1024];
    int  n = 0;
    if (timestamp) n = snprintf(line, sizeof(line), "%10.3f  ", NowMs());
    int m = vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
    if (m < 0) return;
    n += (m < (int)sizeof(line) - n - 2) ? m : (int)sizeof(line) - n - 3;
    line[n++] = '\n';
    std::lock_guard<std::mutex> lk(s.m);
    s.buf.append(line, n);
}

void FlushSink(Sink& s)
{
    std::string out;
    HANDLE      file;
    {
        std::lock_guard<std::mutex> lk(s.m);
        out.swap(s.buf);
        file = s.file;
    }
    if (!out.empty() && file != INVALID_HANDLE_VALUE)
    {
        DWORD written;
        WriteFile(file, out.data(), (DWORD)out.size(), &written, nullptr);
    }
}

DWORD WINAPI WriterThread(void*)
{
    for (;;)
    {
        Sleep(250);
        Log_Flush();
    }
}

HANDLE OpenTruncate(const std::wstring& path)
{
    return CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

} // namespace

std::wstring FileNextToDll(const wchar_t* name) { return std::wstring(g_dir) + name; }

void Timing_Init()
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcFreq  = f.QuadPart;
    g_qpcStart = Qpc();
}

int64_t Qpc()
{
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}
double QpcToUs(int64_t ticks) { return (double)ticks * 1e6 / (double)g_qpcFreq; }
double NowMs() { return (double)(Qpc() - g_qpcStart) * 1e3 / (double)g_qpcFreq; }

void Log_Init(HMODULE self)
{
    ModuleDir(self);
    g_log.file = OpenTruncate(FileNextToDll(L"AnvilStutterFix.log"));
    if (HANDLE writer = CreateThread(nullptr, 0, WriterThread, nullptr, 0, nullptr)) CloseHandle(writer);

    wchar_t exe[MAX_PATH] = L"";
    char    exeUtf8[MAX_PATH * 3];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* name = wcsrchr(exe, L'\\');
    if (!WideCharToMultiByte(CP_UTF8, 0, name ? name + 1 : exe, -1, exeUtf8, sizeof(exeUtf8), nullptr, nullptr)) exeUtf8[0] = 0;
    Log("AnvilStutterFix %s (%s build) loaded into %s", ASF_VERSION, ASF_DEBUG ? "debug" : "release", exeUtf8);

    if (!ASF_DEBUG && GetFileAttributesW(FileNextToDll(L"AnvilStutterFix.ini").c_str()) != INVALID_FILE_ATTRIBUTES)
        Log("AnvilStutterFix.ini is ignored. Only the debug build reads it, this build uses the built-in settings instead.");
}

void Log_OpenEvents(const wchar_t* name)
{
    HANDLE file = OpenTruncate(FileNextToDll(name));
    std::lock_guard<std::mutex> lk(g_events.m);
    g_events.file = file;
}

void Log_Flush()
{
    FlushSink(g_log);
    FlushSink(g_events);
}

void Log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    Append(g_log, fmt, ap, true);
    va_end(ap);
}

void LogEvent(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    Append(g_events, fmt, ap, false);
    va_end(ap);
}
