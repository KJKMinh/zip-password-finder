#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <cwctype>
#include <bit7z/bit7z.hpp>

// --- ID Controls cho Giao diện ---
#define IDC_CHK_NUM     101
#define IDC_CHK_LOWER   102
#define IDC_CHK_UPPER   103
#define IDC_CHK_SYM     104
#define IDC_EDT_MAXLEN  105

namespace {
    constexpr UINT WM_APP_SEARCH_DONE = WM_APP + 1;
    constexpr UINT WM_APP_STATUS_UPDATE = WM_APP + 2;

    struct Task {
        int length;
        char prefix;
    };

    struct SearchRequest {
        HWND hwnd = nullptr;
        std::wstring archivePath;
        std::string charset;
        int maxLength = 6;
    };

    struct SearchResult {
        bool found = false;
        std::string password;
        std::wstring message;
        uint64_t testedCount = 0;
        uint64_t elapsedMs = 0;
    };

    struct AppState {
        HWND hwnd = nullptr;
        std::wstring archivePath;
        std::wstring statusMessage = L"Kéo thả file ZIP/RAR vào đây";
        std::wstring foundPassword;
        std::wstring currentCandidate;
        uint64_t testedCount = 0;
        uint64_t elapsedMs = 0;
        bool searching = false;

        // UI Controls
        HWND hChkNum, hChkLower, hChkUpper, hChkSym, hEdtMaxLen;
    };

    struct StatusUpdate {
        std::string candidate;
        uint64_t testedCount = 0;
        uint64_t elapsedMs = 0;
        bool found = false;
        std::string password;
    };

    struct SharedSearchState {
        HWND hwnd = nullptr;
        std::string archivePathUtf8;
        std::string charset;
        std::wstring dllPath;
        
        std::vector<Task> taskQueue;
        size_t taskIndex = 0;
        std::mutex queueMutex;

        std::atomic<bool> stop{false};
        std::atomic<bool> found{false};
        std::atomic<uint64_t> testedCount{0};
        std::atomic<uint64_t> startMs{0};
        std::string foundPassword;
    };

    struct WorkerArgs {
        SharedSearchState* shared = nullptr;
    };

    // --- Các hàm tiện ích ---
    std::wstring ToWide(const std::string& narrow) {
        if (narrow.empty()) return {};
        int needed = MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), -1, nullptr, 0);
        std::vector<wchar_t> buffer(needed);
        MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), -1, buffer.data(), needed);
        return std::wstring(buffer.data());
    }

    std::string ToUtf8(const std::wstring& wide) {
        if (wide.empty()) return {};
        int needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::vector<char> buffer(needed);
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, buffer.data(), needed, nullptr, nullptr);
        return std::string(buffer.data());
    }

    std::wstring ToLowerW(const std::wstring& value) {
        std::wstring out = value;
        for (auto& c : out) c = static_cast<wchar_t>(std::towlower(c));
        return out;
    }

    std::wstring FormatNumber(uint64_t value) {
        wchar_t buffer[64] = {};
        swprintf(buffer, L"%llu", static_cast<unsigned long long>(value));
        return buffer;
    }

    void RefreshWindow(HWND hwnd) {
        if (hwnd) {
            InvalidateRect(hwnd, nullptr, TRUE);
            UpdateWindow(hwnd);
        }
    }

    void PostStatus(HWND hwnd, const std::string& candidate, uint64_t testedCount, uint64_t elapsedMs, bool found, const std::string& password) {
        auto* update = new StatusUpdate();
        update->candidate = candidate;
        update->testedCount = testedCount;
        update->elapsedMs = elapsedMs;
        update->found = found;
        update->password = password;
        PostMessageW(hwnd, WM_APP_STATUS_UPDATE, 0, reinterpret_cast<LPARAM>(update));
    }

    // --- Lõi Giải mã Trực tiếp trên RAM ---
    bool TryCandidateInMemory(SharedSearchState* state, const bit7z::Bit7zLibrary& lib, const std::string& candidate) {
        if (state == nullptr || state->stop.load(std::memory_order_relaxed)) return false;

        uint64_t tested = state->testedCount.fetch_add(1, std::memory_order_relaxed) + 1;

        try {
            // Tối ưu: Dùng biến utf8 được sinh thẳng từ thuật toán, không cần convert tốn CPU
            std::string pathLower = ToUtf8(ToLowerW(ToWide(state->archivePathUtf8)));
            const bit7z::BitInFormat* format = &bit7z::BitFormat::Zip;
            
            if (pathLower.find(".rar") != std::string::npos) format = &bit7z::BitFormat::Rar5;
            else if (pathLower.find(".7z") != std::string::npos) format = &bit7z::BitFormat::SevenZip;

            bit7z::BitArchiveReader arc{ lib, state->archivePathUtf8, *format, candidate };
            arc.test();
            
            bool expected = false;
            if (state->found.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
                state->stop.store(true, std::memory_order_relaxed);
                state->foundPassword = candidate;
                PostStatus(state->hwnd, candidate, tested, GetTickCount() - state->startMs.load(std::memory_order_relaxed), true, candidate);
            }
            return true;
        } catch (const bit7z::BitException&) {
            // Sai mật khẩu
        }

        if (tested % 10000 == 0) {
            PostStatus(state->hwnd, candidate, tested, GetTickCount() - state->startMs.load(std::memory_order_relaxed), false, "");
        }
        return false;
    }

    // --- Thuật toán Sinh Tổ hợp (Iterative Combinatorics) ---
    void ProcessTask(SharedSearchState* state, const bit7z::Bit7zLibrary& lib, const Task& task) {
        int len = task.length;
        char prefix = task.prefix;
        const std::string& charset = state->charset;
        int num_chars = charset.length();

        if (len == 1) {
            TryCandidateInMemory(state, lib, std::string(1, prefix));
            return;
        }

        std::vector<int> indices(len - 1, 0);
        std::string candidate(len, '\0');
        candidate[0] = prefix;

        while (!state->stop.load(std::memory_order_relaxed)) {
            // Cập nhật chuỗi mật khẩu
            for (int i = 0; i < len - 1; ++i) {
                candidate[i + 1] = charset[indices[i]];
            }

            if (TryCandidateInMemory(state, lib, candidate)) return;

            // Thuật toán cộng dồn base-N
            int p = len - 2;
            while (p >= 0) {
                indices[p]++;
                if (indices[p] < num_chars) break;
                indices[p] = 0;
                p--;
            }
            if (p < 0) break; // Đã duyệt hết các cấu hình cho prefix này
        }
    }

    // --- Luồng Xử lý (Worker) ---
    DWORD WINAPI WorkerThread(LPVOID lpParam) {
        auto* args = static_cast<WorkerArgs*>(lpParam);
        if (!args || !args->shared) return 0;

        try {
            bit7z::Bit7zLibrary lib{ "7z.dll" };

            while (!args->shared->stop.load(std::memory_order_relaxed)) {
                Task currentTask;
                {
                    std::lock_guard<std::mutex> lock(args->shared->queueMutex);
                    if (args->shared->taskIndex >= args->shared->taskQueue.size()) break;
                    currentTask = args->shared->taskQueue[args->shared->taskIndex++];
                }
                ProcessTask(args->shared, lib, currentTask);
            }
        } catch (...) { }

        delete args;
        return 0;
    }

    // --- Luồng Điều phối (Manager) ---
    DWORD WINAPI SearchThread(LPVOID lpParam) {
        auto* request = static_cast<SearchRequest*>(lpParam);
        auto* result = new SearchResult();

        SharedSearchState shared;
        shared.hwnd = request->hwnd;
        shared.archivePathUtf8 = ToUtf8(request->archivePath);
        shared.charset = request->charset;
        shared.startMs.store(GetTickCount(), std::memory_order_relaxed);

        // Sinh danh sách tác vụ (Chia lô công việc)
        for (int len = 1; len <= request->maxLength; ++len) {
            for (char prefix : shared.charset) {
                shared.taskQueue.push_back({len, prefix});
            }
        }

        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        int workerThreads = static_cast<int>(info.dwNumberOfProcessors);
        if (workerThreads <= 0) workerThreads = 4;

        std::vector<HANDLE> handles;
        for (int i = 0; i < workerThreads; ++i) {
            auto* args = new WorkerArgs();
            args->shared = &shared;
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
            result->message = L"Đã tìm thấy mật khẩu!";
        } else {
            result->message = L"Không tìm thấy mật khẩu trong giới hạn chỉ định.";
        }

        PostMessageW(request->hwnd, WM_APP_SEARCH_DONE, 0, reinterpret_cast<LPARAM>(result));
        delete request;
        return 0;
    }

    // --- Giao diện (Win32 API) ---
    LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        static AppState* state = nullptr;

        switch (msg) {
            case WM_CREATE: {
                state = new AppState();
                state->hwnd = hwnd;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
                DragAcceptFiles(hwnd, TRUE);

                // Tạo các Controls
                HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
                
                state->hChkNum = CreateWindowW(L"BUTTON", L"Số (0-9)", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 55, 270, 100, 20, hwnd, (HMENU)IDC_CHK_NUM, nullptr, nullptr);
                state->hChkLower = CreateWindowW(L"BUTTON", L"a-z", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 160, 270, 70, 20, hwnd, (HMENU)IDC_CHK_LOWER, nullptr, nullptr);
                state->hChkUpper = CreateWindowW(L"BUTTON", L"A-Z", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 240, 270, 70, 20, hwnd, (HMENU)IDC_CHK_UPPER, nullptr, nullptr);
                state->hChkSym = CreateWindowW(L"BUTTON", L"Ký tự (!@...)", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 320, 270, 120, 20, hwnd, (HMENU)IDC_CHK_SYM, nullptr, nullptr);
                
                CreateWindowW(L"STATIC", L"Độ dài tối đa:", WS_VISIBLE | WS_CHILD, 55, 305, 100, 20, hwnd, nullptr, nullptr, nullptr);
                state->hEdtMaxLen = CreateWindowW(L"EDIT", L"6", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER, 160, 302, 50, 22, hwnd, (HMENU)IDC_EDT_MAXLEN, nullptr, nullptr);

                SendMessage(state->hChkNum, WM_SETFONT, (WPARAM)hFont, TRUE);
                SendMessage(state->hChkLower, WM_SETFONT, (WPARAM)hFont, TRUE);
                SendMessage(state->hChkUpper, WM_SETFONT, (WPARAM)hFont, TRUE);
                SendMessage(state->hChkSym, WM_SETFONT, (WPARAM)hFont, TRUE);
                SendMessage(state->hEdtMaxLen, WM_SETFONT, (WPARAM)hFont, TRUE);
                
                // Check mặc định
                SendMessage(state->hChkNum, BM_SETCHECK, BST_CHECKED, 0);
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
                    
                    // Build Charset từ UI
                    std::string charset = "";
                    if (SendMessage(state->hChkNum, BM_GETCHECK, 0, 0) == BST_CHECKED) charset += "0123456789";
                    if (SendMessage(state->hChkLower, BM_GETCHECK, 0, 0) == BST_CHECKED) charset += "abcdefghijklmnopqrstuvwxyz";
                    if (SendMessage(state->hChkUpper, BM_GETCHECK, 0, 0) == BST_CHECKED) charset += "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
                    if (SendMessage(state->hChkSym, BM_GETCHECK, 0, 0) == BST_CHECKED) charset += "!@#$%^&*()-_+=~`[]{}|:;\"'<>,.?/";
                    
                    // Nếu không tick gì, coi như muốn thử TẤT CẢ
                    if (charset.empty()) {
                        charset = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()-_+=~`[]{}|:;\"'<>,.?/";
                    }

                    // Lấy độ dài tối đa
                    wchar_t lenBuf[16] = {};
                    GetWindowTextW(state->hEdtMaxLen, lenBuf, 16);
                    int maxLen = _wtoi(lenBuf);
                    if (maxLen <= 0) maxLen = 6;
                    if (maxLen > 15) maxLen = 15; // Giới hạn chống treo máy

                    state->searching = true;
                    state->statusMessage = L"Đang dò tìm...";
                    
                    auto* request = new SearchRequest();
                    request->hwnd = hwnd;
                    request->archivePath = state->archivePath;
                    request->charset = charset;
                    request->maxLength = maxLen;

                    CloseHandle(CreateThread(nullptr, 0, SearchThread, request, 0, nullptr));
                    RefreshWindow(hwnd);
                }
                DragFinish(hDrop);
                return 0;
            }
            case WM_APP_STATUS_UPDATE: {
                if (!state) return 0;
                auto* update = reinterpret_cast<StatusUpdate*>(lParam);
                if (update) {
                    state->currentCandidate = ToWide(update->candidate);
                    state->testedCount = update->testedCount;
                    state->elapsedMs = update->elapsedMs;
                    if (update->found) {
                        state->foundPassword = ToWide(update->password);
                        state->statusMessage = L"Tìm thấy mật khẩu: " + state->foundPassword;
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
                        state->foundPassword = ToWide(result->password);
                        state->statusMessage = L"Tìm thấy mật khẩu: " + state->foundPassword;
                    } else {
                        state->statusMessage = result->message;
                    }
                    delete result;
                }
                RefreshWindow(hwnd);
                return 0;
            }
            case WM_CTLCOLORSTATIC: {
                HDC hdcStatic = (HDC)wParam;
                SetTextColor(hdcStatic, RGB(27, 39, 56));
                SetBkMode(hdcStatic, TRANSPARENT);
                return (LRESULT)GetStockObject(NULL_BRUSH);
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
                HFONT titleFont = CreateFontW(24, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
                HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, titleFont));
                SetTextColor(hdc, RGB(27, 39, 56));
                TextOutW(hdc, 55, 55, L"ZIP/RAR Password Finder (Max Optimized)", 39);

                HFONT bodyFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
                SelectObject(hdc, bodyFont);

                std::wstring archiveText = state->archivePath.empty() ? L"File:" : L"File: " + state->archivePath;
                TextOutW(hdc, 55, 95, archiveText.c_str(), static_cast<int>(archiveText.size()));

                std::wstring statusText = state->searching ? L"Trạng thái: Đang dò..." : L"Trạng thái: " + state->statusMessage;
                if (!state->foundPassword.empty()) SetTextColor(hdc, RGB(0, 128, 0));
                TextOutW(hdc, 55, 130, statusText.c_str(), static_cast<int>(statusText.size()));
                SetTextColor(hdc, RGB(27, 39, 56));

                std::wstring statsText = L"Đã thử: " + FormatNumber(state->testedCount) + L" | Thời gian: " + FormatNumber(state->elapsedMs) + L" ms";
                TextOutW(hdc, 55, 165, statsText.c_str(), static_cast<int>(statsText.size()));

                if (!state->currentCandidate.empty()) {
                    std::wstring currentText = state->foundPassword.empty() ? L"Đang thử: " + state->currentCandidate : L"Đã tìm thấy: " + state->currentCandidate;
                    TextOutW(hdc, 55, 200, currentText.c_str(), static_cast<int>(currentText.size()));
                }

                // Tiêu đề cho vùng cài đặt
                TextOutW(hdc, 55, 245, L"Cấu hình (Chọn trước khi kéo file):", 35);

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
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"ZipPasswordFinderWindow";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, L"ZipPasswordFinderWindow", L"ZIP/RAR Password Finder", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 760, 460, nullptr, nullptr, hInstance, nullptr);
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