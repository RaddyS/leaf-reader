#include <QApplication>
#include <QCommandLineParser>
#include "readerwindow.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Leaf Reader");
    QApplication::setOrganizationName("LeafReader");
    QApplication::setApplicationVersion("1.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("A calm, private ebook reader");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("book", "Book to open (EPUB, PDF, TXT, HTML or Markdown)");
    parser.process(app);

    ReaderWindow window;
    window.show();
    if (!parser.positionalArguments().isEmpty())
        window.openBook(parser.positionalArguments().first());
    return app.exec();
}
