QT += core gui widgets texttospeech xml webenginewidgets
CONFIG += c++17 release
CONFIG += link_pkgconfig
PKGCONFIG += poppler-qt6
TARGET = leafreader
TEMPLATE = app
SOURCES += main.cpp readerwindow.cpp pdfpageview.cpp
HEADERS += readerwindow.h pdfpageview.h
