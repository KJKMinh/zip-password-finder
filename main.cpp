#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <bit7z/bit7z.hpp>

namespace {
    constexpr UINT WM_APP_SEARCH_DONE = WM_APP + 1;
    constexpr UINT WM_APP_STATUS_UPDATE = WM_APP + 2;
    constexpr int DEFAULT_WORKER_THREADS = 4;

    struct SearchRequest {
        HWND hwnd = nullptr;
        std::wstring archivePath;
        std::vector<std::wstring> wordlist;
        std::wstring dllPath;
    };

    struct SearchResult {
        bool found = false;
        std::wstring password;
        std::wstring message;
        uint64_t testedCount = 0;
        uint64_t elapsedMs = 0;
    };

    struct AppState {
        HWND hwnd = nullptr;
        std::wstring archivePath;
        std::wstring statusMessage = L"Drop a ZIP/RAR file here";
        std::wstring foundPassword;
        std::wstring currentCandidate;
        uint64_t testedCount = 0;
        uint64_t elapsedMs = 0;
        bool searching = false;
    };

    struct StatusUpdate {
        std::wstring candidate;
        uint64_t testedCount = 0;
        uint64_t elapsedMs = 0;
        bool found = false;
        std::wstring password;
    };

    struct SharedSearchState {
        HWND hwnd = nullptr;
        std::wstring archivePath;
        std::wstring dllPath;
        std::vector<std::wstring> wordlist;
        std::atomic<bool> stop{false};
        std::atomic<bool> found{false};
        std::atomic<uint64_t> testedCount{0};
        std::atomic<uint64_t> startMs{0};
        std::wstring foundPassword;
    };

    struct WorkerArgs {
        SharedSearchState* shared = nullptr;
        int mode = 0;
    };

    std::wstring ToWide(const std::string& narrow) {
        if (narrow.empty()) return {};
        int needed = MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), -1, nullptr, 0);
        std::vector<wchar_t> buffer(needed);
        MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), -1, buffer.data(), needed);
        return std::wstring(buffer.data());
    }

    // Hàm chuyển đổi wstring sang string (UTF-8)
    std::string ToUtf8(const std::wstring& wide) {
        if (wide.empty()) return {};
        int needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::vector<char> buffer(needed);
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, buffer.data(), needed, nullptr, nullptr);
        return std::string(buffer.data());
    }

    std::wstring Trim(const std::wstring& value) {
        const wchar_t* whitespace = L" \t\r\n";
        size_t start = value.find_first_not_of(whitespace);
        if (start == std::wstring::npos) return L"";
        size_t end = value.find_last_not_of(whitespace);
        return value.substr(start, end - start + 1);
    }

    std::wstring ToLower(const std::wstring& value) {
        std::wstring out = value;
        std::transform(out.begin(), out.end(), out.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        return out;
    }

    std::wstring Capitalize(const std::wstring& value) {
        if (value.empty()) return {};
        std::wstring out = ToLower(value);
        out[0] = static_cast<wchar_t>(std::towupper(out[0]));
        return out;
    }

    std::wstring FormatNumber(uint64_t value) {
        wchar_t buffer[64] = {};
        swprintf(buffer, L"%llu", static_cast<unsigned long long>(value));
        return buffer;
    }

    std::wstring GetExeDirectory() {
        wchar_t buffer[MAX_PATH];
        GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        std::wstring path(buffer);
        size_t pos = path.find_last_of(L"\\/");
        return path.substr(0, pos + 1);
    }

    int GetWorkerThreadCount() {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        int count = static_cast<int>(info.dwNumberOfProcessors);
        return (count > 0) ? count : DEFAULT_WORKER_THREADS;
    }

    std::vector<std::wstring> LoadWordlist(const std::wstring& archivePath) {
        std::vector<std::wstring> words;
        std::wstring folder = archivePath;
        size_t pos = folder.find_last_of(L"\\/");
        if (pos != std::wstring::npos) folder = folder.substr(0, pos);

        std::wstring wordlistPath = folder + L"\\passwords.txt";
        std::ifstream input(ToUtf8(wordlistPath));
        if (input.is_open()) {
            std::string line;
            while (std::getline(input, line)) {
                std::wstring word = ToWide(line);
                word = Trim(word);
                if (!word.empty()) words.push_back(word);
            }
        }
        return words;
    }

    std::vector<std::wstring> BuildWordVariants(const std::wstring& word) {
        std::vector<std::wstring> variants;
        auto add = [&](const std::wstring& value) { if (!value.empty()) variants.push_back(value); };

        add(word);
        add(Capitalize(word));
        add(ToLower(word));
        add(word + L"123"); add(L"123" + word);
        add(word + L"2024"); add(word + L"2025"); add(word + L"2026");
        add(word + L"!"); add(word + L"1"); add(L"1" + word);
        add(word + L"01"); add(word + L"@2024"); add(word + L"2024!"); add(word + L"123!");

        for (int year = 1900; year <= 2030; ++year) {
            wchar_t buffer[16] = {};
            swprintf(buffer, L"%d", year);
            add(word + buffer);
            add(buffer + word);
        }
        return variants;
    }

    void RefreshWindow(HWND hwnd) {
        if (hwnd) {
            InvalidateRect(hwnd, nullptr, TRUE);
            UpdateWindow(hwnd);
        }
    }

    void PostStatus(HWND hwnd, const std::wstring& candidate, uint64_t testedCount, uint64_t elapsedMs, bool found, const std::wstring& password) {
        auto* update = new StatusUpdate();
        update->candidate = candidate;
        update->testedCount = testedCount;
        update->elapsedMs = elapsedMs;
        update->found = found;
        update->password = password;
        PostMessageW(hwnd, WM_APP_STATUS_UPDATE, 0, reinterpret_cast<LPARAM>(update));
    }

 bool TryCandidateInMemory(SharedSearchState* state, const bit7z::Bit7zLibrary& lib, const std::wstring& candidate) {
        if (state == nullptr || state->stop.load(std::memory_order_relaxed)) {
            return false;
        }

        uint64_t tested = state->testedCount.fetch_add(1, std::memory_order_relaxed) + 1;

        try {
            // Nhận diện định dạng file dựa vào đuôi mở rộng (tương thích bit7z v3.x)
            std::wstring pathLower = ToLower(state->archivePath);
            const bit7z::BitInFormat* format = &bit7z::BitFormat::Zip; // Mặc định là ZIP
            
            if (pathLower.find(L".rar") != std::wstring::npos) {
                // File RAR của bạn dùng chuẩn RAR5. Nếu test file RAR quá cũ bị lỗi, bạn có thể đổi thành bit7z::BitFormat::Rar
                format = &bit7z::BitFormat::Rar5; 
            } else if (pathLower.find(L".7z") != std::wstring::npos) {
                format = &bit7z::BitFormat::SevenZip;
            }

            // Gọi thư viện với định dạng đã được nhận diện
            bit7z::BitArchiveReader arc{ lib, ToUtf8(state->archivePath), *format, ToUtf8(candidate) };
            
            // Kiểm tra mật khẩu
            arc.test();
            
            // Nếu không bị văng Exception -> Mật khẩu đúng
            bool expected = false;
            if (state->found.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
                state->stop.store(true, std::memory_order_relaxed);
                state->foundPassword = candidate;
                PostStatus(state->hwnd, candidate, tested, GetTickCount() - state->startMs.load(std::memory_order_relaxed), true, candidate);
            }
            return true;
            
        } catch (const bit7z::BitException&) {
            // Sai mật khẩu, bỏ qua
        }

        if (tested % 5000 == 0) {
            PostStatus(state->hwnd, candidate, tested, GetTickCount() - state->startMs.load(std::memory_order_relaxed), false, L"");
        }
        return false;
    }

    void RunDictionarySearch(SharedSearchState* state, const bit7z::Bit7zLibrary& lib) {
        const std::vector<std::wstring> base = {
            L"", L"password", L"123456", L"12345678", L"qwerty", L"welcome", L"admin", L"letmein"
        };
        for (const auto& candidate : base) {
            if (state->stop.load(std::memory_order_relaxed)) break;
            if (TryCandidateInMemory(state, lib, candidate)) return;
        }
        for (const auto& word : state->wordlist) {
            if (state->stop.load(std::memory_order_relaxed)) break;
            std::vector<std::wstring> variants = BuildWordVariants(word);
            for (const auto& candidate : variants) {
                if (state->stop.load(std::memory_order_relaxed)) break;
                if (TryCandidateInMemory(state, lib, candidate)) return;
            }
        }
    }

    void RunNumericSearch(SharedSearchState* state, const bit7z::Bit7zLibrary& lib) {
        for (int len = 1; len <= 6; ++len) {
            std::wstring current(len, L'0');
            for (uint64_t i = 0; i < 1000000ULL; ++i) {
                if (state->stop.load(std::memory_order_relaxed)) break;
                if (TryCandidateInMemory(state, lib, current)) return;

                bool carry = true;
                for (int d = len - 1; d >= 0 && carry; --d) {
                    if (current[d] < L'9') { ++current[d]; carry = false; }
                    else { current[d] = L'0'; }
                }
                if (carry) break;
            }
        }
    }

    void RunLowercaseSearch(SharedSearchState* state, const bit7z::Bit7zLibrary& lib) {
        const std::wstring alphabet = L"abcdefghijklmnopqrstuvwxyz";
        for (int len = 1; len <= 4; ++len) {
            std::wstring current(len, L'a');
            for (uint64_t i = 0; i < 500000ULL; ++i) {
                if (state->stop.load(std::memory_order_relaxed)) break;
                if (TryCandidateInMemory(state, lib, current)) return;

                bool carry = true;
                for (int d = len - 1; d >= 0 && carry; --d) {
                    size_t pos = alphabet.find(current[d]);
                    if (pos + 1 < alphabet.size()) { current[d] = alphabet[pos + 1]; carry = false; }
                    else { current[d] = alphabet[0]; }
                }
                if (carry) break;
            }
        }
    }

    DWORD WINAPI WorkerThread(LPVOID lpParam) {
        auto* args = static_cast<WorkerArgs*>(lpParam);
        if (!args || !args->shared) return 0;

        try {
            // SỬ DỤNG ToUtf8 ĐỂ CHUYỂN ĐỔI dllPath SANG std::string
            bit7z::Bit7zLibrary lib{ ToUtf8(args->shared->dllPath) };

            switch (args->mode) {
                case 0: RunDictionarySearch(args->shared, lib); break;
                case 1: RunNumericSearch(args->shared, lib); break;
                case 2: RunLowercaseSearch(args->shared, lib); break;
                case 3: RunDictionarySearch(args->shared, lib); break;
            }
        } catch (const std::exception&) {
        }

        delete args;
        return 0;
    }

    DWORD WINAPI SearchThread(LPVOID lpParam) {
        auto* request = static_cast<SearchRequest*>(lpParam);
        auto* result = new SearchResult();

        SharedSearchState shared;
        shared.hwnd = request->hwnd;
        shared.archivePath = request->archivePath;
        shared.dllPath = request->dllPath;
        shared.wordlist = request->wordlist;
        shared.startMs.store(GetTickCount(), std::memory_order_relaxed);

        int workerThreads = GetWorkerThreadCount();
        std::vector<HANDLE> handles;
        
        for (int i = 0; i < workerThreads; ++i) {
            auto* args = new WorkerArgs();
            args->shared = &shared;
            args->mode = i % 3;
            HANDLE handle = CreateThread(nullptr, 0, WorkerThread, args, 0, nullptr);
            if (handle) handles.push_back(handle);
            else delete args;
        }

        if (!handles.empty()) {
            WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), TRUE, INFINITE);
        }
        for (HANDLE handle : handles) CloseHandle(handle);

        result->testedCount = shared.testedCount.load(std::memory_order_relaxed);
        result->elapsedMs = GetTickCount() - shared.startMs.load(std::memory_order_relaxed);
        
        if (shared.found.load(std::memory_order_relaxed)) {
            result->found = true;
            result->password = shared.foundPassword;
            result->message = L"Password found!";
        } else {
            result->message = L"No password found in the given space.";
        }

        PostMessageW(request->hwnd, WM_APP_SEARCH_DONE, 0, reinterpret_cast<LPARAM>(result));
        delete request;
        return 0;
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        static AppState* state = nullptr;

        switch (msg) {
            case WM_CREATE: {
                state = new AppState();
                state->hwnd = hwnd;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
                DragAcceptFiles(hwnd, TRUE);
                return 0;
            }
            case WM_CLOSE: PostQuitMessage(0); return 0;
            case WM_DESTROY: {
                if (state) { delete state; state = nullptr; }
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                return 0;
            }
            case WM_DROPFILES: {
                if (!state || state->searching) { DragFinish(reinterpret_cast<HDROP>(wParam)); return 0; }
                
                HDROP hDrop = reinterpret_cast<HDROP>(wParam);
                wchar_t path[MAX_PATH] = {};
                if (DragQueryFileW(hDrop, 0, path, MAX_PATH) > 0) {
                    state->archivePath = path;
                    state->foundPassword.clear();
                    state->currentCandidate.clear();
                    state->testedCount = 0;
                    state->elapsedMs = 0;
                    
                    std::wstring dllPath = GetExeDirectory() + L"7z.dll";
                    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                        state->statusMessage = L"Error: 7z.dll not found in the executable folder!";
                        RefreshWindow(hwnd);
                    } else {
                        state->searching = true;
                        state->statusMessage = L"Searching for password...";
                        
                        auto* request = new SearchRequest();
                        request->hwnd = hwnd;
                        request->archivePath = state->archivePath;
                        request->dllPath = dllPath;
                        request->wordlist = LoadWordlist(state->archivePath);

                        CloseHandle(CreateThread(nullptr, 0, SearchThread, request, 0, nullptr));
                    }
                    RefreshWindow(hwnd);
                }
                DragFinish(hDrop);
                return 0;
            }
            case WM_APP_STATUS_UPDATE: {
                if (!state) return 0;
                auto* update = reinterpret_cast<StatusUpdate*>(lParam);
                if (update) {
                    state->currentCandidate = update->candidate;
                    state->testedCount = update->testedCount;
                    state->elapsedMs = update->elapsedMs;
                    if (update->found) {
                        state->foundPassword = update->password;
                        state->statusMessage = L"Password found: " + update->password;
                        state->searching = false;
                    }
                    delete update;
                }
                RefreshWindow(hwnd);
                return 0;
            }
            case WM_APP_SEARCH_DONE: {
                if (!state) return 0;
                auto* result = reinterpret_cast<SearchResult*>(lParam);
                state->searching = false;
                if (result) {
                    state->testedCount = result->testedCount;
                    state->elapsedMs = result->elapsedMs;
                    if (result->found) {
                        state->foundPassword = result->password;
                        state->statusMessage = L"Password found: " + result->password;
                    } else {
                        state->statusMessage = result->message;
                    }
                    delete result;
                }
                RefreshWindow(hwnd);
                return 0;
            }
            case WM_PAINT: {
                if (!state) return DefWindowProcW(hwnd, msg, wParam, lParam);
                PAINTSTRUCT ps{};
                HDC hdc = BeginPaint(hwnd, &ps);
                RECT client{}; GetClientRect(hwnd, &client);

                HBRUSH bgBrush = CreateSolidBrush(RGB(244, 247, 252));
                FillRect(hdc, &client, bgBrush); DeleteObject(bgBrush);

                RECT panel = client; panel.left = 30; panel.top = 30; panel.right -= 30; panel.bottom -= 30;
                HBRUSH borderBrush = CreateSolidBrush(RGB(43, 108, 196));
                FrameRect(hdc, &panel, borderBrush); DeleteObject(borderBrush);

                SetBkMode(hdc, TRANSPARENT);
                
                HFONT titleFont = CreateFontW(24, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Segoe UI");
                HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, titleFont));
                SetTextColor(hdc, RGB(27, 39, 56));
                TextOutW(hdc, 55, 55, L"ZIP/RAR Password Finder (In-Memory)", 35);

                HFONT bodyFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Segoe UI");
                SelectObject(hdc, bodyFont);

                std::wstring archiveText = state->archivePath.empty() ? L"Drop a ZIP/RAR file onto this window" : L"Archive: " + state->archivePath;
                TextOutW(hdc, 55, 95, archiveText.c_str(), static_cast<int>(archiveText.size()));

                std::wstring statusText = state->searching ? L"Status: searching..." : L"Status: " + state->statusMessage;
                if (!state->foundPassword.empty()) SetTextColor(hdc, RGB(0, 128, 0));
                TextOutW(hdc, 55, 130, statusText.c_str(), static_cast<int>(statusText.size()));
                SetTextColor(hdc, RGB(27, 39, 56));

                std::wstring statsText = L"Tried: " + FormatNumber(state->testedCount) + L" | Elapsed: " + FormatNumber(state->elapsedMs) + L" ms";
                TextOutW(hdc, 55, 165, statsText.c_str(), static_cast<int>(statsText.size()));

                if (!state->currentCandidate.empty()) {
                    std::wstring currentText = state->foundPassword.empty() ? L"Current: " + state->currentCandidate : L"Found: " + state->currentCandidate;
                    TextOutW(hdc, 55, 200, currentText.c_str(), static_cast<int>(currentText.size()));
                }

                SelectObject(hdc, oldFont);
                DeleteObject(titleFont); DeleteObject(bodyFont);
                EndPaint(hwnd, &ps);
                return 0;
            }
            default: return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    const wchar_t* className = L"ZipPasswordFinderWindow";
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = className;
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, className, L"ZIP/RAR Password Finder", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 760, 460, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}