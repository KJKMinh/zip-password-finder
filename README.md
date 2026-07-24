# 🔐 Zip/Rar Password Finder (5-Stage Engine)

![C++](https://img.shields.io/badge/C++-17-blue.svg?style=flat&logo=c%2B%2B)
![Windows](https://img.shields.io/badge/Platform-Windows-lightgrey.svg?style=flat&logo=windows)
![License](https://img.shields.io/badge/License-MIT-green.svg)

**Zip Password Finder** is a high-performance, multi-threaded GUI tool designed to recover lost passwords for protected ZIP and RAR archives. 

Instead of relying solely on blind brute-force, this tool implements a smart **5-Stage Attack Pipeline** (similar to industry-standard tools like Hashcat) to crack passwords efficiently using hardware optimization.

## ✨ Features

* **Supports Multiple Formats:** Recovers passwords for ZIP, RAR, and 7z archives seamlessly via the LZMA SDK (7z.dll).
* **Multi-threaded Engine:** Automatically detects and utilizes 100% of available physical CPU cores.
* **In-Memory Cracking:** Bypasses heavy string allocations and disk I/O bottlenecks by generating and testing combinations directly on RAM.
* **Modern GUI:** Clean, responsive Win32 interface with real-time statistics (Tested count, Elapsed time, Current stage).
* **Smart 5-Stage Pipeline:** Progressively attacks from the highest-probability passwords down to complete brute-force.

## 🚀 Attack Modes (The 5-Stage Pipeline)

1. **Dictionary Attack:** Tests millions of common passwords from imported text files (e.g., `rockyou.txt`).
2. **Rule Engine:** Dynamically mutates dictionary words (e.g., appending common numbers like `123`, `2026`, or special characters `!`).
3. **Mask Attack:** Targets predefined, highly probable structural masks (e.g., `Capital Letter` + `Letters` + `Numbers`).
4. **Probability Model (Leetspeak):** Simulates user behavior by swapping characters (e.g., replacing `a` with `@`, `e` with `3`).
5. **Full Brute-Force:** The ultimate fallback. Automatically generates pure combinations across the selected charset (Numbers, Lowercase, Uppercase, Symbols) up to the specified length.

## 🛠️ How to Compile (MinGW)

To build this project on Windows using MinGW (GCC), run the following command in PowerShell:

```powershell
cmd /c "g++ password.cpp src/*.cpp src/filesystem/*.cpp src/internal/*.cpp -o ZipPasswordFind.exe -std=c++17 -I include -I include/bit7z -I src -I 7z -I C -I lib -mwindows -DUNICODE -D_UNICODE -luser32 -lgdi32 -lshell32 -lole32 -loleaut32 -luuid -lstdc++fs -static-libgcc -static-libstdc++ -static"