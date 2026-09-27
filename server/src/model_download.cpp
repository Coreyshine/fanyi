/*
 * fanyi — model_download.cpp
 * 多镜像模型下载器：按顺序尝试镜像，单个失败自动切下一个；
 * .part 断点续传（同一文件跨镜像续传安全：各镜像内容一致，已实测大小精确一致）；
 * 后台线程执行，curl 子进程下载，进度按已落盘字节估算。
 */
#include "model_download.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <functional>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <signal.h>
#endif

namespace fanyi {

static std::mutex g_mtx;
static DownloadState g_state;
static std::atomic<long long> g_cancel{0};   /* 取消代次：值变化 = 请求取消 */
static bool g_thread_alive = false;

std::vector<std::string> model_default_mirrors() {
    /* 两个独立运营的源，均已实测（文件大小 1,908,528,192 字节精确一致）：
       1) hf-mirror.com — HuggingFace 国内全量镜像，大陆直连快
       2) huggingface.co — 官方源 */
    return {
        "https://hf-mirror.com/tencent/Hy-MT2-1.8B-GGUF/resolve/main/Hy-MT2-1.8B-Q8_0.gguf",
        "https://huggingface.co/tencent/Hy-MT2-1.8B-GGUF/resolve/main/Hy-MT2-1.8B-Q8_0.gguf",
    };
}

static long long file_size(const std::string &path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path.c_str(), GetFileAttributesExInfo, &fa)) return -1;
    return ((long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return -1;
    return (long long)st.st_size;
#endif
}

/* HEAD 探测：取 Content-Length（校验可达性 + 进度基准），失败返回 -1 */
static long long probe_size(const std::string &url) {
    std::string cmd = "curl -sIL --connect-timeout 12 --max-time 30 '" + url + "' 2>/dev/null";
#ifdef _WIN32
    FILE *p = _popen(cmd.c_str(), "r");
#else
    FILE *p = popen(cmd.c_str(), "r");
#endif
    if (!p) return -1;
    char buf[512];
    long long len = -1;
    while (fgets(buf, sizeof buf, p)) {
        std::string line(buf);
        for (char &c : line) if (c >= 'A' && c <= 'Z') c += 32;
        if (line.rfind("content-length:", 0) == 0) len = atoll(line.c_str() + 15);
    }
#ifdef _WIN32
    _pclose(p);
#else
    pclose(p);
#endif
    return len;
}

/* curl 子进程封装（可非阻塞查询是否退出） */
struct CurlProc {
#ifdef _WIN32
    PROCESS_INFORMATION pi{};
    bool has_exit_ = false;
    int exit_code_ = -1;
    bool run(const std::string &cmdline) {
        STARTUPINFOA si{};
        si.cb = sizeof si;
        std::string c = cmdline;
        return CreateProcessA(NULL, c.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW,
                              NULL, NULL, &si, &pi);
    }
    bool running() {
        if (!pi.hProcess || has_exit_) return false;
        DWORD r = WaitForSingleObject(pi.hProcess, 0);
        if (r == WAIT_OBJECT_0) {
            DWORD code = 1;
            GetExitCodeProcess(pi.hProcess, &code);
            exit_code_ = (int)code;
            has_exit_ = true;
            return false;
        }
        return true;
    }
    void kill() {
        if (pi.hProcess && !has_exit_) TerminateProcess(pi.hProcess, 42);
    }
    int wait() {
        while (running()) Sleep(100);
        return exit_code_;
    }
    ~CurlProc() {
        if (pi.hProcess) CloseHandle(pi.hProcess);
        if (pi.hThread) CloseHandle(pi.hThread);
    }
#else
    pid_t pid_ = -1;
    bool has_exit_ = false;
    int exit_code_ = -1;
    bool run(const std::string &cmdline) {
        pid_ = fork();
        if (pid_ < 0) return false;
        if (pid_ == 0) {
            execl("/bin/sh", "sh", "-c", cmdline.c_str(), (char *)NULL);
            _exit(127);
        }
        return true;
    }
    bool running() {
        if (pid_ <= 0 || has_exit_) return false;
        int status = 0;
        if (waitpid(pid_, &status, WNOHANG) == pid_) {
            has_exit_ = true;
            exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            return false;
        }
        return true;
    }
    void kill() {
        if (pid_ > 0 && !has_exit_) ::kill(pid_, SIGTERM);
    }
    int wait() {
        while (running()) usleep(100000);
        return exit_code_;
    }
    ~CurlProc() {}
#endif
};

void model_download_start(const std::string &target_dir, const std::string &filename,
                          const std::vector<std::string> &mirrors, long long expected_size,
                          void (*on_success)(void *), void *userdata) {
    {
        std::lock_guard<std::mutex> g(g_mtx);
        if (g_state.active) return;
        g_state = DownloadState{};
        g_state.active = true;
        ++g_cancel;
    }

    auto *done = new std::function<void()>([on_success, userdata] { if (on_success) on_success(userdata); });

    std::thread([target_dir, filename, mirrors, expected_size, done]() {
        long long cancel_gen;
        { std::lock_guard<std::mutex> g(g_mtx); cancel_gen = g_cancel.load(); }
        auto cancelled = [&cancel_gen]() {
            std::lock_guard<std::mutex> g(g_mtx);
            return g_cancel.load() != cancel_gen;
        };

        std::string part = target_dir + "/" + filename + ".part";
        std::string full = target_dir + "/" + filename;
        std::string err;
        bool ok = false;

        for (const auto &url : mirrors) {
            if (cancelled()) { err = "已取消"; break; }
            { std::lock_guard<std::mutex> g(g_mtx); g_state.mirror = url; }

            long long expect = expected_size > 0 ? expected_size : probe_size(url);
            if (cancelled()) { err = "已取消"; break; }

            std::string cmd = "curl -fL -C - --connect-timeout 15 --retry 2 --retry-delay 2 -o '" +
                              part + "' '" + url + "' 2>/dev/null";
            CurlProc c;
            if (!c.run(cmd)) { err = "无法启动 curl"; continue; }

            bool was_cancelled = false;
            while (c.running()) {
                if (cancelled()) { c.kill(); c.wait(); was_cancelled = true; break; }
                long long got = file_size(part);
                if (expect > 0) {
                    int pct = (int)(got * 100 / expect);
                    if (pct < 0) pct = 0;
                    if (pct > 100) pct = 100;
                    std::lock_guard<std::mutex> g(g_mtx);
                    g_state.progress = pct;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(800));
            }

            if (was_cancelled) { err = "已取消"; break; }

            int exit_code = c.wait();
            long long got = file_size(part);
            if (exit_code == 0 && (expect <= 0 || got == expect)) {
                if (std::rename(part.c_str(), full.c_str()) == 0) { ok = true; break; }
                err = "下载完成但保存失败";
            } else if (exit_code == 42) {
                err = "已取消";
                break;
            } else {
                /* 该镜像失败（中断/404/超时）——保留 .part 供下个镜像续传 */
                std::lock_guard<std::mutex> g(g_mtx);
                g_state.error = "镜像不可用(curl exit " + std::to_string(exit_code) + ")，切换下一个…";
                g_state.progress = -1;
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
            }
        }

        std::lock_guard<std::mutex> g(g_mtx);
        g_state.active = false;
        g_state.succeeded = ok;
        if (ok) { g_state.progress = 100; g_state.error = ""; }
        else if (err.empty()) g_state.error = "全部镜像均失败";
        else g_state.error = err;
        auto fn = *done;
        delete done;
        if (ok) fn();
    }).detach();
    g_thread_alive = true;
}

void model_download_cancel() {
    std::lock_guard<std::mutex> g(g_mtx);
    ++g_cancel;
    g_state.active = false;
    g_state.error = "已取消";
}

DownloadState model_download_status() {
    std::lock_guard<std::mutex> g(g_mtx);
    return g_state;
}

} // namespace fanyi
