#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <fstream>
#include <atomic>
#include <mutex>
#include <cwctype>
#include <bit7z/bit7z.hpp>

#define IDC_BTN_DICT    101
#define IDC_CHK_NUM     102
#define IDC_CHK_LOWER   103
#define IDC_CHK_UPPER   104
#define IDC_CHK_SYM     105
#define IDC_EDT_MAXLEN  106

namespace {
    constexpr UINT WM_APP_SEARCH_DONE = WM_APP + 1;
    constexpr UINT WM_APP_STATUS_UPDATE = WM_APP + 2;
    constexpr UINT WM_APP_STAGE_UPDATE = WM_APP + 3;

    enum class TaskType {
        DIRECT_TEST,
        BRUTE_PREFIX
    };

    struct Task {
        TaskType type;
        std::string data;
        int length = 0;
    };

    struct SharedSearchState {
        HWND hwnd = nullptr;
        std::string archivePathUtf8;
        std::string charset;
        std::vector<std::string> dictionary;
        
        std::vector<Task> taskQueue;
        size_t taskIndex = 0;
        std::mutex queueMutex;

        std::atomic<bool> stop{false};
        std::atomic<bool> found{false};
        std::atomic<uint64_t> testedCount{0};
        std::atomic<uint64_t> startMs{0};
        std::string foundPassword;
    };

    struct AppState {
        HWND hwnd = nullptr;
        std::wstring archivePath;
        std::wstring dictPath;
        std::wstring statusMessage = L"Kéo thả file ZIP/RAR vào vùng này để BẮT ĐẦU!";
        std::wstring currentStage = L"Đang chờ...";
        std::wstring currentCandidate;
        std::wstring foundPassword; // Đã bổ sung biến còn thiếu
        uint64_t testedCount = 0;
        uint64_t elapsedMs = 0;
        bool searching = false;

        HWND hBtnDict, hChkNum, hChkLower, hChkUpper, hChkSym, hEdtMaxLen;
    };

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

    std::wstring FormatNumber(uint64_t value) {
        wchar_t buffer[64] = {};
        swprintf(buffer, L"%llu", static_cast<unsigned long long>(value));
        return buffer;
    }

    void RefreshUI(HWND hwnd) {
        InvalidateRect(hwnd, nullptr, TRUE);
        UpdateWindow(hwnd);
    }

    bool TryCandidateInMemory(SharedSearchState* state, const bit7z::Bit7zLibrary& lib, const std::string& candidate) {
        if (state->stop.load(std::memory_order_relaxed)) return false;
        uint64_t tested = state->testedCount.fetch_add(1, std::memory_order_relaxed) + 1;

        try {
            const bit7z::BitInFormat* format = &bit7z::BitFormat::Zip;
            std::string pathLower = state->archivePathUtf8;
            for(auto& c : pathLower) c = tolower(c);
            
            if (pathLower.find(".rar") != std::string::npos) format = &bit7z::BitFormat::Rar5;
            else if (pathLower.find(".7z") != std::string::npos) format = &bit7z::BitFormat::SevenZip;

            bit7z::BitArchiveReader arc{ lib, state->archivePathUtf8, *format, candidate };
            arc.test();
            
            bool expected = false;
            if (state->found.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
                state->stop.store(true, std::memory_order_relaxed);
                state->foundPassword = candidate;
                return true;
            }
        } catch (const bit7z::BitException&) {}

        if (tested % 15000 == 0) {
            auto* cand = new std::string(candidate);
            PostMessage(state->hwnd, WM_APP_STATUS_UPDATE, reinterpret_cast<WPARAM>(cand), 0);
        }
        return false;
    }

    void ProcessBruteForceTask(SharedSearchState* state, const bit7z::Bit7zLibrary& lib, const Task& task) {
        int len = task.length;
        if (len == 1) { TryCandidateInMemory(state, lib, task.data); return; }

        const std::string& charset = state->charset;
        int num_chars = charset.length();
        std::vector<int> indices(len - 1, 0);
        std::string candidate(len, '\0');
        candidate[0] = task.data[0];

        while (!state->stop.load(std::memory_order_relaxed)) {
            for (int i = 0; i < len - 1; ++i) candidate[i + 1] = charset[indices[i]];
            if (TryCandidateInMemory(state, lib, candidate)) return;

            int p = len - 2;
            while (p >= 0) {
                if (++indices[p] < num_chars) break;
                indices[p] = 0;
                p--;
            }
            if (p < 0) break;
        }
    }

    DWORD WINAPI WorkerThread(LPVOID lpParam) {
        auto* state = static_cast<SharedSearchState*>(lpParam);
        try {
            bit7z::Bit7zLibrary lib{ "7z.dll" };
            while (!state->stop.load(std::memory_order_relaxed)) {
                Task task;
                {
                    std::lock_guard<std::mutex> lock(state->queueMutex);
                    if (state->taskIndex >= state->taskQueue.size()) break;
                    task = state->taskQueue[state->taskIndex++];
                }
                
                if (task.type == TaskType::DIRECT_TEST) TryCandidateInMemory(state, lib, task.data);
                else ProcessBruteForceTask(state, lib, task);
            }
        } catch (...) {}
        return 0;
    }

    void RunWorkersAndWait(SharedSearchState* state) {
        SYSTEM_INFO info{}; GetSystemInfo(&info);
        int numThreads = info.dwNumberOfProcessors > 0 ? info.dwNumberOfProcessors : 4;
        
        std::vector<HANDLE> handles;
        for (int i = 0; i < numThreads; ++i) {
            HANDLE h = CreateThread(nullptr, 0, WorkerThread, state, 0, nullptr);
            if (h) handles.push_back(h);
        }
        WaitForMultipleObjects(handles.size(), handles.data(), TRUE, INFINITE);
        for (HANDLE h : handles) CloseHandle(h);
    }

    void NotifyStage(HWND hwnd, const std::wstring& stageName) {
        auto* msg = new std::wstring(stageName);
        PostMessage(hwnd, WM_APP_STAGE_UPDATE, reinterpret_cast<WPARAM>(msg), 0);
    }

    DWORD WINAPI OrchestratorThread(LPVOID lpParam) {
        auto* state = static_cast<SharedSearchState*>(lpParam);
        state->startMs.store(GetTickCount(), std::memory_order_relaxed);

        if (!state->dictionary.empty() && !state->found.load()) {
            NotifyStage(state->hwnd, L"Giai đoạn 1/5: Tấn công Từ Điển...");
            state->taskQueue.clear(); state->taskIndex = 0;
            for (const auto& w : state->dictionary) state->taskQueue.push_back({TaskType::DIRECT_TEST, w});
            RunWorkersAndWait(state);
        }

        if (!state->dictionary.empty() && !state->found.load()) {
            NotifyStage(state->hwnd, L"Giai đoạn 2/5: Động cơ Quy tắc (Rule Engine)...");
            state->taskQueue.clear(); state->taskIndex = 0;
            for (const auto& w : state->dictionary) {
                state->taskQueue.push_back({TaskType::DIRECT_TEST, w + "123"});
                state->taskQueue.push_back({TaskType::DIRECT_TEST, w + "!"});
                state->taskQueue.push_back({TaskType::DIRECT_TEST, w + "2026"});
                for (int i = 1; i <= 99; ++i) state->taskQueue.push_back({TaskType::DIRECT_TEST, w + std::to_string(i)});
            }
            RunWorkersAndWait(state);
        }

        if (!state->found.load()) {
            NotifyStage(state->hwnd, L"Giai đoạn 3/5: Tấn công Mặt nạ (Mask Attack)...");
            state->taskQueue.clear(); state->taskIndex = 0;
            std::vector<std::string> baseMasks = {"Admin", "Password", "Welcome"};
            for (const auto& b : baseMasks) {
                for(int i=0; i<9999; i++) state->taskQueue.push_back({TaskType::DIRECT_TEST, b + std::to_string(i)});
            }
            RunWorkersAndWait(state);
        }

        if (!state->dictionary.empty() && !state->found.load()) {
            NotifyStage(state->hwnd, L"Giai đoạn 4/5: Mô hình Xác suất (Leetspeak)...");
            state->taskQueue.clear(); state->taskIndex = 0;
            for (auto w : state->dictionary) {
                for(char& c : w) { if(c=='a') c='@'; else if(c=='e') c='3'; else if(c=='i') c='1'; else if(c=='o') c='0'; }
                state->taskQueue.push_back({TaskType::DIRECT_TEST, w});
            }
            RunWorkersAndWait(state);
        }

        if (!state->found.load()) {
            NotifyStage(state->hwnd, L"Giai đoạn 5/5: Vét cạn (Brute-Force Toàn diện)...");
            state->taskQueue.clear(); state->taskIndex = 0;
            int maxLen = 6; 
            for (int len = 1; len <= maxLen; ++len) {
                for (char prefix : state->charset) {
                    state->taskQueue.push_back({TaskType::BRUTE_PREFIX, std::string(1, prefix), len});
                }
            }
            RunWorkersAndWait(state);
        }

        PostMessage(state->hwnd, WM_APP_SEARCH_DONE, 0, 0);
        return 0;
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        static AppState* app = nullptr;
        static SharedSearchState* searchState = nullptr;

        switch (msg) {
            case WM_CREATE: {
                app = new AppState(); app->hwnd = hwnd;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
                DragAcceptFiles(hwnd, TRUE);

                HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
                
                app->hBtnDict = CreateWindowW(L"BUTTON", L"Tải File Từ Điển (.txt)", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 55, 110, 200, 30, hwnd, (HMENU)IDC_BTN_DICT, nullptr, nullptr);
                app->hChkNum = CreateWindowW(L"BUTTON", L"Số (0-9)", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 55, 300, 100, 20, hwnd, (HMENU)IDC_CHK_NUM, nullptr, nullptr);
                app->hChkLower = CreateWindowW(L"BUTTON", L"Chữ (a-z)", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 160, 300, 100, 20, hwnd, (HMENU)IDC_CHK_LOWER, nullptr, nullptr);
                app->hChkUpper = CreateWindowW(L"BUTTON", L"Chữ (A-Z)", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 270, 300, 100, 20, hwnd, (HMENU)IDC_CHK_UPPER, nullptr, nullptr);
                app->hChkSym = CreateWindowW(L"BUTTON", L"Ký tự (!@...)", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 380, 300, 120, 20, hwnd, (HMENU)IDC_CHK_SYM, nullptr, nullptr);
                
                SendMessage(app->hBtnDict, WM_SETFONT, (WPARAM)hFont, TRUE);
                SendMessage(app->hChkNum, WM_SETFONT, (WPARAM)hFont, TRUE); SendMessage(app->hChkLower, WM_SETFONT, (WPARAM)hFont, TRUE);
                SendMessage(app->hChkUpper, WM_SETFONT, (WPARAM)hFont, TRUE); SendMessage(app->hChkSym, WM_SETFONT, (WPARAM)hFont, TRUE);
                
                SendMessage(app->hChkNum, BM_SETCHECK, BST_CHECKED, 0);
                SendMessage(app->hChkLower, BM_SETCHECK, BST_CHECKED, 0);
                return 0;
            }
            case WM_COMMAND: {
                if (LOWORD(wParam) == IDC_BTN_DICT && !app->searching) {
                    wchar_t filename[MAX_PATH] = {};
                    OPENFILENAMEW ofn = {};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFilter = L"Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
                    ofn.lpstrFile = filename;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
                    if (GetOpenFileNameW(&ofn)) {
                        app->dictPath = filename;
                        RefreshUI(hwnd);
                    }
                }
                return 0;
            }
            case WM_DROPFILES: {
                if (app->searching) { DragFinish((HDROP)wParam); return 0; }
                wchar_t path[MAX_PATH] = {};
                if (DragQueryFileW((HDROP)wParam, 0, path, MAX_PATH) > 0) {
                    app->archivePath = path;
                    app->searching = true; app->foundPassword.clear(); app->statusMessage = L"Hệ thống đang nạp dữ liệu...";
                    
                    searchState = new SharedSearchState();
                    searchState->hwnd = hwnd; searchState->archivePathUtf8 = ToUtf8(app->archivePath);
                    
                    if (SendMessage(app->hChkNum, BM_GETCHECK, 0, 0)) searchState->charset += "0123456789";
                    if (SendMessage(app->hChkLower, BM_GETCHECK, 0, 0)) searchState->charset += "abcdefghijklmnopqrstuvwxyz";
                    if (SendMessage(app->hChkUpper, BM_GETCHECK, 0, 0)) searchState->charset += "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
                    if (SendMessage(app->hChkSym, BM_GETCHECK, 0, 0)) searchState->charset += "!@#$%^&*()-_+=~`[]{}|:;\"'<>,.?/";
                    if (searchState->charset.empty()) searchState->charset = "0123456789abcdefghijklmnopqrstuvwxyz";
                    
                    if (!app->dictPath.empty()) {
                        // Sửa lỗi đọc file bằng cách convert đường dẫn wstring sang string (UTF-8)
                        std::string dictPathA = ToUtf8(app->dictPath);
                        std::ifstream file(dictPathA);
                        std::string line;
                        while (std::getline(file, line)) {
                            if(!line.empty()) searchState->dictionary.push_back(line);
                        }
                    }

                    CloseHandle(CreateThread(nullptr, 0, OrchestratorThread, searchState, 0, nullptr));
                    RefreshUI(hwnd);
                }
                DragFinish((HDROP)wParam);
                return 0;
            }
            case WM_APP_STAGE_UPDATE: {
                auto* msgStr = reinterpret_cast<std::wstring*>(wParam);
                app->currentStage = *msgStr;
                delete msgStr;
                RefreshUI(hwnd);
                return 0;
            }
            case WM_APP_STATUS_UPDATE: {
                auto* cand = reinterpret_cast<std::string*>(wParam);
                app->currentCandidate = ToWide(*cand);
                app->testedCount = searchState->testedCount.load(std::memory_order_relaxed);
                app->elapsedMs = GetTickCount() - searchState->startMs.load(std::memory_order_relaxed);
                delete cand;
                RefreshUI(hwnd);
                return 0;
            }
            case WM_APP_SEARCH_DONE: {
                app->searching = false;
                app->testedCount = searchState->testedCount.load(std::memory_order_relaxed);
                app->elapsedMs = GetTickCount() - searchState->startMs.load(std::memory_order_relaxed);
                if (searchState->found.load()) {
                    app->foundPassword = ToWide(searchState->foundPassword);
                    app->statusMessage = L"KHÓA ĐÃ BỊ PHÁ! Mật khẩu: " + app->foundPassword;
                } else {
                    app->statusMessage = L"Đã quét qua 5 giai đoạn nhưng không tìm thấy mật khẩu.";
                }
                delete searchState; searchState = nullptr;
                RefreshUI(hwnd);
                return 0;
            }
  case WM_PAINT: {
                PAINTSTRUCT ps{}; HDC hdc = BeginPaint(hwnd, &ps);
                RECT client{}; GetClientRect(hwnd, &client);

                HBRUSH bgBrush = CreateSolidBrush(RGB(248, 250, 252));
                FillRect(hdc, &client, bgBrush); DeleteObject(bgBrush);

                SetBkMode(hdc, TRANSPARENT);
                HFONT titleFont = CreateFontW(28, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
                HFONT oldFont = (HFONT)SelectObject(hdc, titleFont);
                SetTextColor(hdc, RGB(15, 23, 42));
                TextOutW(hdc, 55, 30, L"Hệ Thống Phá Khóa Đa Tầng (5-Stage Engine)", 44);

                HFONT bodyFont = CreateFontW(17, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
                SelectObject(hdc, bodyFont);

                std::wstring dictStr = app->dictPath.empty() ? L"Chưa nạp mảng tĩnh (Sẽ bỏ qua GĐ 1, 2, 4)" : L"Đã nạp: " + app->dictPath;
                SetTextColor(hdc, app->dictPath.empty() ? RGB(100, 116, 139) : RGB(34, 197, 94));
                TextOutW(hdc, 270, 115, dictStr.c_str(), dictStr.length());

                SetTextColor(hdc, RGB(15, 23, 42));
                std::wstring archStr = app->archivePath.empty() ? L"File ZIP/RAR: (Chưa chọn)" : L"File ZIP/RAR: " + app->archivePath;
                TextOutW(hdc, 55, 160, archStr.c_str(), archStr.length());

                SetTextColor(hdc, RGB(59, 130, 246));
                TextOutW(hdc, 55, 190, app->currentStage.c_str(), app->currentStage.length());

                if (!app->foundPassword.empty()) SetTextColor(hdc, RGB(22, 163, 74));
                else SetTextColor(hdc, RGB(225, 29, 72));
                TextOutW(hdc, 55, 220, app->statusMessage.c_str(), app->statusMessage.length());

                SetTextColor(hdc, RGB(15, 23, 42));
                std::wstring stats = L"Mật khẩu đã thử: " + FormatNumber(app->testedCount) + L"   |   Thời gian: " + FormatNumber(app->elapsedMs) + L" ms";
                TextOutW(hdc, 55, 250, stats.c_str(), stats.length());

                HFONT boldFont = CreateFontW(16, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
                SelectObject(hdc, boldFont);
                TextOutW(hdc, 55, 275, L"Cấu hình Vét Cạn (Giai đoạn 5):", 31);

                // --- THÊM TÊN TÁC GIẢ Ở ĐÂY ---
                HFONT authorFont = CreateFontW(15, 0, 0, 0, FW_NORMAL, TRUE, FALSE, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
                SelectObject(hdc, authorFont);
                SetTextColor(hdc, RGB(148, 163, 184)); // Màu xám nhạt tinh tế
                std::wstring authorName = L"Developed by Đinh Tuấn Minh (GitHub: KJKMinh / Fuuki)";
                TextOutW(hdc, 55, 380, authorName.c_str(), authorName.length());
                // ------------------------------

                SelectObject(hdc, oldFont);
                DeleteObject(titleFont); DeleteObject(bodyFont); DeleteObject(boldFont); DeleteObject(authorFont);
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_CTLCOLORSTATIC: return (LRESULT)GetStockObject(NULL_BRUSH);
            case WM_DESTROY: PostQuitMessage(0); return 0;
            default: return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    WNDCLASSW wc{}; wc.lpfnWndProc = WindowProc; wc.hInstance = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.lpszClassName = L"ZipPWFinder";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, L"ZipPWFinder", L"ZIP/RAR 5-Stage Password Recovery", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 450, nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    MSG msg{}; while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}   