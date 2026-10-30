// fw-backup.cpp — минималистичный монитор папок в трее.
// Следит за списком пар «источник → приёмник» и зеркалирует изменения:
// изменившийся файл копируется в соответствующее место приёмника после
// DebounceMs мс «тишины» (защита от файлов, дописываемых по частям).
//
// Конфиг: HKCU\Software\FwBackup (см. README.md)
//
// Сборка (MinGW-w64):
//   g++ -O2 -s -mwindows fw-backup.cpp -o fw-backup.exe -lshlwapi -static -static-libgcc -static-libstdc++
// Сборка (MSVC):
//   cl /O1 /MD fw-backup.cpp /Fe:fw-backup.exe /link shlwapi.lib

#define _WIN32_WINNT 0x0601
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <mutex>

#if defined(_MSC_VER)
#pragma comment(lib, "shlwapi.lib")
#endif

// ============================ константы ============================

static const wchar_t kAppKey[]    = L"Software\\FwBackup";
static const wchar_t kMutexName[] = L"Local\\FwBackupSingle";
static const wchar_t kRunKey[]    = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t kRunValue[]  = L"FwBackup";
static const wchar_t kArchDir[]   = L"!src_deleted";   // архив удалённого — в корне приёмника («!» гонит папку наверх сортировки)

static const UINT WM_TRAY   = WM_APP + 1;  // сообщения иконки трея
static const UINT WM_UPDTIP = WM_APP + 2;  // обновить подсказку иконки

enum { MENU_SETTINGS = 1, MENU_SYNC = 2, MENU_PAUSE = 3, MENU_RELOAD = 4, MENU_EXIT = 5 };

// элементы окна настроек
enum {
    IDC_PAIRS = 2001, IDC_EXCL, IDC_DEB, IDC_LOG,
    IDC_AUTOSTART, IDC_ARCHIVE, IDC_KEEP, IDC_SKIPFULL,
    IDC_SAVE, IDC_CANCEL
};

static const int kBufSize = 64 * 1024;     // буфер событий на пару
static const int kRetries = 3;             // повторов при копировании/удалении

// ============================ типы и глобальное состояние ============================

struct PairCfg { std::wstring src, dst; };

struct Settings {
    std::vector<PairCfg> pairs;
    std::vector<std::wstring> excludes;   // шаблоны: с '\' — сверяются с путём, без — с именем
    DWORD debounceMs = 3000;              // мс тишины перед копированием
    bool  keepOrphans = false;            // не удалять на приёмнике то, что исчезло в источнике
    bool  archiveDeleted = true;          // удалённое — перемещать в !src_deleted, а не стирать
    bool  skipFullSync = false;           // не делать полную сверку при старте пары
    std::wstring logFile;                 // пусто = журнал не ведётся
};

struct Watcher;   // вперёд, нужен для_running-проверок в сверке

static std::mutex gLogMx;
static std::wstring gLogFile;             // копия Settings::logFile (пишут любые нити)

static std::mutex gCfgMx;
static Settings gSettings;                // меняется только под gCfgMx

static volatile LONG gRunning  = 1;       // 0 = выходим
static volatile LONG gBusySync = 0;       // идёт сверка/перезагрузка конфига
static volatile LONG gCfgVersion = 1;     // растёт при перезагрузке конфига
static bool gPaused = false;              // меняет только поток интерфейса

static HWND  gWnd = nullptr;
static UINT  gTaskbarCreatedMsg = 0;
static DWORD gMainThreadId = 0;
static std::wstring gExePath;

static HINSTANCE gHInst = nullptr;      // устанавливается в WinMain
static HFONT    gUiFont = nullptr;      // шрифт окна настроек
static HWND     gSettingsWnd = nullptr; // открытое окно настроек

static bool RunningW(const Watcher* w);

// ============================ утилиты строк/путей ============================

static std::wstring LowerCopy(const std::wstring& s) {
    std::wstring r = s;
    if (!r.empty()) CharLowerBuffW(&r[0], (DWORD)r.size());
    return r;
}

static bool StartsWithI(const std::wstring& a, const std::wstring& b) {
    if (a.size() < b.size()) return false;
    return _wcsnicmp(a.c_str(), b.c_str(), b.size()) == 0;
}

// убираем завершающие '\', но корень "X:\" не трогаем
static std::wstring TrimSlash(std::wstring p) {
    while (p.size() >= 2 && p.back() == L'\\' && !(p.size() == 3 && p[1] == L':')) p.pop_back();
    return p;
}

static std::wstring JoinPath(const std::wstring& root, const std::wstring& rel) {
    if (rel.empty()) return root;
    return TrimSlash(root) + L"\\" + rel;
}

// длинные пути: D:\... → \\?\D:\..., UNC → \\?\UNC\...
static std::wstring MakeAbs(const std::wstring& p) {
    if (p.size() >= 4 && p[0] == L'\\' && p[1] == L'\\' && p[2] == L'?' && p[3] == L'\\') return p;
    if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\')
        return L"\\\\?\\UNC\\" + std::wstring(p.begin() + 2, p.end());
    return L"\\\\?\\" + p;
}

static std::wstring TimeStr() {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t b[40];
    swprintf(b, 40, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return b;
}

static void LogWrite(const std::wstring& msg) {
    std::lock_guard<std::mutex> lk(gLogMx);
    if (gLogFile.empty()) return;
    HANDLE h = CreateFileW(MakeAbs(gLogFile).c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    std::wstring line = TimeStr() + L"  " + msg + L"\r\n";
    int cb = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), NULL, 0, NULL, NULL);
    if (cb > 0) {
        std::string a((size_t)cb, 0);
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), &a[0], cb, NULL, NULL);
        DWORD wr = 0;
        WriteFile(h, a.data(), (DWORD)a.size(), &wr, NULL);
    }
    CloseHandle(h);
}

// ============================ реестр ============================

static bool RegGetString(const wchar_t* name, std::wstring& out) {
    DWORD type = 0, cb = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kAppKey, name, RRF_RT_REG_SZ, &type, NULL, &cb) != ERROR_SUCCESS ||
        cb < sizeof(wchar_t)) return false;
    std::vector<wchar_t> buf(cb / sizeof(wchar_t) + 1, 0);
    DWORD cb2 = (DWORD)buf.size() * sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, kAppKey, name, RRF_RT_REG_SZ, &type, buf.data(), &cb2) != ERROR_SUCCESS)
        return false;
    out.assign(buf.data());
    while (!out.empty() && out.back() == L'\0') out.pop_back();
    return true;
}

static bool RegGetMulti(const wchar_t* name, std::vector<std::wstring>& out) {
    DWORD type = 0, cb = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kAppKey, name, RRF_RT_REG_MULTI_SZ, &type, NULL, &cb) != ERROR_SUCCESS)
        return false;
    std::vector<wchar_t> buf(cb / sizeof(wchar_t) + 2, 0);
    DWORD cb2 = (DWORD)buf.size() * sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, kAppKey, name, RRF_RT_REG_MULTI_SZ, &type, buf.data(), &cb2) != ERROR_SUCCESS)
        return false;
    wchar_t* p = buf.data();
    while (*p) { out.emplace_back(p); p += out.back().size() + 1; }
    return true;
}

static bool RegGetDword(const wchar_t* name, DWORD& out) {
    DWORD type = 0, cb = sizeof(DWORD), v = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kAppKey, name, RRF_RT_REG_DWORD, &type, &v, &cb) != ERROR_SUCCESS)
        return false;
    out = v;
    return true;
}

static HKEY RegKeyForWrite() {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kAppKey, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return nullptr;
    return k;
}

static void RegWriteMulti(const wchar_t* name, const std::vector<std::wstring>& items) {
    std::vector<wchar_t> data;
    for (auto& s : items) { data.insert(data.end(), s.begin(), s.end()); data.push_back(L'\0'); }
    data.push_back(L'\0');
    HKEY k = RegKeyForWrite();
    if (k) {
        RegSetValueExW(k, name, 0, REG_MULTI_SZ, (const BYTE*)data.data(), (DWORD)(data.size() * sizeof(wchar_t)));
        RegCloseKey(k);
    }
}

static void RegWriteString(const wchar_t* name, const std::wstring& v) {
    HKEY k = RegKeyForWrite();
    if (k) {
        RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
}

static void RegWriteDword(const wchar_t* name, DWORD v) {
    HKEY k = RegKeyForWrite();
    if (k) {
        RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(DWORD));
        RegCloseKey(k);
    }
}

static bool IsAutostartOn() {
    DWORD type = 0, cb = 0;
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, &type, NULL, &cb) == ERROR_SUCCESS;
}

static void SetAutostart(bool on) {
    if (on) {
        HKEY k;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
            std::wstring cmd = L"\"" + gExePath + L"\"";
            RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                           (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(k);
        }
    } else {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            RegDeleteValueW(k, kRunValue);
            RegCloseKey(k);
        }
    }
}

// ============================ настройки ============================

static void LoadSettingsInto(Settings& s) {
    s = Settings{};
    std::vector<std::wstring> lines;
    RegGetMulti(L"Pairs", lines);
    for (const auto& ln : lines) {
        size_t pos = ln.find(L'|');
        if (pos == std::wstring::npos || pos == 0) continue;
        PairCfg pc;
        pc.src = TrimSlash(ln.substr(0, pos));
        pc.dst = TrimSlash(ln.substr(pos + 1));
        if (pc.src.empty() || pc.dst.empty()) continue;
        if (_wcsicmp(pc.src.c_str(), pc.dst.c_str()) == 0) continue;
        // защита от вложенности: источник внутри приёмника или наоборот — бесконечное зеркало
        if (StartsWithI(pc.dst + L"\\", pc.src + L"\\") || StartsWithI(pc.src + L"\\", pc.dst + L"\\")) continue;
        s.pairs.push_back(pc);
    }
    RegGetMulti(L"Excludes", s.excludes);
    DWORD d = 0;
    if (RegGetDword(L"DebounceMs", d) && d >= 100) s.debounceMs = d;
    if (RegGetDword(L"KeepOrphans", d)) s.keepOrphans = (d != 0);
    if (RegGetDword(L"ArchiveDeleted", d)) s.archiveDeleted = (d != 0);
    if (RegGetDword(L"SkipFullSync", d)) s.skipFullSync = (d != 0);
    RegGetString(L"LogFile", s.logFile);
}

// ============================ работа с файлами ============================

static DWORD AttrOf(const std::wstring& p) { return GetFileAttributesW(MakeAbs(p).c_str()); }
static bool  DirExists(const std::wstring& p) {
    DWORD a = AttrOf(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// создаёт на приёмнике все промежуточные папки для rel (rel может быть "")
static bool EnsureDstDirs(const PairCfg& pc, const std::wstring& rel) {
    std::wstring cur = TrimSlash(pc.dst);
    // создаем и сам корень приёмника, если его ещё нет
    {
        DWORD ra = GetFileAttributesW(MakeAbs(cur).c_str());
        if (ra == INVALID_FILE_ATTRIBUTES) {
            if (!CreateDirectoryW(MakeAbs(cur).c_str(), NULL) &&
                GetLastError() != ERROR_ALREADY_EXISTS) {
                LogWrite(L"не удалось создать корень приёмника (err " +
                         std::to_wstring(GetLastError()) + L"): " + cur);
                return false;
            }
        } else if (ra & FILE_ATTRIBUTE_DIRECTORY) {
            // корень на месте
        } else {
            LogWrite(L"на месте корня приёмника находится файл: " + cur);
            return false;
        }
    }
    size_t i = 0;
    for (;;) {
        size_t j = rel.find(L'\\', i);
        std::wstring part = rel.substr(i, (j == std::wstring::npos ? rel.size() : j) - i);
        if (!part.empty()) {
            cur = cur + L"\\" + part;
            DWORD a = GetFileAttributesW(MakeAbs(cur).c_str());
            if (a == INVALID_FILE_ATTRIBUTES) {
                if (!CreateDirectoryW(MakeAbs(cur).c_str(), NULL) &&
                    GetLastError() != ERROR_ALREADY_EXISTS) {
                    LogWrite(L"не удалось создать папку: " + cur +
                             L" (err " + std::to_wstring(GetLastError()) + L")");
                    return false;
                }
            }
        }
        if (j == std::wstring::npos) break;
        i = j + 1;
    }
    return true;
}

static std::wstring ParentRel(const std::wstring& rel) {
    size_t j = rel.find_last_of(L'\\');
    if (j == std::wstring::npos) return L"";
    return rel.substr(0, j);
}

// копия с повторами (файл может быть занят другим приложением)
static bool CopyRel(const PairCfg& pc, const std::wstring& rel) {
    std::wstring src = JoinPath(pc.src, rel), dst = JoinPath(pc.dst, rel);
    std::wstring srcA = MakeAbs(src), dstA = MakeAbs(dst);
    for (int attempt = 0;; ++attempt) {
        EnsureDstDirs(pc, ParentRel(rel));
        if (CopyFileW(srcA.c_str(), dstA.c_str(), FALSE)) return true;
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            LogWrite(L"копирование прервано (err " + std::to_wstring(e) + L"): " + src);
            return false;
        }
        if (attempt >= kRetries - 1) {
            LogWrite(L"не удалось копировать (err " + std::to_wstring(e) + L"): " + src);
            return false;
        }
        Sleep(500);
    }
}

static void DeleteFileRetry(const std::wstring& dstFile) {
    std::wstring dstA = MakeAbs(dstFile);
    for (int attempt = 0;; ++attempt) {
        if (DeleteFileW(dstA.c_str())) return;
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return;
        if (attempt >= kRetries - 1) {
            LogWrite(L"не удалось удалить (err " + std::to_wstring(e) + L"): " + dstFile);
            return;
        }
        Sleep(500);
    }
}

// рекурсивное удаление дерева на приёмнике
static void DeleteTree(const PairCfg& pc, const std::wstring& rel) {
    std::wstring full = JoinPath(pc.dst, rel);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(MakeAbs(full + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            std::wstring childRel = rel + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                DeleteTree(pc, childRel);
            else
                DeleteFileRetry(JoinPath(pc.dst, childRel));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!RemoveDirectoryW(MakeAbs(full).c_str()))
        LogWrite(L"не удалить папку (err " + std::to_wstring(GetLastError()) + L"): " + full);
}

// суффикс " (n)": для файла — перед расширением, для папки — в конец имени
static std::wstring SuffixedPath(const std::wstring& path, const wchar_t* suf, bool isDir) {
    size_t slash = path.find_last_of(L'\\');
    size_t nameStart = (slash == std::wstring::npos) ? 0 : slash + 1;
    size_t extPos = path.find_last_of(L'.');
    bool splitExt = !isDir && extPos != std::wstring::npos && extPos > nameStart;
    std::wstring s = suf;
    if (splitExt) return path.substr(0, extPos) + L" (" + s + L")" + path.substr(extPos);
    return path + L" (" + s + L")";
}

// свободный путь архива, если место уже занято предыдущим удалением
static std::wstring UniqueArchTarget(const std::wstring& target, bool isDir) {
    if (GetFileAttributesW(MakeAbs(target).c_str()) == INVALID_FILE_ATTRIBUTES) return target;
    for (int n = 2; n < 10000; ++n) {
        std::wstring cand = SuffixedPath(target, std::to_wstring(n).c_str(), isDir);
        if (GetFileAttributesW(MakeAbs(cand).c_str()) == INVALID_FILE_ATTRIBUTES) return cand;
    }
    return target;
}

static void ArchiveOnDst(const PairCfg& pc, const std::wstring& rel);

// перемещение дерева в архив по одному файлу (когда перенести целиком не удалось)
static void MoveTreeFallback(const PairCfg& pc, const std::wstring& rel) {
    std::wstring dirPath = JoinPath(pc.dst, rel);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(MakeAbs(dirPath + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            ArchiveOnDst(pc, rel + L"\\" + fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!RemoveDirectoryW(MakeAbs(dirPath).c_str()))
        LogWrite(L"не удалить пустую папку (err " + std::to_wstring(GetLastError()) + L"): " + dirPath);
}

// удаление в источнике: копия перемещается в !src_deleted\<относительный путь>.
// Если ArchiveDeleted=0 — просто удаляется.
static void ArchiveOnDst(const PairCfg& pc, const std::wstring& rel) {
    if (rel.empty()) return;
    std::wstring dstPath = JoinPath(pc.dst, rel);
    std::wstring dstA = MakeAbs(dstPath);
    DWORD a = GetFileAttributesW(dstA.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;      // копии и так нет

    bool archDeleted;
    { std::lock_guard<std::mutex> g(gCfgMx); archDeleted = gSettings.archiveDeleted; }

    if (!archDeleted) {
        if (a & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(pc, rel);
        else DeleteFileRetry(dstPath);
        return;
    }

    bool isDir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    std::wstring archRel = JoinPath(kArchDir, rel);
    EnsureDstDirs(pc, ParentRel(archRel));         // папки внутри архива
    std::wstring target = JoinPath(pc.dst, archRel);
    if (GetFileAttributesW(MakeAbs(target).c_str()) != INVALID_FILE_ATTRIBUTES)
        target = UniqueArchTarget(target, isDir);  // уже архивировали такое раньше

    if (MoveFileW(dstA.c_str(), MakeAbs(target).c_str())) return;   // тот же том — мгновенно

    // перемещение целиком не вышло (блокировка и т.п.) — переносим содержимое пофайлово
    if (isDir) {
        MoveTreeFallback(pc, rel);
    } else {
        DWORD e = GetLastError();
        if (CopyFileW(dstA.c_str(), MakeAbs(target).c_str(), FALSE))
            DeleteFileRetry(dstPath);
        else
            LogWrite(L"не удалось заархивировать (err " + std::to_wstring(e) + L"): " + dstPath);
    }
}

// ============================ полная сверка (зеркало) ============================

struct VolInfo { std::wstring rel; FILETIME t; ULONGLONG size; bool dir; };
typedef std::unordered_map<std::wstring, VolInfo> IndexMap;

// шаблон с '\' сверяется с относительным путём, без '\' — с именем файла
static bool IsExcluded(const std::wstring& rel) {
    std::lock_guard<std::mutex> g(gCfgMx);
    if (gSettings.excludes.empty()) return false;
    std::wstring name = rel;
    size_t j = rel.find_last_of(L'\\');
    if (j != std::wstring::npos) name = rel.substr(j + 1);
    for (const auto& pat : gSettings.excludes) {
        if (pat.empty()) continue;
        if (pat.find(L'\\') != std::wstring::npos) {
            if (PathMatchSpecW(rel.c_str(), pat.c_str())) return true;
        } else {
            if (PathMatchSpecW(name.c_str(), pat.c_str())) return true;
        }
    }
    return false;
}

static bool ScanTree(const std::wstring& rootFull, const std::wstring& relPrefix,
                     bool skipArch, IndexMap& out, const Watcher* w);

// полная сверка одной пары; w == nullptr для ручной «Синхронизировать сейчас»
static void MirrorPair(PairCfg pc, Watcher* w) {
    bool keepOrphans;
    { std::lock_guard<std::mutex> g(gCfgMx); keepOrphans = gSettings.keepOrphans; }

    IndexMap srcIdx, dstIdx;
    if (!ScanTree(pc.src, L"", false, srcIdx, w)) { LogWrite(L"источник недоступен для сверки: " + pc.src); return; }
    if (!ScanTree(pc.dst, L"", true, dstIdx, w)) { LogWrite(L"приёмник недоступен для сверки: " + pc.dst); return; }
    if (DirExists(JoinPath(pc.src, kArchDir)))
        LogWrite(L"внимание: в источнике есть папка с именем " + std::wstring(kArchDir) +
                 L" — имя зарезервировано в корне приёмника, содержимое перемешается с архивом удалённого");

    // 1) папки, которых нет на приёмнике — создаём
    for (auto& kv : srcIdx) {
        if (!kv.second.dir) continue;
        auto it = dstIdx.find(kv.first);
        if (it != dstIdx.end() && it->second.dir) continue;
        if (!RunningW(w)) return;
        EnsureDstDirs(pc, kv.second.rel);
    }
    // 2) файлы: новые или изменившиеся (по размеру и времени)
    for (auto& kv : srcIdx) {
        if (kv.second.dir) continue;
        if (!RunningW(w)) return;
        auto it = dstIdx.find(kv.first);
        if (it != dstIdx.end() && !it->second.dir &&
            it->second.size == kv.second.size &&
            CompareFileTime(&it->second.t, &kv.second.t) == 0) continue;
        CopyRel(pc, kv.second.rel);
    }
    // 3) на приёмнике удаляем то, чего больше нет в источнике
    if (!keepOrphans) {
        for (auto& kv : dstIdx) {
            if (srcIdx.count(kv.first)) continue;
            if (!RunningW(w)) return;
            ArchiveOnDst(pc, kv.second.rel);
        }
    }
}

static bool ScanTree(const std::wstring& rootFull, const std::wstring& relPrefix,
                     bool skipArch, IndexMap& out, const Watcher* w) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(MakeAbs(rootFull + L"\\*").c_str(),
                                FindExInfoBasic, &fd, FindExSearchNameMatch, NULL,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring name = fd.cFileName;
        std::wstring childRel = relPrefix.empty() ? name : (relPrefix + L"\\" + name);
        if (relPrefix.empty() && skipArch && _wcsicmp(name.c_str(), kArchDir) == 0)
            continue;                       // архив удалённого не зеркалим сам на себя
        if (IsExcluded(childRel)) continue;
        VolInfo vi;
        vi.rel = childRel;
        vi.t = fd.ftLastWriteTime;
        vi.size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        vi.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        bool isReparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        out[LowerCopy(childRel)] = vi;
        if (vi.dir && !isReparse) {           // в symlink/junction не заходим (циклы)
            if (!RunningW(w)) { FindClose(h); return false; }
            ScanTree(JoinPath(rootFull, name), childRel, skipArch, out, w);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

// ============================ наблюдатель ============================

struct Watcher {
    PairCfg pair;
    int ver = 0;

    HANDLE hStop = nullptr;        // сигнал останова
    HANDLE evRead = nullptr;       // завершение overlapped-чтения (manual-reset)
    HANDLE evPend = nullptr;       // «есть отложенные изменения» (auto-reset)
    HANDLE hDir = nullptr;         // открытая папка источника
    HANDLE thWatch = nullptr;
    HANDLE thCopy = nullptr;
    std::vector<BYTE> buf = std::vector<BYTE>(kBufSize);

    std::mutex pendMx;
    std::map<std::wstring, std::wstring> pendCopy;    // lower(rel) → rel
    std::map<std::wstring, std::wstring> pendDelete;  // lower(rel) → rel
};

static std::vector<Watcher*> gWatchers;
static std::mutex gWatchMx;

static bool RunningW(const Watcher* w) {
    if (!w) return gRunning != 0;
    return gRunning != 0 && w->ver == gCfgVersion;
}

static bool HasPend(Watcher* w) {
    std::lock_guard<std::mutex> g(w->pendMx);
    return !w->pendCopy.empty() || !w->pendDelete.empty();
}

// появление/изменение → копия; удаление → удалить на приёмнике
static void OnEvent(Watcher* w, DWORD action, const std::wstring& rel) {
    if (rel.empty()) return;
    if (IsExcluded(rel)) return;
    int flag;
    switch (action) {
        case FILE_ACTION_ADDED:
        case FILE_ACTION_MODIFIED:
        case FILE_ACTION_RENAMED_NEW_NAME: flag = 1; break;
        case FILE_ACTION_REMOVED:
        case FILE_ACTION_RENAMED_OLD_NAME: flag = 2; break;
        default: return;
    }
    {
        std::lock_guard<std::mutex> g(w->pendMx);
        auto& m = (flag == 1) ? w->pendCopy : w->pendDelete;
        m[LowerCopy(rel)] = rel;
    }
    SetEvent(w->evPend);
}

static void HandleChanges(Watcher* w, BYTE* buf, DWORD bytes) {
    if (bytes == 0) return;    // переполнение обрабатывает вызывающий
    BYTE* p = buf;
    for (;;) {
        FILE_NOTIFY_INFORMATION* ni = (FILE_NOTIFY_INFORMATION*)p;
        size_t chars = ni->FileNameLength / sizeof(wchar_t);
        std::wstring rel(ni->FileName, chars);
        OnEvent(w, ni->Action, rel);
        if (ni->NextEntryOffset == 0 || ni->NextEntryOffset > bytes) break;
        p += ni->NextEntryOffset;
    }
}

// разбор отложенных изменений; финальное решение — по текущему состоянию источника
static void ProcessPend(Watcher* w) {
    std::map<std::wstring, std::wstring> cp, del;
    {
        std::lock_guard<std::mutex> g(w->pendMx);
        cp.swap(w->pendCopy);
        del.swap(w->pendDelete);
    }
    const PairCfg& pc = w->pair;

    // 1) копии
    for (auto& kv : cp) {
        if (!RunningW(w)) return;
        const std::wstring& rel = kv.second;
        std::wstring src = JoinPath(pc.src, rel);
        DWORD a = GetFileAttributesW(MakeAbs(src).c_str());
        if (a == INVALID_FILE_ATTRIBUTES) {
            ArchiveOnDst(pc, rel);          // источник уже исчез — это удаление
            continue;
        }
        if (a & FILE_ATTRIBUTE_DIRECTORY) { EnsureDstDirs(pc, rel); continue; }
        CopyRel(pc, rel);
    }
    // 2) удаления
    for (auto& kv : del) {
        if (!RunningW(w)) return;
        if (cp.count(kv.first)) continue;  // уже решено веткой копий
        const std::wstring& rel = kv.second;
        std::wstring src = JoinPath(pc.src, rel);
        if (GetFileAttributesW(MakeAbs(src).c_str()) != INVALID_FILE_ATTRIBUTES)
            continue;                      // источник снова есть — не трогаем
        ArchiveOnDst(pc, rel);
    }
}

static void MirrorPairBusy(PairCfg pc, Watcher* w);   // MirrorPair + busy-анимация
static void ProcessPendBusy(Watcher* w);              // ProcessPend + busy-анимация

// нить наблюдения за одной парой
static DWORD WINAPI WatchThread(LPVOID pv) {
    Watcher* w = (Watcher*)pv;
    PairCfg pc = w->pair;
    bool skipFull;
    { std::lock_guard<std::mutex> g(gCfgMx); skipFull = gSettings.skipFullSync; }

    static const DWORD kNotify =
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;

    HANDLE hDir = INVALID_HANDLE_VALUE;
    for (;;) {   // ждём, пока источник станет доступен (съёмный диск и т.п.)
        hDir = CreateFileW(MakeAbs(pc.src).c_str(), FILE_LIST_DIRECTORY,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
        if (hDir != INVALID_HANDLE_VALUE) break;
        if (!RunningW(w)) return 0;
        LogWrite(L"источник недоступен (err " + std::to_wstring(GetLastError()) + L"): " + pc.src);
        if (WaitForSingleObject(w->hStop, 10000) == WAIT_OBJECT_0) return 0;
    }

    w->hDir = hDir;
    OVERLAPPED ov = {}; ov.hEvent = w->evRead;

    // запрос событий ДО полной сверки — изменения во время сверки не теряются
    if (!ReadDirectoryChangesW(hDir, w->buf.data(), (DWORD)w->buf.size(), TRUE,
                               kNotify, NULL, &ov, NULL)) {
        LogWrite(L"ReadDirectoryChangesW не стартовал (err " +
                 std::to_wstring(GetLastError()) + L"): " + pc.src);
        CloseHandle(hDir);
        w->hDir = nullptr;
        return 0;
    }
    if (!skipFull) MirrorPairBusy(pc, w);   // исходное состояние приёмника (+ анимация)

    HANDLE hs[2] = { w->evRead, w->hStop };
    for (;;) {
        DWORD wr = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
        if (wr == WAIT_OBJECT_0 + 1) {     // стоп
            CancelIoEx(hDir, NULL);
            DWORD dummy = 0;
            GetOverlappedResult(hDir, &ov, &dummy, TRUE);
            break;
        }
        if (wr != WAIT_OBJECT_0) break;

        if (!RunningW(w)) break;
        DWORD bytes = 0;
        if (!GetOverlappedResult(hDir, &ov, &bytes, FALSE)) {
            LogWrite(L"ошибка чтения изменений (err " +
                     std::to_wstring(GetLastError()) + L"): " + pc.src);
            break;
        }
        HandleChanges(w, w->buf.data(), bytes);
        if (bytes == 0) {
            // буфер событий переполнился — сверим всё, что могли пропустить
            LogWrite(L"переполнение буфера событий, полная сверка: " + pc.src);
            MirrorPair(pc, w);
        }
        // перевзводим ожидание
        ResetEvent(w->evRead);
        if (!ReadDirectoryChangesW(hDir, w->buf.data(), (DWORD)w->buf.size(), TRUE,
                                   kNotify, NULL, &ov, NULL)) {
            LogWrite(L"ReadDirectoryChangesW не перезапустился (err " +
                     std::to_wstring(GetLastError()) + L"): " + pc.src);
            break;
        }
    }
    CloseHandle(hDir);
    w->hDir = nullptr;
    return 0;
}

// нить копирования одной пары: копирует только после DebounceMs мс тишины
static DWORD WINAPI CopyThread(LPVOID pv) {
    Watcher* w = (Watcher*)pv;
    ULONGLONG deadline = 0;

    for (;;) {
        if (!RunningW(w)) return 0;
        DWORD debounce;
        { std::lock_guard<std::mutex> g(gCfgMx); debounce = gSettings.debounceMs; }

        ULONGLONG now = GetTickCount64();
        DWORD waitMs = (deadline == 0 || now >= deadline) ? 400 : (DWORD)(deadline - now);

        DWORD r = WaitForSingleObject(w->evPend, waitMs);
        if (!RunningW(w)) return 0;

        if (r == WAIT_OBJECT_0) {
            if (gPaused) { deadline = 0; continue; }
            deadline = GetTickCount64() + debounce;
            continue;
        }
        // таймаут: тишина держится — выгружаем накопленное
        if (deadline && GetTickCount64() >= deadline) {
            deadline = 0;
            if (!gPaused && HasPend(w)) ProcessPendBusy(w);   // копируем с анимацией занятости
        }
    }
}

// ============================ управление нитями ============================

static Watcher* StartWatcher(const PairCfg& pc) {
    Watcher* w = new Watcher();
    w->pair = pc;
    w->ver = gCfgVersion;
    w->hStop  = CreateEventW(NULL, FALSE, FALSE, NULL);
    w->evRead = CreateEventW(NULL, TRUE,  FALSE, NULL);
    w->evPend = CreateEventW(NULL, FALSE, FALSE, NULL);
    w->thWatch = CreateThread(NULL, 0, WatchThread, w, 0, NULL);
    w->thCopy  = CreateThread(NULL, 0, CopyThread,  w, 0, NULL);
    std::lock_guard<std::mutex> g(gWatchMx);
    gWatchers.push_back(w);
    return w;
}

static void StopAllWatchers() {
    std::vector<Watcher*> list;
    { std::lock_guard<std::mutex> g(gWatchMx); list.swap(gWatchers); }
    for (auto* w : list) {
        SetEvent(w->hStop);
        SetEvent(w->evPend);
        if (w->hDir) CancelIoEx(w->hDir, NULL);
    }
    for (auto* w : list) {
        if (w->thWatch) { WaitForSingleObject(w->thWatch, 15000); CloseHandle(w->thWatch); }
        if (w->thCopy)  { WaitForSingleObject(w->thCopy,  15000); CloseHandle(w->thCopy);  }
        CloseHandle(w->hStop);
        CloseHandle(w->evRead);
        CloseHandle(w->evPend);
        delete w;
    }
}

static void WakeAllCopiers() {
    std::vector<Watcher*> list;
    { std::lock_guard<std::mutex> g(gWatchMx); list = gWatchers; }
    for (auto* w : list) SetEvent(w->evPend);
}

// busy-обёртки: помечают «идёт сверка/копирование» (анимация иконки), вызывая
// UpdateTip через WM_UPDTIP. Счётчик CRITICAL: если busy уже держит другая работа,
// просто выполняем работу без повторной анимации — см. InterlockedCompareExchange
static void MirrorPairBusy(PairCfg pc, Watcher* w) {
    if (InterlockedCompareExchange(&gBusySync, 1, 0) != 0) {
        MirrorPair(pc, w);          // busy уже держит ручная синхронизация — тихо работаем
        return;
    }
    PostMessageW(gWnd, WM_UPDTIP, 0, 0);   // взводим busy → вспыхнет анимация
    MirrorPair(pc, w);
    InterlockedDecrement(&gBusySync);
    PostMessageW(gWnd, WM_UPDTIP, 0, 0);   // busy снят — анимация догорит grace-тики
}
static void ProcessPendBusy(Watcher* w) {
    if (InterlockedCompareExchange(&gBusySync, 1, 0) != 0) {
        ProcessPend(w);             // busy уже держит другая работа — тихо копируем
        return;
    }
    PostMessageW(gWnd, WM_UPDTIP, 0, 0);   // взводим busy → анимация
    ProcessPend(w);
    InterlockedDecrement(&gBusySync);
    PostMessageW(gWnd, WM_UPDTIP, 0, 0);   // busy снят — анимация догорит grace-тики
}

// полная сверка всех пар (пункт меню) — отдельная нить
static DWORD WINAPI SyncNowThread(LPVOID) {
    if (InterlockedCompareExchange(&gBusySync, 1, 0) != 0) return 0;
    std::vector<PairCfg> snapshot;
    { std::lock_guard<std::mutex> g(gCfgMx); snapshot = gSettings.pairs; }
    SendMessageW(gWnd, WM_UPDTIP, 0, 0);
    for (auto& pc : snapshot) {
        if (gRunning == 0) break;
        MirrorPair(pc, nullptr);
    }
    InterlockedDecrement(&gBusySync);
    SendMessageW(gWnd, WM_UPDTIP, 0, 0);
    return 0;
}

// перечитать конфиг и пересоздать наблюдателей — отдельная нить
static DWORD WINAPI ReloadThread(LPVOID) {
    if (InterlockedCompareExchange(&gBusySync, 1, 0) != 0) return 0;
    SendMessageW(gWnd, WM_UPDTIP, 0, 0);
    StopAllWatchers();
    {
        std::lock_guard<std::mutex> g(gCfgMx);
        LoadSettingsInto(gSettings);
        gLogFile = gSettings.logFile;
    }
    InterlockedIncrement(&gCfgVersion);
    std::vector<PairCfg> snapshot;
    { std::lock_guard<std::mutex> g(gCfgMx); snapshot = gSettings.pairs; }
    for (auto& pc : snapshot) StartWatcher(pc);
    InterlockedDecrement(&gBusySync);
    SendMessageW(gWnd, WM_UPDTIP, 0, 0);
    return 0;
}

// ============================ окно настроек ============================

static std::wstring TrimWs(std::wstring s) {
    size_t b = s.find_first_not_of(L" \t");
    if (b == std::wstring::npos) return L"";
    size_t e = s.find_last_not_of(L" \t");
    return s.substr(b, e - b + 1);
}

static std::wstring GetEditText(HWND c) {
    int len = GetWindowTextLengthW(c);
    std::wstring s((size_t)len + 1, L'\0');
    GetWindowTextW(c, &s[0], len + 1);
    s.resize((size_t)len);
    return TrimWs(s);
}

static std::vector<std::wstring> GetEditLines(HWND e) {
    int len = GetWindowTextLengthW(e);
    std::wstring txt((size_t)len + 1, L'\0');
    if (len > 0) GetWindowTextW(e, &txt[0], len + 1);
    std::vector<std::wstring> out;
    size_t start = 0;
    for (;;) {
        size_t j = txt.find(L'\n', start);
        std::wstring line = (j == std::wstring::npos) ? txt.substr(start) : txt.substr(start, j - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        line = TrimWs(line);
        if (!line.empty()) out.push_back(line);
        if (j == std::wstring::npos) break;
        start = j + 1;
    }
    return out;
}

static HWND MkCtl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, DWORD ex,
                  int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id, gHInst, NULL);
    if (gUiFont) SendMessageW(c, WM_SETFONT, (WPARAM)gUiFont, TRUE);
    return c;
}

static bool BtnChecked(HWND h, int id) { return IsDlgButtonChecked(h, id) == BST_CHECKED; }

static void SettingsSave(HWND h) {
    std::vector<std::wstring> pairs;
    for (auto& ln : GetEditLines(GetDlgItem(h, IDC_PAIRS))) {
        size_t pos = ln.find(L'|');
        if (pos == std::wstring::npos || pos == 0 || pos + 1 == ln.size()) {
            MessageBoxW(h, L"Каждая строка в списке папок должна быть вида:\nисточник|приёмник\n\nНапример:\nD:\\my dox\\документы|E:\\backup\\документы",
                        L"fw-backup", MB_OK | MB_ICONWARNING);
            SetFocus(GetDlgItem(h, IDC_PAIRS));
            return;
        }
        pairs.push_back(ln);
    }
    if (pairs.empty()) {
        if (MessageBoxW(h, L"Список папок пуст — отслеживание будет отключено.\nПродолжить?",
                        L"fw-backup", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    }
    long deb = _wtoi(GetEditText(GetDlgItem(h, IDC_DEB)).c_str());
    if (deb < 100) deb = 100;
    if (deb > 600000) deb = 600000;

    RegWriteMulti(L"Pairs", pairs);
    RegWriteMulti(L"Excludes", GetEditLines(GetDlgItem(h, IDC_EXCL)));
    RegWriteDword(L"DebounceMs", (DWORD)deb);
    RegWriteString(L"LogFile", GetEditText(GetDlgItem(h, IDC_LOG)));
    RegWriteDword(L"ArchiveDeleted", BtnChecked(h, IDC_ARCHIVE) ? 1 : 0);
    RegWriteDword(L"KeepOrphans",    BtnChecked(h, IDC_KEEP) ? 1 : 0);
    RegWriteDword(L"SkipFullSync",   BtnChecked(h, IDC_SKIPFULL) ? 1 : 0);
    SetAutostart(BtnChecked(h, IDC_AUTOSTART));

    // настройки вступают в силу сразу: пересоздаём наблюдателей
    CloseHandle(CreateThread(NULL, 0, ReloadThread, NULL, 0, NULL));
    DestroyWindow(h);
}

static void SettingsFill(HWND h) {
    std::vector<PairCfg> pairs;
    std::vector<std::wstring> excl;
    DWORD deb = 3000;
    bool arch = true, keep = false, skip = false;
    std::wstring logf;
    {
        std::lock_guard<std::mutex> g(gCfgMx);
        pairs = gSettings.pairs; excl = gSettings.excludes;
        deb = gSettings.debounceMs; arch = gSettings.archiveDeleted;
        keep = gSettings.keepOrphans; skip = gSettings.skipFullSync;
        logf = gSettings.logFile;
    }
    std::wstring pt, et;
    for (auto& p : pairs) { if (!pt.empty()) pt += L"\r\n"; pt += p.src + L"|" + p.dst; }
    for (auto& e : excl)  { if (!et.empty()) et += L"\r\n"; et += e; }
    SetWindowTextW(GetDlgItem(h, IDC_PAIRS), pt.c_str());
    SetWindowTextW(GetDlgItem(h, IDC_EXCL), et.c_str());
    SetWindowTextW(GetDlgItem(h, IDC_DEB), std::to_wstring(deb).c_str());
    SetWindowTextW(GetDlgItem(h, IDC_LOG), logf.c_str());
    CheckDlgButton(h, IDC_AUTOSTART, IsAutostartOn() ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(h, IDC_ARCHIVE,   arch ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(h, IDC_KEEP,      keep ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(h, IDC_SKIPFULL,  skip ? BST_CHECKED : BST_UNCHECKED);
}

static LRESULT CALLBACK SettingsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wp) == IDC_SAVE)   { SettingsSave(h); return 0; }
            if (LOWORD(wp) == IDC_CANCEL) { DestroyWindow(h); return 0; }
            break;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_NCDESTROY:
            gSettingsWnd = nullptr;
            break;
        default:
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void OpenSettings() {
    if (gSettingsWnd) {                   // окно уже открыто — просто показать
        ShowWindow(gSettingsWnd, SW_SHOWNORMAL);
        SetForegroundWindow(gSettingsWnd);
        return;
    }
    static bool clsDone = false;
    if (!clsDone) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = SettingsProc;
        wc.hInstance = gHInst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"FwBackupSet";
        RegisterClassW(&wc);
        clsDone = true;
    }
    if (!gUiFont)
        gUiFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPSIBLINGS;
    DWORD exs   = WS_EX_DLGMODALFRAME;
    RECT rc = { 0, 0, 540, 500 };
    AdjustWindowRectEx(&rc, style, FALSE, exs);
    int w = rc.right - rc.left, hh = rc.bottom - rc.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - hh) / 3;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    HWND h = CreateWindowExW(exs, L"FwBackupSet", L"Параметры fw-backup", style,
                             x, y, w, hh, NULL, NULL, gHInst, NULL);
    gSettingsWnd = h;

    MkCtl(h, L"BUTTON", L"Папки для наблюдения — одна строка вида «источник|приёмник»",
          BS_GROUPBOX, 0, 12, 8, 516, 152, 0);
    MkCtl(h, L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP,
          WS_EX_CLIENTEDGE, 24, 26, 492, 120, IDC_PAIRS);
    MkCtl(h, L"BUTTON", L"Исключения — один шаблон в строке («*.tmp», «~$*», «архив\\*»)",
          BS_GROUPBOX, 0, 12, 168, 516, 108, 0);
    MkCtl(h, L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP,
          WS_EX_CLIENTEDGE, 24, 186, 492, 76, IDC_EXCL);
    MkCtl(h, L"STATIC", L"Задержка тишины, мс (100–600000):", 0, 0, 24, 292, 240, 16, 0);
    MkCtl(h, L"EDIT", L"", ES_NUMBER, WS_EX_CLIENTEDGE, 268, 289, 100, 20, IDC_DEB);
    MkCtl(h, L"STATIC", L"Журнал (пусто = не вести):", 0, 0, 24, 322, 180, 16, 0);
    MkCtl(h, L"EDIT", L"", 0, WS_EX_CLIENTEDGE, 208, 319, 308, 20, IDC_LOG);
    MkCtl(h, L"BUTTON", L"Автозагрузка при входе в Windows", BS_AUTOCHECKBOX | WS_TABSTOP,
          0, 24, 352, 470, 18, IDC_AUTOSTART);
    MkCtl(h, L"BUTTON", L"Переносить удалённое в папку !src_deleted", BS_AUTOCHECKBOX | WS_TABSTOP,
          0, 24, 376, 470, 18, IDC_ARCHIVE);
    MkCtl(h, L"BUTTON", L"Не удалять на приёмнике то, чего нет в источнике", BS_AUTOCHECKBOX | WS_TABSTOP,
          0, 24, 400, 470, 18, IDC_KEEP);
    MkCtl(h, L"BUTTON", L"Не делать полную сверку при старте", BS_AUTOCHECKBOX | WS_TABSTOP,
          0, 24, 424, 470, 18, IDC_SKIPFULL);
    MkCtl(h, L"BUTTON", L"Сохранить", BS_DEFPUSHBUTTON | WS_TABSTOP,
          0, 350, 456, 80, 26, IDC_SAVE);
    MkCtl(h, L"BUTTON", L"Отмена", BS_PUSHBUTTON | WS_TABSTOP,
          0, 440, 456, 80, 26, IDC_CANCEL);

    SettingsFill(h);
    ShowWindow(h, SW_SHOWNORMAL);
    SetForegroundWindow(h);
}

// ============================ иконка в трее ============================

static void AddTrayIcon();   // нужно для самовосстановления в UpdateTip

// РАСКАДРОВКА значка (MakeTrayIcon(phase)):
//   phase 0    — покой: оранжевый фон, белая стрелка вниз + «полка»
//   phase 1..3 — занят: КРАСНЫЙ фон, стрелка, под ней три бегущие точки,
//                сдвигаются от левого края (x=3) до правого (полка-тона)
// Отрисовку значка НЕ делает NIM_MODIFY со старым (кэшированным) HICON:
// на этой сборке Windows кадр переносится только через свежий HICON
// (SetGlyph); NIM_MODIFY с прежним (кэшированным) HICON не красит значок.
static HICON gAnimIdle = nullptr;  // кэш кадра покоя для AddTrayIcon
static void  SetGlyph(int phase);  // отрисовка кадра значка: MOD со свежим HICON
static bool  gAnimOn   = false;    // крутится ли таймер анимации
static int   gAnimFrame = 0;       // текущая фаза 0..3 (0 = покой)
static int   gAnimGrace  = 0;      // сколько тиков догорать после конца busy

static void DelTrayIcon() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = gWnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void UpdateTip() {
    if (!gWnd) return;
    DWORD pairsCount;
    { std::lock_guard<std::mutex> g(gCfgMx); pairsCount = (DWORD)gSettings.pairs.size(); }
    bool busy = (gBusySync != 0);
    // старт анимации занятости: мгновенно показать фазу 1 (красный) и завести
    // таймер №2 (400 мс) — сама смена фаз идёт в WM_TIMER, отсюда только старт
    if (busy && !gAnimOn)  { gAnimOn = true;  gAnimFrame = 1; SetTimer(gWnd, 2, 400, NULL); SetGlyph(1); return; }
    if (busy) gAnimGrace = 2;
    std::wstring t = L"fw-backup: пар " + std::to_wstring(pairsCount);
    if (gBusySync)  t += L" — синхронизация";
    else if (gPaused) t += L" — пауза";
    else t += L" — активен";
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = gWnd;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;                    // только подсказка; значком правит SetGlyph
    lstrcpynW(nid.szTip, t.c_str(), 128);
    if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) {
        // иконки нет (перезапуск проводника/трея) — зарегистрировать заново
        LogWrite(L"иконка отсутствует — переустанавливаю");
        AddTrayIcon();
        Shell_NotifyIconW(NIM_MODIFY, &nid);   // вернуть актуальную подсказку
    }
}

// собственная иконка: оранжевый квадрат, белая стрелка вниз и «полка» —
// символ зеркального копирования; легко отличима от системных иконок.
// phase 0 — покой; phase 1..3 — «занят»: фон краснеет, вместо полки под
// стрелкой бегут три точки (полный пробег от левого края до правого)
static HICON MakeTrayIcon(int phase) {
    const int SZ = 32;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = SZ;
    bi.bmiHeader.biHeight      = -SZ;              // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC scr = GetDC(NULL);
    HDC mem = CreateCompatibleDC(scr);
    BYTE* bits = nullptr;
    HBITMAP cbm = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
    HGDIOBJ oldBm = SelectObject(mem, cbm);

    RECT rc = {0, 0, SZ, SZ};
    const bool busy = (phase != 0);
    HBRUSH bg = CreateSolidBrush(busy ? RGB(200, 35, 35) : RGB(235, 140, 0));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    HPEN wp = CreatePen(PS_SOLID, 4, RGB(255, 255, 255));
    HGDIOBJ oldPen = SelectObject(mem, wp);
    MoveToEx(mem, 15, 5, NULL);  LineTo(mem, 15, 17);   // ствол стрелки
    MoveToEx(mem, 8, 13, NULL);  LineTo(mem, 15, 20);   // левое крыло
    MoveToEx(mem, 15, 20, NULL); LineTo(mem, 22, 13);   // правое крыло
    SelectObject(mem, oldPen);
    DeleteObject(wp);

    HBRUSH bw = CreateSolidBrush(RGB(255, 255, 255));
    if (!busy) {
        RECT shelf = {5, 23, 27, 27};                       // «полка» приёмника
        FillRect(mem, &shelf, bw);
    } else {
        // три бегущие точки: полный пробег от левого края (x=3 — на пару
        // пикселей левее «полки») до правого x=27 — фаза 3 встаёт вровень
        const int offU = (phase - 1) * 4;                   // сдвиг 0/4/8 px
        RECT dot0 = {3 + offU,      23, 3 + offU + 4, 27};
        RECT dot1 = {3 + offU + 6,  23, 3 + offU + 10, 27};
        RECT dot2 = {3 + offU + 12, 23, 3 + offU + 16, 27};
        FillRect(mem, &dot0, bw);
        FillRect(mem, &dot1, bw);
        FillRect(mem, &dot2, bw);
    }
    DeleteObject(bw);

    if (bits) for (int i = 3; i < SZ * SZ * 4; i += 4) bits[i] = 0xFF; // непрозрачность

    SelectObject(mem, oldBm);
    // маска обязана быть КОРРЕКТНОЙ (0 = непрозрачно): CreateBitmap с pvBits=NULL
    // даёт НЕОПРЕДЕЛЁННОЕ содержимое, и мусорная маска ломает отрисовку иконки
    // (проявляется как исчезновение при перерисовке — наведении мыши)
    static const BYTE opaqueMask[32 * 32 / 8] = {0};   // все биты 0
    HBITMAP mask = CreateBitmap(SZ, SZ, 1, 1, opaqueMask);
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = cbm;
    ii.hbmMask = mask;
    HICON ic = CreateIconIndirect(&ii);

    DeleteObject(cbm);
    DeleteObject(mask);
    DeleteDC(mem);
    ReleaseDC(NULL, scr);
    return ic;
}

// кадр покоя ставится при добавлении значка (кэш)
static HICON MakeIdleIcon() {
    if (!gAnimIdle) gAnimIdle = MakeTrayIcon(0);
    return gAnimIdle;
}

static HICON gLiveIcon  = nullptr;   // значок, поставленный последним SetGlyph/AddTrayIcon
static bool  gLiveOwned = false;     // создан под нас (кадр анимации) — погасим сами

// подсказка (текст один на все механизмы обновления)
static std::wstring TipText() {
    DWORD pairsCount;
    { std::lock_guard<std::mutex> g(gCfgMx); pairsCount = (DWORD)gSettings.pairs.size(); }
    std::wstring t = L"fw-backup: пар " + std::to_wstring(pairsCount);
    if (gBusySync)  t += L" — синхронизация";
    else if (gPaused) t += L" — пауза";
    else t += L" — активен";
    return t;
}

// Отрисовка кадра значка каналом «NIM_MODIFY + свежий HICON»: на этой сборке
// Windows Explorer перекрашивает значок при смене handle (и это работает без
// «прыжков» и без мигания, в отличие от NIM_DELETE+NIM_ADD). Неудача = значка
// нет — ставим заново через AddTrayIcon.
static void SetGlyph(int phase) {
    if (!gWnd) return;
    HICON h = MakeTrayIcon(phase);
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = gWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon = h;
    lstrcpynW(nid.szTip, TipText().c_str(), 128);
    if (Shell_NotifyIconW(NIM_MODIFY, &nid)) {
        if (gLiveOwned && gLiveIcon) DestroyIcon(gLiveIcon);  // старый уже не показывают
        gLiveIcon = h;
        gLiveOwned = true;
    } else {
        DestroyIcon(h);
        AddTrayIcon();              // значок пропал — ставим кадр покоя заново
    }
}

static void AddTrayIcon() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = gWnd;
    nid.uID = 1;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = MakeIdleIcon();     // значок ставим в кадре покоя
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    lstrcpynW(nid.szTip, TipText().c_str(), 128);
    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        gLiveIcon = nid.hIcon;
        gLiveOwned = false;         // кадр покоя общий — уничтожается на выходе
        LogWrite(L"иконка добавлена (легаси-протокол, оранжевая иконка)");
    } else {
        LogWrite(L"иконка в трее недоступна (продолжаю без иконки, err " +
                 std::to_wstring(GetLastError()) + L")");
    }
}

static void ShowTrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, MENU_SETTINGS, L"Настройки…");
    AppendMenuW(m, (gBusySync ? MF_STRING | MF_GRAYED : MF_STRING), MENU_SYNC, L"Синхронизировать сейчас");
    AppendMenuW(m, MF_STRING, MENU_PAUSE, gPaused ? L"Продолжить" : L"Пауза");
    AppendMenuW(m, (gBusySync ? MF_STRING | MF_GRAYED : MF_STRING), MENU_RELOAD, L"Перечитать конфиг");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, MENU_EXIT, L"Выход");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(gWnd);
    int cmd = (int)TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                  pt.x, pt.y, 0, gWnd, NULL);
    PostMessageW(gWnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (cmd) {
        case MENU_SETTINGS: OpenSettings(); break;
        case MENU_SYNC:     CloseHandle(CreateThread(NULL, 0, SyncNowThread, NULL, 0, NULL)); break;
        case MENU_PAUSE:    gPaused = !gPaused; WakeAllCopiers(); UpdateTip(); break;
        case MENU_RELOAD:  CloseHandle(CreateThread(NULL, 0, ReloadThread, NULL, 0, NULL)); break;
        case MENU_EXIT:    PostThreadMessageW(gMainThreadId, WM_QUIT, 0, 0); break;
    }
}

#ifndef NOTIFYICON_VERSION_4
#define NOTIFYICON_VERSION_4 4
#endif
#ifndef NIN_SELECT
#define NIN_SELECT 0x0400
#endif
#ifndef NIN_KEYSELECT
#define NIN_KEYSELECT 0x0401
#endif

// события WM_TRAY: формат совместим и с легаси (lParam = сообщение), и с версией 4
// (LOWORD(lParam) = событие)
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == gTaskbarCreatedMsg && gTaskbarCreatedMsg) {
        AddTrayIcon();
        UpdateTip();
        return 0;
    }
    switch (msg) {
        case WM_TRAY: {
            WORD evt = LOWORD(lp);
            if (evt == WM_CONTEXTMENU || evt == WM_RBUTTONUP || evt == WM_LBUTTONUP ||
                evt == NIN_SELECT || evt == NIN_KEYSELECT)
                ShowTrayMenu();
            return 0;
        }
        case WM_UPDTIP: UpdateTip(); return 0;
        case WM_TIMER: {
            if (wp == 2) {                          // анимация «идёт синхронизация»
                if (gBusySync) {
                    gAnimGrace = 2;                 // сколько тиков догорит после конца busy
                    gAnimFrame = (gAnimFrame % 3) + 1;      // фазы 1→2→3→1
                    SetGlyph(gAnimFrame);
                    return 0;
                }
                if (gAnimGrace-- <= 0) {            // busy кончился — дотикали и гасим
                    KillTimer(h, 2);
                    gAnimOn = false; gAnimFrame = 0;
                    SetGlyph(0);                     // кадр покоя, свежим HICON'ом (без мигания)
                    UpdateTip();
                    return 0;
                }
                return 0;                           // grace-тик: фаза не меняется
            }
            // wp == 1 — страховка раз в 30 с: лёгкий NIM_MODIFY;
            // полный NIM_DELETE+NIM_ADD (с «прыжком» на первое место полосы) —
            // только если иконка действительно отсутствует (см. UpdateTip)
            UpdateTip();
            return 0;
        }
        default: return DefWindowProcW(h, msg, wp, lp);
    }
}

// ============================ WinMain ============================

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR lpCmdLine, int nShow) {
    (void)nShow;
    gHInst = hInst;

    // только один экземпляр
    HANDLE mutex = CreateMutexW(NULL, TRUE, kMutexName);
    if (mutex == NULL || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        MessageBoxW(NULL, L"fw-backup уже запущен.", L"fw-backup", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    wchar_t exe[MAX_PATH + 2] = L"";
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    gExePath = exe;

    gMainThreadId = GetCurrentThreadId();
    gTaskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    // обычное НЕВИДИМОЕ окно верхнего уровня (не message-only!): только такое окно
    // получает широковещательные сообщения оболочки (TaskbarCreated) и надёжно
    // держит иконку в трее при всех перерисовках/наведениях мыши
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"FwBackupWnd";
    RegisterClassW(&wc);
    gWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           L"FwBackupWnd", L"fw-backup", WS_POPUP,
                           0, 0, 0, 0, NULL, NULL, hInst, NULL);

    {
        std::lock_guard<std::mutex> g(gCfgMx);
        LoadSettingsInto(gSettings);
        gLogFile = gSettings.logFile;
    }
    LogWrite(L"=== запуск, пар: " + std::to_wstring(gSettings.pairs.size()));

    std::vector<PairCfg> snapshot;
    { std::lock_guard<std::mutex> g(gCfgMx); snapshot = gSettings.pairs; }
    for (auto& pc : snapshot) StartWatcher(pc);

    AddTrayIcon();
    UpdateTip();
    SetTimer(gWnd, 1, 30000, NULL);   // напоминать оболочке о себе каждые 30 с

    // fw-backup.exe /settings — открыть настройки сразу после запуска
    {
        char low[256] = {};
        if (lpCmdLine) {
            size_t n = strlen(lpCmdLine);
            if (n > 255) n = 255;
            for (size_t i = 0; i < n; ++i) low[i] = (char)tolower((unsigned char)lpCmdLine[i]);
        }
        if (strstr(low, "/settings") || strstr(low, "-settings"))
            OpenSettings();
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    StopAllWatchers();
    DelTrayIcon();
    if (gLiveOwned && gLiveIcon) DestroyIcon(gLiveIcon);
    if (gAnimIdle) { DestroyIcon(gAnimIdle); gAnimIdle = nullptr; }
    CloseHandle(mutex);
    LogWrite(L"=== выход");
    return 0;
}
