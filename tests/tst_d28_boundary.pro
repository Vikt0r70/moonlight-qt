# D-28 fork-boundary tests (Plan 03-01 truth 3, STREAM-06).
#
# The boundary - the streaming engine is upstream's except for two ADR-named exceptions - was
# measured by hand in each verification pass and by a CI job that has never run and is not a
# required check (security F-2). This project is that measurement as a suite, with whole-path
# comparison instead of the gate's unanchored `grep -Ev 'overlaymanager|session\.cpp'`.
#
# It needs no Qt module beyond core/testlib and no engine: it grades the repository with `git` and
# builds a throwaway one to prove the check can fail.
#
# Build recipe (nothing is on PATH machine-wide - Qt and MSVC are both absolute):
#   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
#   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
#   cd tests && qmake tst_d28_boundary.pro && jom && tst_d28_boundary.exe -o tst_d28_boundary-out.txt,txt

QT += core testlib

CONFIG += console testcase c++17
CONFIG -= app_bundle debug_and_release debug

TEMPLATE = app
TARGET = tst_d28_boundary
DESTDIR = $$OUT_PWD

DEFINES += FORK_ROOT=\\\"$$PWD/..\\\"

SOURCES += tst_d28_boundary.cpp
