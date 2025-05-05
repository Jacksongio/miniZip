# miniZip - Reverse Engineered WinZip Compressor

## Overview

**miniZip** is a lightweight C program that emulates the compression behavior of **WinZip** by manually writing valid ZIP file structures and using `zlib` for DEFLATE compression. This project was built as part of a **Software Reverse Engineering** course at Virginia Tech, based on a detailed disassembly and analysis of the original WinZip binary.

The goal of this project was to reverse engineer WinZip’s compression algorithm and reimplement its functionality from scratch to better understand how ZIP archives are created and how compression settings affect file sizes.

---

## Project Objectives

- Reverse engineer the WinZip executable using IDA Free and Ghidra
- Identify core compression logic and ZIP file format structure
- Recreate a functional ZIP creator in C using zlib
- Understand and control compression behavior via raw ZIP format manipulation

---

## Features

- Compresses multiple files into a ZIP archive
- Manually writes ZIP headers:
  - Local File Headers
  - Central Directory Headers
  - End of Central Directory Record
- Uses raw `zlib` DEFLATE compression (level 6 by default)
- Handles file metadata including CRC32, file size, and timestamps
- Fully cross-platform C code

---

## File Structure

```
winzip-project/
├── winzip.c           # Core ZIP file creator using zlib
├── sample.txt         # Sample input file (can be replaced)
├── archive.zip        # Output ZIP archive created by winzip.c
├── README.md          # Project documentation (this file)
└── Makefile           # (Optional) Build script for compilation
```

---

## Compression Logic

The file `winzip.c` replicates the ZIP file structure as follows:

1. **Reads and compresses each input file using `zlib`**
2. **Calculates CRC32 checksum and size information**
3. **Writes local file headers and compressed data**
4. **Stores central directory records in memory**
5. **Appends central directory and End-of-Central-Directory (EOCD) records**

All structures are written in **little-endian** format, and binary-level correctness was verified with third-party ZIP extractors.

---

## How to Build

You’ll need `zlib` installed on your system. Then run:

```bash
gcc winzip.c -lz -o winzip
```

---

## How to Run

```bash
./winzip output.zip input1.txt input2.png input3.docx
```

This will create a ZIP archive `output.zip` containing the listed input files.

---

## Tools Used

- **IDA Free / Ghidra** – Static binary analysis of WinZip
- **zlib** – Compression engine (DEFLATE)
- **C** – Low-level control over binary ZIP format
- **x64dbg / PE Explorer** – Supplementary analysis
- **ZIP File Format Spec** – Manual header formatting

---

## Research Outcome

This project demonstrated how commercial tools like WinZip build ZIP files and revealed opportunities for further compression tuning. Manual control of ZIP header values offered insight into compatibility, file size, and extractability behaviors.

---

## ⚠️ Legal Notice

This project is for **educational purposes only**. All analysis and reimplementation were done in a controlled environment for academic use. WinZip is a trademark of Corel Corporation.

---

## Author

**Jackson Giordano**  
Computer Science Student @ Virginia Tech  
Course: Software Reverse Engineering – Spring 2025

---

## References

- [ZIP File Format Spec (PKWARE)](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT)
- [zlib Home Page](https://zlib.net)
- [Ghidra](https://ghidra-sre.org/)
- [WinZip](https://www.winzip.com/)

---
## 🚀 What's Next?

To make miniZip more accessible and user-friendly, the next step in this project is building a **graphical user interface (GUI)**.

### Planned Features:
- Drag-and-drop support for selecting files
- File preview before compression
- Compression level slider (1–9)
- Progress bar during compression
- ZIP archive viewer (list contents, extract)

### Technology Stack:
- **Frontend GUI**: Likely built with **Python (Tkinter or PyQt)** or **Electron + Node.js** for cross-platform desktop support
- **Backend**: Wrap the existing C logic using **Python C bindings (ctypes or CFFI)** or reimplement compression in Python using `zlib` for simplicity

This enhancement will provide a user-friendly alternative to the CLI version and simulate the usability of commercial tools like WinZip.

Stay tuned for the **miniZip GUI Edition**!

