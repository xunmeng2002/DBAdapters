#include <DbAdapters/BackendLoader/DbBackendLoader.h>

#include <DbAdapters/DbInterface/DbBackendFactory.h>

#ifdef _WIN32
// 本编译单元会拉入 windows.h, 并在其之前定义这两道 —— windows.h 的 min/max 宏会吃掉
// std::numeric_limits<T>::max() 这类写法. 关在 .cpp 里, 使用者的编译单元不受影响.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <array>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace DbAdapters
{
    namespace
    {
        constexpr int BackendFailureTextCapacity = 512;

#ifdef _WIN32
        constexpr const char* BackendModuleFilenamePrefix = "";
        constexpr const char* BackendModuleFilenameExtension = ".dll";
#else
        constexpr const char* BackendModuleFilenamePrefix = "lib";
        constexpr const char* BackendModuleFilenameExtension = ".so";
#endif

        // 调试后缀由 CMakeLists 按配置加在本目标上 (Debug 取 "d", 其余取空串). 不设静默兜底:
        // 漏了定义就编译不过, 而不是在 Debug 构装里静默装到 Release 那份同名模块 —— 那正是本库
        // 把后缀从消费方收回来要消灭的那个失效.
#ifndef BACKENDLOADER_FILENAME_DEBUG_POSTFIX
#error "BACKENDLOADER_FILENAME_DEBUG_POSTFIX 未定义. 该宏由 CMakeLists 加在 BackendLoaderStatic 上."
#endif
        constexpr const char* BackendModuleFilenameDebugPostfix = BACKENDLOADER_FILENAME_DEBUG_POSTFIX;

        std::string DatabaseBackendModuleFilename(const std::string& backendModuleBaseName)
        {
            return BackendModuleFilenamePrefix + backendModuleBaseName + BackendModuleFilenameDebugPostfix + BackendModuleFilenameExtension;
        }

        // 模块基名是本库自己的知识 (见本仓 CMakeLists 里那四个 add_shared_module), 故"哪种后端对应
        // 哪个模块"留在这里; 种类本身用 Spark 的 DbTypeType, 本库不自备. 越界的种类返回 nullptr,
        // 由调用方报错.
        constexpr const char* BackendModuleBaseName(DbTypeType dbType)
        {
            switch (dbType)
            {
            case DbTypeType::DuckDb:   return "DuckdbWrapper";
            case DbTypeType::SqliteDb: return "SqliteWrapper";
            case DbTypeType::MysqlDb:  return "MysqlWrapper";
            case DbTypeType::MariaDb:  return "MariadbWrapper";
            }
            return nullptr;
        }

        // 取本函数所在模块 (即引擎那张 DLL / 共享库) 的目录, 而不是宿主 (python.exe / python3) 的目录 ——
        // 部署上两者是分开的, 而 Windows 的显式 LoadLibraryEx 搜索序不含调用方自己的目录.
        // 路径按窄字符处理 (Windows 上是 A 系列 API): 中文路径下走系统 ANSI 代码页往返, 同机同页时仍成立,
        // 但不再向下兼容. 失败返回空串.
        std::string CurrentModuleDirectoryPath()
        {
#ifdef _WIN32
            HMODULE ownModule = nullptr;
            if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCSTR>(&CurrentModuleDirectoryPath), &ownModule) == FALSE)
            {
                return std::string();
            }

            char modulePath[MAX_PATH] = { 0 };
            const DWORD writtenLength = ::GetModuleFileNameA(ownModule, modulePath, MAX_PATH);
            if (writtenLength == 0 || writtenLength >= MAX_PATH)
            {
                return std::string();
            }
            return std::filesystem::path(modulePath).parent_path().string();
#else
            Dl_info ownModule = {};
            if (::dladdr(reinterpret_cast<const void*>(&CurrentModuleDirectoryPath), &ownModule) == 0
                || ownModule.dli_fname == nullptr)
            {
                return std::string();
            }
            return std::filesystem::path(ownModule.dli_fname).parent_path().string();
#endif
        }

#ifdef _WIN32
        std::string DescribeLastWin32Failure()
        {
            constexpr int ErrorCodeHexWidth = 8;
            std::ostringstream failureReason;
            failureReason << "Win32 错误 0x" << std::hex << std::uppercase << std::setfill('0')
                          << std::setw(ErrorCodeHexWidth) << static_cast<unsigned long>(::GetLastError());
            return failureReason.str();
        }

        BackendCreateFunction FindBackendCreateFunction(HMODULE openedModule)
        {
            return reinterpret_cast<BackendCreateFunction>(::GetProcAddress(openedModule, DbBackendCreateSymbolName));
        }
#else
        BackendCreateFunction FindBackendCreateFunction(void* openedModule)
        {
            return reinterpret_cast<BackendCreateFunction>(::dlsym(openedModule, DbBackendCreateSymbolName));
        }
#endif

        struct OpenedBackendModule
        {
            void* ModuleHandle = nullptr;
            BackendCreateFunction CreateFunction = nullptr;
            std::string FailureReason;
        };

        // 模块句柄在本进程内永不释放: 后端对象的虚表与驱动内部的静态状态要活到进程结束, 卸载只会换来
        // 之后的崩溃. 故这里只持有、不使用.
        OpenedBackendModule OpenBackendModule(const std::string& modulePathOrBareName)
        {
            OpenedBackendModule opened;

#ifdef _WIN32
            const HMODULE openedModule =
                ::LoadLibraryExA(modulePathOrBareName.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (openedModule == nullptr)
            {
                opened.FailureReason = DescribeLastWin32Failure();
                return opened;
            }
#else
            // dlerror 是粘的: 不先清一次, 下面读到的可能是上一步留下的旧错.
            ::dlerror();

            // RTLD_NOW 而非 RTLD_LAZY: 缺依赖要在装载这一刻就露头, 与 Windows 上 LoadLibrary 急切解析
            // 导入表的行为对齐. RTLD_LOCAL: 不把后端的符号倒进全局名字空间.
            void* const openedModule = ::dlopen(modulePathOrBareName.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (openedModule == nullptr)
            {
                const char* const openFailure = ::dlerror();
                opened.FailureReason = openFailure != nullptr ? openFailure : "dlopen 未给出原因";
                return opened;
            }
#endif

            opened.ModuleHandle = openedModule;
            opened.CreateFunction = FindBackendCreateFunction(openedModule);
            if (opened.CreateFunction == nullptr)
            {
                opened.FailureReason = std::string("已装载但未导出 ") + DbBackendCreateSymbolName
                    + ", 该模块与本装载器版本不匹配";
            }
            return opened;
        }

        struct BackendModuleSearchResult
        {
            OpenedBackendModule Opened;
            std::string ResolvedModulePath;
            std::string AttemptsReport;
        };

        // 每一步的路径与原因都留在 AttemptsReport 里 —— Linux 上第一步通常落空, 只报最后一次的话,
        // "缺的是哪个依赖库"会被后一次的"模块找不到"盖掉.
        BackendModuleSearchResult SearchBackendModule(const std::array<std::string, 2>& candidateModulePaths)
        {
            BackendModuleSearchResult searchResult;
            for (const std::string& candidateModulePath : candidateModulePaths)
            {
                if (candidateModulePath.empty())
                {
                    continue;
                }

                const OpenedBackendModule opened = OpenBackendModule(candidateModulePath);
                if (opened.ModuleHandle != nullptr)
                {
                    searchResult.Opened = opened;
                    searchResult.ResolvedModulePath = candidateModulePath;
                    return searchResult;
                }

                if (!searchResult.AttemptsReport.empty())
                {
                    searchResult.AttemptsReport += "; ";
                }
                searchResult.AttemptsReport += candidateModulePath + " -> " + opened.FailureReason;
            }
            return searchResult;
        }
    }

    Db* LoadDatabaseBackend(const std::string& backendModuleBaseName, const std::string& connectionTarget, const std::string& userName, const std::string& password)
    {
        const std::string moduleFilename = DatabaseBackendModuleFilename(backendModuleBaseName);
        const std::string moduleDirectory = CurrentModuleDirectoryPath();
        // 两步次序: 先"本模块所在目录 + 文件名", 再裸文件名. Windows 上第一步非走不可 —— 显式
        // LoadLibraryEx 的标准搜索序不含调用方自己的目录, 宿主是 python.exe 时进程目录是解释器目录
        // 而非引擎根. Linux 上第一步通常落空, 第二步的裸名 dlopen 用调用方 RUNPATH 解析, 与链结期
        // DT_NEEDED 的规则逐字相同, 故失败模式不变.
        const std::array<std::string, 2> candidateModulePaths = {
            moduleDirectory.empty() ? std::string() : (std::filesystem::path(moduleDirectory) / moduleFilename).string(),
            moduleFilename
        };

        const BackendModuleSearchResult searchResult = SearchBackendModule(candidateModulePaths);
        if (searchResult.Opened.ModuleHandle == nullptr)
        {
            throw std::runtime_error("数据库后端模块加载失败: " + searchResult.AttemptsReport);
        }
        if (searchResult.Opened.CreateFunction == nullptr)
        {
            throw std::runtime_error("数据库后端模块 " + searchResult.ResolvedModulePath + ": " + searchResult.Opened.FailureReason);
        }

        char failureText[BackendFailureTextCapacity] = { 0 };
        Db* const backend = searchResult.Opened.CreateFunction(connectionTarget.c_str(), userName.c_str(), password.c_str(), failureText, BackendFailureTextCapacity);
        if (backend == nullptr)
        {
            throw std::runtime_error(std::string("数据库后端构造失败 (") + searchResult.ResolvedModulePath + "): " + failureText);
        }
        return backend;
    }

    Db* LoadDatabaseBackend(DbTypeType dbType, const std::string& connectionTarget, const std::string& userName, const std::string& password)
    {
        const char* const backendModuleBaseName = BackendModuleBaseName(dbType);
        if (backendModuleBaseName == nullptr)
        {
            throw std::runtime_error("未知的数据库后端种类: " + std::to_string(static_cast<int>(dbType)));
        }
        return LoadDatabaseBackend(backendModuleBaseName, connectionTarget, userName, password);
    }
}
