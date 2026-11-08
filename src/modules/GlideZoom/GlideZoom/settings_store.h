#pragma once
#include <windows.h>
#include <shlobj.h>
#include <string>
#include "settings.h"
#include "powertoys_settings.h"

namespace glide::settings
{
    enum class LoadResult
    {
        Loaded,
        Missing,
        Invalid,
        Unavailable
    };
    enum class SaveResult
    {
        Saved,
        Invalid,
        ChangedOutside,
        Unavailable
    };

    class Store
    {
        struct File
        {
            HANDLE handle = INVALID_HANDLE_VALUE;
            File() = default;
            explicit File(HANDLE h) :
                handle(h) {}
            File(const File&) = delete;
            File& operator=(const File&) = delete;
            ~File() { Close(); }
            void Close() noexcept
            {
                if (handle != INVALID_HANDLE_VALUE)
                    CloseHandle(handle);
                handle = INVALID_HANDLE_VALUE;
            }
            explicit operator bool() const noexcept { return handle != INVALID_HANDLE_VALUE; }
        };

    public:
        explicit Store(bool powerToys = false) :
            powerToys_(powerToys) {}

        LoadResult Load(Values& output)
        {
            loaded_ = false;
            output = {};
            PWSTR folder = nullptr;
            if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder)))
                return LoadResult::Unavailable;
            try
            {
                directory_ = std::wstring(folder) + (powerToys_ ? L"\\Microsoft\\PowerToys\\GlideZoom" : L"\\GlideZoom");
            }
            catch (...)
            {
                CoTaskMemFree(folder);
                throw;
            }
            CoTaskMemFree(folder);
            path_ = directory_ + (powerToys_ ? L"\\settings.json" : L"\\settings.ini");
            if (!DirectorySafe(false) || !Read(original_, existed_))
                return LoadResult::Unavailable;
            loaded_ = true;
            if (!existed_)
                return LoadResult::Missing;
            return (powerToys_ ? ParsePowerToys(original_, output) : Parse(original_, output)) ? LoadResult::Loaded : LoadResult::Invalid;
        }

        SaveResult Save(const Values& values)
        {
            std::string bytes = powerToys_ ? SerializePowerToys(values) : Serialize(values);
            if (bytes.empty())
                return SaveResult::Invalid;
            if (!loaded_ || !DirectorySafe(true))
                return SaveResult::Unavailable;
            // Keep this directory from being renamed while its child is replaced.
            File directory(CreateFileW(directory_.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            BY_HANDLE_FILE_INFORMATION info{};
            if (!directory || !GetFileInformationByHandle(directory.handle, &info) ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                return SaveResult::Unavailable;
            std::string current;
            bool exists = false;
            if (!Read(current, exists))
                return SaveResult::Unavailable;
            if (exists != existed_ || current != original_)
                return SaveResult::ChangedOutside;

            // CREATE_NEW never truncates an existing temporary file. A failed/crashed
            // save may leave its named temp file; the last committed settings survive.
            std::wstring temporary;
            File file;
            for (unsigned attempt = 0; attempt < 16; ++attempt)
            {
                temporary = path_ + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt);
                file.handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (file || GetLastError() != ERROR_FILE_EXISTS)
                    break;
            }
            if (!file)
                return SaveResult::Unavailable;
            DWORD written = 0;
            const bool ready = WriteFile(file.handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                               written == bytes.size() && FlushFileBuffers(file.handle);
            file.Close();
            // Repeat optimistic conflict detection immediately before commit. Windows
            // has no file-replace API conditional on the old contents; concurrent
            // editors must still coordinate. No unrelated file is opened for writing.
            if (!ready || !Read(current, exists))
            {
                DeleteFileW(temporary.c_str());
                return SaveResult::Unavailable;
            }
            if (exists != existed_ || current != original_)
            {
                DeleteFileW(temporary.c_str());
                return SaveResult::ChangedOutside;
            }
            const DWORD flags = MOVEFILE_WRITE_THROUGH | (existed_ ? MOVEFILE_REPLACE_EXISTING : 0);
            if (!MoveFileExW(temporary.c_str(), path_.c_str(), flags))
            {
                DeleteFileW(temporary.c_str());
                return SaveResult::Unavailable;
            }
            // swap cannot allocate after the disk commit.
            original_.swap(bytes);
            existed_ = true;
            return SaveResult::Saved;
        }

    private:
        bool DirectorySafe(bool create) const noexcept
        {
            DWORD attributes = GetFileAttributesW(directory_.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
            {
                const DWORD error = GetLastError();
                if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                    return false;
                if (!create)
                    return true;
                if (!CreateDirectoryW(directory_.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
                    return false;
                attributes = GetFileAttributesW(directory_.c_str());
            }
            return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
                   !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
        }
        bool Read(std::string& bytes, bool& exists) const
        {
            bytes.clear();
            exists = false;
            File file(CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!file)
                return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
            BY_HANDLE_FILE_INFORMATION info{};
            if (GetFileType(file.handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(file.handle, &info) ||
                (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
                info.nFileSizeHigh != 0 || info.nFileSizeLow > 16384)
                return false;
            bytes.resize(info.nFileSizeLow);
            DWORD read = 0;
            if (!ReadFile(file.handle, bytes.data(), info.nFileSizeLow, &read, nullptr) || read != info.nFileSizeLow)
                return false;
            exists = true;
            return true;
        }
        std::wstring directory_, path_;
        std::string original_;
        bool loaded_ = false, existed_ = false;
        bool powerToys_ = false;
    };
} // namespace glide::settings
