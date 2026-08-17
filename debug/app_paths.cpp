/**
 * app_paths.cpp
 *
 * Resolution of the executable directory and the per-user data directory.
 *
 * Deliberately free of the logging macros: this is what logging itself calls to
 * decide where to put its files, so anything here that tried to log would be
 * reaching into a module that has not been initialized yet.
 */

#include "debug/app_paths.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif


namespace
{
    std::mutex  f_mutex;
    std::string f_folderName = "Dartmatic";

    /**
     * Cached because it cannot change while the process runs, and because the
     * logging module asks for it on every logger it creates.
     */
    const std::string& exeDir()
    {
        static const std::string dir = []() -> std::string
        {
#ifdef _WIN32
            // MAX_PATH is not the limit on modern Windows, so grow until the
            // call stops truncating rather than assuming a size.
            std::wstring buf(512, L'\0');
            for(;;)
            {
                const DWORD n = GetModuleFileNameW(nullptr, buf.data(),
                                                   static_cast<DWORD>(buf.size()));
                if(n == 0) return ".";
                if(n < buf.size())
                {
                    buf.resize(n);
                    break;
                }
                buf.resize(buf.size() * 2);
            }
            std::error_code ec;
            const std::filesystem::path p(buf);
            return p.parent_path().string();
#else
            std::error_code ec;
            const std::filesystem::path p =
                std::filesystem::read_symlink("/proc/self/exe", ec);
            if(ec) return ".";
            return p.parent_path().string();
#endif
        }();
        return dir;
    }


    /** The per-user data root, without the application folder appended. */
    std::string userDataRoot()
    {
#ifdef _WIN32
        // getenv rather than SHGetKnownFolderPath: one fewer library to link
        // into a headless server, and LOCALAPPDATA is set in every session a
        // user can actually log into.
        if(const char* local = std::getenv("LOCALAPPDATA"); local && *local)
        {
            return local;
        }
        if(const char* profile = std::getenv("USERPROFILE"); profile && *profile)
        {
            return std::string(profile) + "\\AppData\\Local";
        }
        return exeDir();
#else
        if(const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        {
            return xdg;
        }
        if(const char* home = std::getenv("HOME"); home && *home)
        {
            return std::string(home) + "/.local/share";
        }
        return exeDir();
#endif
    }


    std::string join(const std::string& base, const std::string& rel)
    {
        if(base.empty()) return rel;
        if(rel.empty())  return base;

        std::filesystem::path p(base);
        p /= rel;
        return p.lexically_normal().string();
    }
}


void setAppDataFolderName(const std::string& name)
{
    std::lock_guard<std::mutex> lock(f_mutex);
    if(!name.empty()) f_folderName = name;
}


bool isPortableInstall()
{
    // Resolved once: the marker is not expected to appear or vanish while the
    // program runs, and this is consulted on every path lookup.
    static const bool portable = []()
    {
        std::error_code ec;
        return std::filesystem::exists(join(exeDir(), "portable.txt"), ec);
    }();
    return portable;
}


std::string executableDirectory()
{
    return exeDir();
}


std::string appAssetPath(const std::string& relative)
{
    return join(exeDir(), relative);
}


std::string appDataPath(const std::string& relative)
{
    std::string base;
    if(isPortableInstall())
    {
        base = exeDir();
    }
    else
    {
        std::string folder;
        {
            std::lock_guard<std::mutex> lock(f_mutex);
            folder = f_folderName;
        }
#ifdef _WIN32
        base = join(userDataRoot(), folder);
#else
        // Lower-case on Linux, where a capitalised directory in ~/.local/share
        // looks out of place next to everything else there.
        std::string lower = folder;
        for(char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        base = join(userDataRoot(), lower);
#endif
    }

    const std::string full = join(base, relative);

    // Create the parent so every caller does not have to. A path with no
    // extension is treated as a directory in its own right — that is what
    // callers asking for "captures" or "logs" mean.
    std::error_code ec;
    const std::filesystem::path p(full);
    if(p.has_extension())
    {
        if(p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    }
    else
    {
        std::filesystem::create_directories(p, ec);
    }

    return full;
}
