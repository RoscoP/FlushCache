// FlushCache
//      This tool is used to flush a specific file or directory (and all files and subdirectories) from the file
//      cache.  This is done to ensure the file cache isn't filled when doing performance tests for loading time.
//
#include "stdafx.h"
#include <windows.h>

#include <string>
#include <list>
#include <vector>
#include <thread>

typedef std::list<const std::wstring> FileList;

// Flags set by ParseArgs
bool gVerbose = false;
bool gShowErrors = true;
TCHAR gStartFileOrDir[MAX_PATH];

// Cheap, but the allocation/deallocation cost of a vector is huge, lets just push the iterator through
FileList::const_iterator GetIteratorOffset(const FileList::const_iterator& start, size_t offset)
{
    FileList::const_iterator itr = start;
    while (offset--)
    {
        ++itr;
    }

    return itr;
}

// Flushes a specific file
bool FlushFile(const _TCHAR* file)
{
    bool ret = false;

    // Using FILE_FLAG_NO_BUFFERING causes the file cache to be flushed from the system
    // Found this as a solution via Stack Overflow: http://stackoverflow.com/a/7113153
    HANDLE hFile = CreateFile(file, GENERIC_READ, 0 /* no share */, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        if (gVerbose)
        {
            printf("Flushed: %S\n", file);
        }
        CloseHandle(hFile);
        ret = true;
    }
    else if (gShowErrors)
    {
        printf("Error: %d flushing: %S\n", GetLastError(), file);
    }

    return ret;
}

// Recurses the given directory and creates a list of files
bool RecurseDirectory(const _TCHAR* dirPath, FileList& fileList)
{
    WIN32_FIND_DATA findData = { 0 };
    TCHAR fullPath[MAX_PATH] = { 0 };
    int len = _tcslen(dirPath) + 2;
    if (len >= MAX_PATH)
    {
        if (gShowErrors)
        {
            printf("Error: descending into directory %S - path too long (%d)\n", dirPath, len);
        }
        return false;
    }
    _tcscpy_s(fullPath, dirPath);
    _tcscat_s(fullPath, TEXT("\\"));
    _tcscat_s(fullPath, TEXT("*"));
    HANDLE hFind = FindFirstFile(fullPath, &findData);
    LARGE_INTEGER filesize = { 0 };

    if (hFind == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    do
    {
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (_tcscmp(findData.cFileName, TEXT(".")) == 0 ||
                _tcscmp(findData.cFileName, TEXT(".."))  == 0)
            {
                continue;
            }
            std::wstring fullDirPath = dirPath;
            fullDirPath += TEXT("\\");
            fullDirPath += findData.cFileName;

            RecurseDirectory(fullDirPath.c_str(), fileList);
        }
        else
        {
            std::wstring fullFilePath = dirPath;
            fullFilePath += TEXT("\\");
            fullFilePath += findData.cFileName;

            fileList.push_back(fullFilePath);
        }
    } while (FindNextFile(hFind, &findData) != 0);

    FindClose(hFind);

    return true;
}

// Um, prints help.
void PrintHelp()
{
    printf("FlushCache - This will flush the OS file cache for any passed in file,\n");
    printf("             or all files contained in a directory.\n");
    printf("\n");
    printf("  [TARGET] : Optional name of file or directory to use. If none specified\n");
    printf("             it will use current directory.\n");
    printf("  -h       : Show help.\n");
    printf("  -v       : Verbose - Show status messages (files as they are processed).\n");
    printf("  -q       : Quiet   - Don't print errors as they happen.\n");
    printf("\n");
}

// Parses args and puts them into global settings
bool ParseArgs(int argc, _TCHAR* argv[])
{
    if (argc <= 1)
    {
        return 1;
    }

    for (int i = 1; i < argc; ++i)
    {
        if (argv[i][0] != '\0' && argv[i][0] != '/' && argv[i][0] != '-')
        {
            _tcscpy_s(gStartFileOrDir, argv[i]);
            continue;
        }
        
        switch (argv[i][1])
        {
        case 'v':
            gVerbose = true;
            break;
        case 'q':
            gShowErrors = false;
            break;
        case 'h':
            PrintHelp();
            return false;
            break;
        default:
            printf("Unknown option passed in: %S\n", argv[i]);
            PrintHelp();
            return false;
            break;
        }
    }

    return true;
}

// Lifted and modified from: http://msdn.microsoft.com/en-us/library/windows/desktop/ms683194(v=vs.85).aspx
// Get logical count (including hyperthreaded).  If function is unsupported (XPSP2), return 2 as the logical count.
size_t GetCoreCount()
{
    typedef BOOL(WINAPI *LPFN_GLPI)(
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION,
        PDWORD);
    
    // Helper function to count set bits in the processor mask.
    auto CountSetBits = [](ULONG_PTR bitMask)
    {
        DWORD LSHIFT = sizeof(ULONG_PTR)* 8 - 1;
        DWORD bitSetCount = 0;
        ULONG_PTR bitTest = (ULONG_PTR)1 << LSHIFT;
        DWORD i;

        for (i = 0; i <= LSHIFT; ++i)
        {
            bitSetCount += ((bitMask & bitTest) ? 1 : 0);
            bitTest /= 2;
        }

        return bitSetCount;
    };

    const size_t defaultProcessorCount = 2;
    LPFN_GLPI glpi;
    BOOL done = FALSE;
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer = NULL;
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION ptr = NULL;
    DWORD returnLength = 0;
    DWORD logicalProcessorCount = 0;
    DWORD numaNodeCount = 0;
    DWORD processorCoreCount = 0;
    DWORD byteOffset = 0;

    glpi = (LPFN_GLPI)GetProcAddress(GetModuleHandle(TEXT("kernel32")),"GetLogicalProcessorInformation");
    // This most likely means we are on XP SP2 (GetLogicalProcessorInformation is supported on XP SP3+)
    // Be nice and say we have 2 (and hope we do)
    if (!glpi)
    {
        return (defaultProcessorCount);
    }

    do
    {
        DWORD rc = glpi(buffer, &returnLength);

        if (FALSE == rc)
        {
            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
            {
                if (buffer)
                {
                    free(buffer);
                }

                buffer = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)malloc(returnLength);
            }
            else
            {
                return (defaultProcessorCount);
            }
        }
        else
        {
            done = TRUE;
        }
    } while (!done);

    ptr = buffer;

    while (byteOffset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) <= returnLength)
    {
        switch (ptr->Relationship)
        {
        case RelationProcessorCore:
            processorCoreCount++;

            // A hyperthreaded core supplies more than one logical processor.
            logicalProcessorCount += CountSetBits(ptr->ProcessorMask);
            break;
        }
        byteOffset += sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
        ptr++;
    }

    free(buffer);

    return logicalProcessorCount > 0 ? logicalProcessorCount : 1;

}

// This is another way to flush the entire drive, much faster than opening every file
// Found here: http://stackoverflow.com/questions/7405868/how-to-invalidate-the-file-system-cache
bool FlushDrive(const TCHAR* drive)
{
    std::wstring fullDrivePath = TEXT("\\\\.\\");   // Volume prefix
    fullDrivePath += drive;

    // Open the volume handle
    HANDLE hFile = CreateFile(fullDrivePath.c_str(), FILE_READ_DATA, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hFile);
    }
    else
    {
        DWORD gle = GetLastError();
        if (gle != ERROR_SHARING_VIOLATION && gle != ERROR_ACCESS_DENIED)
        {
            printf("Error %d clearing %S\n", gle, drive);
            return false;
        }
    }

    return true;
}

// Main entry point
int _tmain(int argc, _TCHAR* argv[])
{
    int ret = 0;

    // Don't show an error dialog if there is a problem
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG);

    // Only allowing one right now.
    HANDLE hMutex = CreateMutex(NULL, FALSE, TEXT("FlushCache"));
    if (hMutex == INVALID_HANDLE_VALUE || hMutex == NULL || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        return 4;
    }

    bool keepGoing = ParseArgs(argc, argv);
    if (!keepGoing)
    {
        return 3;
    }

    DWORD fileAttr = 0;

    // If nothing passed in, used current directory
    if (!gStartFileOrDir[0])
    {
        GetCurrentDirectory(MAX_PATH, gStartFileOrDir);
        fileAttr = FILE_ATTRIBUTE_DIRECTORY;
    }
    else
    {
        // Otherwise figure out if this is a directory or a file
        fileAttr = GetFileAttributes(gStartFileOrDir);
    }

    if (fileAttr == (~0u))
    {
        if (gShowErrors)
        {
            printf("File or directory not found: %S\n", gStartFileOrDir);
        }
        ret = 2;
    }
    else if (fileAttr & FILE_ATTRIBUTE_DIRECTORY)
    {
        int len = _tcslen(gStartFileOrDir);
        // Use the drive method only if drive is passed in (not c:\ - that will use directory method)
        bool isDrive = len == 2 && gStartFileOrDir[1] == TEXT(':');

        if (isDrive)
        {
            if (gVerbose)
            {
                printf("Flushing drive\n");
            }

            bool flushDriveSuccess = FlushDrive(gStartFileOrDir);
            ret = flushDriveSuccess ? ret : 5;
        }
        else
        {
            if (gVerbose)
            {
                printf("Gathering files to flush\n");
            }

            FileList fileList;
            RecurseDirectory(gStartFileOrDir, fileList);

            const int chunkDivizor = GetCoreCount();
            int chunk = fileList.size() / chunkDivizor;

            // Split up the files into equal sets and let the threads take care of them
            auto loopFunction = [](FileList::const_iterator start, FileList::const_iterator end)
            {
                for (FileList::const_iterator itr = start; itr != end; ++itr)
                {
                    FlushFile(itr->c_str());
                }
            };

            std::vector<std::thread> threads(chunkDivizor);
            for (int i = 0; i < chunkDivizor; ++i)
            {
                FileList::const_iterator start = GetIteratorOffset(fileList.begin(), (i*chunk));
                int countLeft = fileList.size() - (i*chunk);
                int chunkToUse = countLeft > chunk ? chunk : countLeft;
                if (i == chunkDivizor - 1)
                {
                    // use up the last few that didn't make the divisor
                    chunkToUse = countLeft;
                }
                FileList::const_iterator end = GetIteratorOffset(start, chunkToUse);
                threads[i] = std::thread(loopFunction, start, end);
            }

            for (auto& t : threads)
            {
                t.join();
            }
        }
    }
    else
    {
        FlushFile(gStartFileOrDir);
    }

	return ret;
}

