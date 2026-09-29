/*
 * fanyi — model_download.h
 * 模型下载管理：多镜像自动降级、断点续传、进度查询、取消。
 * 用系统 curl 子进程实现（macOS/Win10+/Linux 均内置 curl），零额外依赖。
 */
#ifndef FANYI_MODEL_DOWNLOAD_H
#define FANYI_MODEL_DOWNLOAD_H

#include <string>
#include <vector>

namespace fanyi {

struct DownloadState {
    bool     active  = false;   // 正在下载
    int      progress = -1;     // 0-100（-1 未知）
    std::string mirror;          // 当前使用的镜像
    std::string error;           // 最后一次失败原因（active=false 时有效）
    bool     succeeded = false; // 最近一次下载是否成功
};

/* 模型目录条目（三档量化） */
struct CatalogEntry {
    std::string id;        // "Q8_0" / "Q6_K" / "Q4_K_M"
    std::string file;      // GGUF 文件名
    long long   size;      // 精确字节数（完整性校验）
    std::string size_text; // 展示用，如 "1.9 GB"
    std::string ram_text;  // 翻译时内存，如 "约 2.5 GB"
};

/*
 * 启动下载（已在下载中则忽略）。
 * target_dir: 保存目录；filename: 文件名；mirrors: 按顺序尝试的完整 URL 列表。
 * expected_size: 期望字节数（用于进度与完整性校验，<=0 则只看 curl 退出码）。
 * on_success: 成功后回调（在下载线程执行）。
 */
void model_download_start(const std::string &target_dir, const std::string &filename,
                          const std::vector<std::string> &mirrors, long long expected_size,
                          void (*on_success)(void *), void *userdata);
void model_download_cancel();
DownloadState model_download_status();

/* 默认镜像表（按优先级；已在多环境实测） */
std::vector<std::string> model_default_mirrors();

/* 三档量化目录（tencent/Hy-MT2-1.8B-GGUF） */
std::vector<CatalogEntry> model_catalog();
/* 按条目生成镜像 URL 列表 */
std::vector<std::string> model_mirrors_for(const CatalogEntry &e);

} // namespace fanyi
#endif
