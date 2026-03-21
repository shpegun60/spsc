# SPSC Test Project

Main integration project for the `spsc` buffer library and paranoid API tests.

## What Is Here

- `src/spsc/`: core SPSC library headers (`fifo`, `queue`, `typed_pool`, `fifo_view`, `pool`, `pool_view`, `latest`, `chunk`, etc.).
- `src/*_test.cpp`: paranoid test suites for each buffer type.
- `spsc_test.pro`: Qt/qmake project file.
- `mainwindow.cpp`: Qt dashboard that runs suites, shows `PASS/FAIL`, logs, and timeout status.

Detailed API documentation is in `src/spsc/README.md`.

## Build

Prerequisites:

- Qt 6.x with `QtTest`
- MinGW toolchain (or compatible C++ toolchain configured in Qt)

Typical qmake build flow (Windows/MinGW):

```powershell
mkdir build
cd build
qmake ..\spsc_test.pro
mingw32-make -j8
```

If you use Qt Creator, opening `spsc_test.pro` is enough.

## Run Tests

Launch the Qt dashboard:

```powershell
.\debug\spsc_test.exe
```

The dashboard:

- runs each suite in a separate child process
- shows `PASS`, `FAIL`, `TIMEOUT`, or `CRASH`
- captures the full QtTest log in a text pane
- supports a per-suite timeout from the UI

Main suites include:

- `fifo`
- `fifo_view`
- `pool`
- `pool_view`
- `latest`
- `chunk`
- `queue`
- `typed_pool`

## Qt Dashboard

Screenshot of the integrated Qt runner with all suites passing:

![Qt dashboard with all test suites passed](img/Screenshot%202026-03-21%20141744.png)

## Latest Test Report (Integrated Run)

Run source: `build/Desktop_Qt_6_10_1_MinGW_64_bit-Debug/debug/spsc_test.exe`

Environment:

- QtTest 6.10.1 / Qt 6.10.1
- GCC 13.1.0
- Windows 11

Observed startup warning (non-test-fatal):

- `qt.qpa.window: SetProcessDpiAwarenessContext() failed: Access is denied.`

Per-suite totals:

- `fifo`: 46 passed, 0 failed
- `fifo_view`: 47 passed, 0 failed
- `pool`: 19 passed, 0 failed
- `pool_view`: 12 passed, 0 failed
- `latest`: 28 passed, 0 failed
- `chunk`: 11 passed, 0 failed
- `queue`: 47 passed, 0 failed
- `typed_pool`: 60 passed, 0 failed

Conclusion:

- All suites passed, including `death_tests_debug_only`.

## Notes About Death Tests

Debug death tests use child process spawning (`QProcess`).  
In restricted environments they can fail with:

- `QProcess: CreateFile failed. (Access is denied.)`

In a normal local dev environment these tests should pass.
