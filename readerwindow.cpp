#include "readerwindow.h"
#include "pdfpageview.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QDomDocument>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QtEndian>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPalette>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <algorithm>

struct ReadingColors {
    QColor background;
    QColor page;
    QColor ink;
    QColor muted;
    QColor accent;
    QColor selectedInk;
};

static ReadingColors colorsForTheme(const QString &theme, const QPalette &systemPalette) {
    if (theme == "night")
        return {{"#171918"}, {"#202321"}, {"#e7e3d8"}, {"#9da49d"}, {"#8fbf9f"}, {"#171918"}};
    if (theme == "sepia")
        return {{"#d9c9a7"}, {"#efe2c2"}, {"#443828"}, {"#796b55"}, {"#8a5a32"}, {"#ffffff"}};
    if (theme == "paper")
        return {{"#deded9"}, {"#faf9f5"}, {"#292c29"}, {"#747a74"}, {"#477a59"}, {"#ffffff"}};

    return {
        systemPalette.color(QPalette::Window),
        systemPalette.color(QPalette::Base),
        systemPalette.color(QPalette::Text),
        systemPalette.color(QPalette::PlaceholderText),
        systemPalette.color(QPalette::Highlight),
        systemPalette.color(QPalette::HighlightedText)
    };
}

static QString cssRgba(QColor color, int alpha) {
    color.setAlpha(alpha);
    return QString("rgba(%1, %2, %3, %4)")
        .arg(color.red()).arg(color.green()).arg(color.blue())
        .arg(color.alphaF(), 0, 'f', 3);
}

static QString readUtf8(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

static QString piperVoiceLabel(const QString &modelPath) {
    const QString name = QFileInfo(modelPath).completeBaseName();
    const QStringList parts = name.split('-');
    if (parts.size() < 3) return name + " (Piper)";

    QString locale = parts.first();
    locale.replace('_', '-');
    QString voice = parts.mid(1, parts.size() - 2).join(' ');
    voice.replace('_', ' ');
    if (!voice.isEmpty()) voice[0] = voice[0].toUpper();
    QString quality = parts.last();
    if (!quality.isEmpty()) quality[0] = quality[0].toUpper();
    return QString("%1 — %2, %3 (Piper)").arg(voice, locale, quality);
}

static qint64 wavDurationMs(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return 0;
    const QByteArray wav = file.readAll();
    if (wav.size() < 44 || wav.left(4) != "RIFF" || wav.mid(8, 4) != "WAVE") return 0;
    const auto u16 = [&wav](int offset) { return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(wav.constData() + offset)); };
    const auto u32 = [&wav](int offset) { return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(wav.constData() + offset)); };
    const quint16 channels = u16(22);
    const quint32 sampleRate = u32(24);
    const quint16 bits = u16(34);
    const int dataTag = wav.indexOf("data", 36);
    if (!channels || !sampleRate || !bits || dataTag < 0 || dataTag + 8 > wav.size()) return 0;
    const quint32 dataBytes = u32(dataTag + 4);
    return qint64(dataBytes) * 8000 / (qint64(sampleRate) * channels * bits);
}

ReaderWindow::ReaderWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("Leaf Reader");
    setMinimumSize(820, 560);
    resize(1180, 780);
    setAcceptDrops(true);

    const QStringList speechEngines = QTextToSpeech::availableEngines();
    const QString speechEngine = speechEngines.contains("flite") ? QStringLiteral("flite") : QString();
    speech = new QTextToSpeech(speechEngine, this);
    connect(speech, &QTextToSpeech::stateChanged, this, &ReaderWindow::speechStateChanged);
    connect(speech, &QTextToSpeech::sayingWord, this, [this](const QString &, qsizetype, qsizetype start, qsizetype) {
        int word = 0;
        auto matches = QRegularExpression("\\S+").globalMatch(activeSpeechText.left(start));
        while (matches.hasNext()) { matches.next(); ++word; }
        if (speechIsPdf) pdfReader->setPlaybackWord(word);
        else reader->page()->runJavaScript(QString("window.__leafSetReadingWord?.(%1)").arg(word));
    });
    piperProcess = new QProcess(this);
    audioProcess = new QProcess(this);
    speechCursorTimer = new QTimer(this);
    speechCursorTimer->setInterval(55);
    connect(speechCursorTimer, &QTimer::timeout, this, &ReaderWindow::updateSpeechCursor);
    speechTemp = std::make_unique<QTemporaryDir>();

    auto *bar = addToolBar("Reader controls");
    bar->setMovable(false);
    auto *open = bar->addAction("Open book");
    open->setShortcut(QKeySequence::Open);
    connect(open, &QAction::triggered, this, &ReaderWindow::chooseBook);
    bar->addSeparator();

    speakButton = new QPushButton("Read aloud");
    speakButton->setEnabled(speech->state() != QTextToSpeech::Error);
    connect(speakButton, &QPushButton::clicked, this, &ReaderWindow::toggleSpeech);
    bar->addWidget(speakButton);

    cursorButton = new QPushButton("Reading cursor");
    cursorButton->setCheckable(true);
    cursorButton->setToolTip("Click in the document to choose where reading begins");
    connect(cursorButton, &QPushButton::toggled, this, &ReaderWindow::setReadingCursorEnabled);
    bar->addWidget(cursorButton);

    voiceBox = new QComboBox;
    refreshVoices();
    voiceBox->setToolTip("Voice");
    connect(voiceBox, &QComboBox::currentIndexChanged, this, [this](int i) {
        const QVariant data = voiceBox->itemData(i);
        if (data.canConvert<QVoice>()) speech->setVoice(data.value<QVoice>());
    });
    bar->addWidget(voiceBox);

    bar->addWidget(new QLabel("  Speed "));
    rateSlider = new QSlider(Qt::Horizontal);
    rateSlider->setRange(-8, 8); rateSlider->setValue(0); rateSlider->setFixedWidth(90);
    connect(rateSlider, &QSlider::valueChanged, this, [this](int value) { speech->setRate(value / 10.0); });
    bar->addWidget(rateSlider);

    bar->addSeparator();
    bar->addWidget(new QLabel(" Text "));
    fontSlider = new QSlider(Qt::Horizontal);
    fontSlider->setRange(14, 34); fontSlider->setValue(20); fontSlider->setFixedWidth(100);
    connect(fontSlider, &QSlider::valueChanged, this, &ReaderWindow::applyAppearance);
    bar->addWidget(fontSlider);

    themeBox = new QComboBox;
    themeBox->addItem("System", "system");
    themeBox->addItem("Paper", "paper");
    themeBox->addItem("Sepia", "sepia");
    themeBox->addItem("Night", "night");
    themeBox->setToolTip("Reading theme; app controls follow the system theme");
    connect(themeBox, &QComboBox::currentIndexChanged, this, &ReaderWindow::applyAppearance);
    bar->addWidget(themeBox);

    auto *splitter = new QSplitter;
    auto *sidebar = new QWidget;
    sidebar->setMinimumWidth(190); sidebar->setMaximumWidth(330);
    auto *sideLayout = new QVBoxLayout(sidebar);
    bookTitle = new QLabel("Your library starts here");
    bookTitle->setWordWrap(true);
    QFont titleFont = bookTitle->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 2.0);
    titleFont.setBold(true);
    bookTitle->setFont(titleFont);
    bookTitle->setContentsMargins(4, 8, 4, 8);
    sideLayout->addWidget(bookTitle);
    chaptersList = new QListWidget;
    sideLayout->addWidget(chaptersList, 1);
    progressLabel = new QLabel("Drop a book here, or press Ctrl+O");
    progressLabel->setWordWrap(true);
    sideLayout->addWidget(progressLabel);
    connect(chaptersList, &QListWidget::currentRowChanged, this, &ReaderWindow::showChapter);

    reader = new QWebEngineView;
    reader->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);
    reader->settings()->setAttribute(QWebEngineSettings::PdfViewerEnabled, true);
    reader->settings()->setAttribute(QWebEngineSettings::PluginsEnabled, true);
    connect(reader, &QWebEngineView::loadFinished, this, [this](bool loaded) {
        if (loaded) {
            applyDocumentAppearance();
            setReadingCursorEnabled(cursorButton->isChecked());
        }
    });
    reader->setHtml("<div style='margin:15%; text-align:center'><h1>Leaf Reader</h1><p>A quiet place for your books.</p><p>Open an EPUB, PDF, TXT, HTML, or Markdown file.</p></div>");
    pdfReader = new PdfPageView;
    contentStack = new QStackedWidget;
    contentStack->addWidget(reader);
    contentStack->addWidget(pdfReader);
    contentStack->setCurrentWidget(reader);
    splitter->addWidget(sidebar); splitter->addWidget(contentStack);
    splitter->setStretchFactor(1, 1); splitter->setSizes({250, 900});
    setCentralWidget(splitter);

    QSettings settings;
    restoreGeometry(settings.value("window/geometry").toByteArray());
    fontSlider->setValue(settings.value("appearance/fontSize", 20).toInt());
    QString readingTheme = settings.value("appearance/readingTheme").toString();
    if (readingTheme.isEmpty() && settings.contains("appearance/theme")) {
        const QStringList legacyThemes = {"paper", "sepia", "night"};
        const int legacyIndex = settings.value("appearance/theme").toInt();
        if (legacyIndex >= 0 && legacyIndex < legacyThemes.size()) readingTheme = legacyThemes[legacyIndex];
    }
    if (readingTheme.isEmpty()) readingTheme = "system";
    const int readingThemeIndex = themeBox->findData(readingTheme);
    themeBox->setCurrentIndex(readingThemeIndex >= 0 ? readingThemeIndex : 0);
    rateSlider->setValue(settings.value("speech/rate", 0).toInt());
    cursorButton->setChecked(settings.value("speech/readingCursor", true).toBool());
    applyAppearance();

    connect(piperProcess, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        if (status == QProcess::NormalExit && exitCode == 0 && speechTemp && speechTemp->isValid()) {
            const QString audioPath = speechTemp->filePath("speech.wav");
            speechDurationMs = wavDurationMs(audioPath);
            speechClock.start();
            speechCursorTimer->start();
            audioProcess->start("pw-play", {audioPath});
            speakButton->setText("Stop reading");
            speakButton->setToolTip("Playing with Piper neural voice");
        } else if (status != QProcess::NormalExit || exitCode != 0) {
            const QString error = QString::fromUtf8(piperProcess->readAllStandardError()).trimmed();
            speakButton->setText("Read aloud");
            speakButton->setToolTip(error.isEmpty() ? "Piper could not generate speech" : error);
        }
    });
    connect(audioProcess, &QProcess::finished, this, [this] {
        speechCursorTimer->stop();
        speakButton->setText("Read aloud");
        speakButton->setToolTip("Piper neural voice");
    });
}

ReaderWindow::~ReaderWindow() = default;

void ReaderWindow::chooseBook() {
    const QString path = QFileDialog::getOpenFileName(this, "Open a book", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        "Books (*.epub *.pdf *.txt *.html *.htm *.md);;All files (*)");
    if (!path.isEmpty()) openBook(path);
}

void ReaderWindow::openBook(const QString &path) {
    savePosition();
    stopSpeech(); chapters.clear(); chaptersList->clear(); epubTemp.reset();
    currentPath = QFileInfo(path).absoluteFilePath();
    const QString ext = QFileInfo(path).suffix().toLower();
    bool ok = ext == "epub" ? loadEpub(path) : ext == "pdf" ? loadPdf(path) : loadText(path);
    if (!ok || chapters.isEmpty()) { setError("I couldn't read this book. The file may be damaged or protected by DRM."); return; }
    populateBook(QFileInfo(path).completeBaseName());
}

bool ReaderWindow::loadText(const QString &path) {
    QString text = readUtf8(path);
    if (text.isEmpty()) return false;
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == "html" || ext == "htm") chapters.push_back({"Document", text, QFileInfo(path).absoluteFilePath()});
    else {
        if (ext == "md") {
            QTextDocument doc; doc.setMarkdown(text); text = doc.toHtml();
        } else text = QString("<pre style='white-space:pre-wrap'>%1</pre>").arg(text.toHtmlEscaped());
        chapters.push_back({"Document", text, {}});
    }
    return true;
}

bool ReaderWindow::loadPdf(const QString &path) {
    if (!pdfReader->load(path)) return false;
    QProcess process;
    process.start("pdftotext", {"-layout", path, "-"});
    QStringList pages;
    if (process.waitForFinished(30000) && process.exitCode() == 0)
        pages = QString::fromUtf8(process.readAllStandardOutput()).split(QChar::FormFeed);
    for (int page = 0; page < pdfReader->pageCount(); ++page) {
        const QString content = page < pages.size() ? pages[page] : QString();
        chapters.push_back({QString("Page %1").arg(page + 1),
            QString("<pre style='white-space:pre-wrap'>%1</pre>").arg(content.toHtmlEscaped()),
            QFileInfo(path).absoluteFilePath()});
    }
    return !chapters.isEmpty();
}

bool ReaderWindow::loadEpub(const QString &path) {
    QProcess list;
    list.start("bsdtar", {"-tf", path});
    if (!list.waitForFinished(15000) || list.exitCode() != 0) return false;
    for (QString entry : QString::fromUtf8(list.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts)) {
        entry = QDir::cleanPath(entry);
        if (entry.startsWith('/') || entry == ".." || entry.startsWith("../")) return false;
    }
    epubTemp = std::make_unique<QTemporaryDir>();
    if (!epubTemp->isValid()) return false;
    QProcess extract;
    extract.setWorkingDirectory(epubTemp->path());
    extract.start("bsdtar", {"-xf", path});
    if (!extract.waitForFinished(30000) || extract.exitCode() != 0) return false;

    QDomDocument container;
    if (!container.setContent(readUtf8(epubTemp->filePath("META-INF/container.xml")))) return false;
    const QString opfRel = container.elementsByTagName("rootfile").at(0).toElement().attribute("full-path");
    if (opfRel.isEmpty()) return false;
    const QString opfPath = epubTemp->filePath(opfRel);
    const QDir opfDir = QFileInfo(opfPath).dir();
    QDomDocument opf;
    if (!opf.setContent(readUtf8(opfPath))) return false;

    QHash<QString, QString> items;
    QHash<QString, QString> titles;
    auto manifests = opf.elementsByTagName("item");
    for (int i = 0; i < manifests.count(); ++i) {
        auto el = manifests.at(i).toElement();
        items.insert(el.attribute("id"), el.attribute("href"));
    }
    auto spine = opf.elementsByTagName("itemref");
    int number = 1;
    for (int i = 0; i < spine.count(); ++i) {
        QString href = items.value(spine.at(i).toElement().attribute("idref"));
        if (href.isEmpty()) continue;
        const QString chapterPath = opfDir.filePath(QUrl::fromPercentEncoding(href.toUtf8()));
        QString html = readUtf8(chapterPath);
        if (html.isEmpty()) continue;
        QRegularExpression titleRx("<title[^>]*>(.*?)</title>", QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
        QString title = titleRx.match(html).captured(1).trimmed();
        if (title.isEmpty()) title = QString("Chapter %1").arg(number);
        chapters.push_back({title, html, chapterPath}); number++;
    }
    return !chapters.isEmpty();
}

void ReaderWindow::populateBook(const QString &title) {
    bookTitle->setText(title);
    for (const auto &chapter : chapters) chaptersList->addItem(chapter.title);
    setWindowTitle(title + " — Leaf Reader");
    restorePosition();
}

void ReaderWindow::showChapter(int index) {
    if (index < 0 || index >= chapters.size()) return;
    currentChapter = index;
    const Chapter &chapter = chapters[index];
    const QString ext = QFileInfo(currentPath).suffix().toLower();
    if (ext == "pdf") {
        contentStack->setCurrentWidget(pdfReader);
        pdfReader->setPage(index);
    } else if (!chapter.source.isEmpty()) {
        contentStack->setCurrentWidget(reader);
        reader->load(QUrl::fromLocalFile(chapter.source));
    } else {
        contentStack->setCurrentWidget(reader);
        reader->setHtml(chapter.html, QUrl::fromLocalFile(QFileInfo(currentPath).absolutePath() + "/"));
    }
    updateProgress();
}

void ReaderWindow::speakText(const QString &rawText) {
    const QString text = rawText.trimmed().left(12000);
    if (text.isEmpty()) return;
    activeSpeechText = text;
    speechIsPdf = QFileInfo(currentPath).suffix().toLower() == "pdf";
    if (voiceBox->currentData().toString().startsWith("piper:")) startPiper(text);
    else speech->say(text);
}

void ReaderWindow::updateSpeechCursor() {
    if (speechDurationMs <= 0 || speechWordWeights.isEmpty()) return;
    const double progress = std::clamp(double(speechClock.elapsed()) / speechDurationMs, 0.0, 1.0);
    const double target = progress * speechWordWeights.last();
    const int word = std::clamp(int(std::lower_bound(speechWordWeights.cbegin(), speechWordWeights.cend(), target)
        - speechWordWeights.cbegin()), 0, int(speechWordWeights.size()) - 1);
    if (speechIsPdf) pdfReader->setPlaybackWord(word);
    else reader->page()->runJavaScript(QString("window.__leafSetReadingWord?.(%1)").arg(word));
}

void ReaderWindow::clearSpeechHighlight() {
    speechCursorTimer->stop();
    reader->page()->runJavaScript("CSS.highlights?.delete('leaf-reading-word')");
}

void ReaderWindow::setReadingCursorEnabled(bool enabled) {
    pdfReader->setCursorEnabled(enabled);
    const QString script = QString(R"JS(
        (() => {
          window.__leafCursorEnabled = %1;
          const old = document.getElementById('__leaf_reader_cursor');
          if (!window.__leafCursorEnabled) {
            if (old) old.remove();
            return;
          }
          if (!document.getElementById('__leaf_reader_cursor_style')) {
            const style = document.createElement('style');
            style.id = '__leaf_reader_cursor_style';
            style.textContent = `#__leaf_reader_cursor {
              display: inline-block !important; width: 3px !important;
              height: 1.15em !important; margin: 0 2px !important;
              vertical-align: text-bottom !important; border-radius: 2px !important;
              background: var(--leaf-reader-accent, #55b879) !important;
              box-shadow: 0 0 0 2px var(--leaf-reader-accent-faint, #55b87933) !important;
              animation: leaf-reader-blink 1.1s step-end infinite !important;
            }
            @keyframes leaf-reader-blink { 50% { opacity: .25; } }`;
            (document.head || document.documentElement).appendChild(style);
          }
          window.__leafPlaceCursor = (node, offset, shouldScroll = false) => {
            if (!node) return false;
            document.getElementById('__leaf_reader_cursor')?.remove();
            const marker = document.createElement('span');
            marker.id = '__leaf_reader_cursor';
            marker.setAttribute('role', 'mark');
            marker.setAttribute('aria-label', 'Reading starts here');
            const range = document.createRange();
            try {
              const maximum = node.nodeType === Node.TEXT_NODE ? node.length : node.childNodes.length;
              range.setStart(node, Math.max(0, Math.min(offset, maximum)));
              range.collapse(true);
              range.insertNode(marker);
              if (shouldScroll) marker.scrollIntoView({ block: 'center', behavior: 'smooth' });
              return true;
            } catch (_) {
              marker.remove();
              return false;
            }
          };
          if (!window.__leafCursorHandlerInstalled) {
            document.addEventListener('click', (event) => {
              if (!window.__leafCursorEnabled || event.button !== 0) return;
              if (event.target instanceof Element && event.target.closest('a, button, input, select, textarea')) return;
              const position = document.caretPositionFromPoint
                ? document.caretPositionFromPoint(event.clientX, event.clientY) : null;
              const legacy = !position && document.caretRangeFromPoint
                ? document.caretRangeFromPoint(event.clientX, event.clientY) : null;
              const node = position ? position.offsetNode : legacy ? legacy.startContainer : null;
              const offset = position ? position.offset : legacy ? legacy.startOffset : 0;
              if (!node || (node.nodeType !== Node.TEXT_NODE && node.nodeType !== Node.ELEMENT_NODE)) return;
              window.__leafPlaceCursor(node, offset, true);
            }, true);
            window.__leafCursorHandlerInstalled = true;
          }
          if (!old && document.body) {
            const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT, {
              acceptNode: (node) => node.textContent.trim() &&
                !node.parentElement?.closest('script, style, noscript')
                  ? NodeFilter.FILTER_ACCEPT : NodeFilter.FILTER_REJECT
            });
            const firstText = walker.nextNode();
            if (firstText) window.__leafPlaceCursor(firstText, 0, false);
          }
        })();
    )JS").arg(enabled ? "true" : "false");
    reader->page()->runJavaScript(script);
}

void ReaderWindow::toggleSpeech() {
    if (piperIsActive() || speech->state() == QTextToSpeech::Speaking || speech->state() == QTextToSpeech::Paused) {
        stopSpeech();
        return;
    }

    if (QFileInfo(currentPath).suffix().toLower() == "pdf" && currentChapter < chapters.size()) {
        speakText(pdfReader->speechText());
        return;
    }
    reader->page()->runJavaScript(
        "(() => { const selected = window.getSelection().toString().trim(); "
        "const marker = document.getElementById('__leaf_reader_cursor'); const scope = document.createRange(); "
        "if (selected) { const chosen = window.getSelection().getRangeAt(0); scope.setStart(chosen.startContainer, chosen.startOffset); scope.setEnd(chosen.endContainer, chosen.endOffset); } "
        "else { scope.selectNodeContents(document.body); if (marker) scope.setStartAfter(marker); } "
        "const ranges = []; const root = scope.commonAncestorContainer; const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT); "
        "if (root.nodeType === Node.TEXT_NODE) { const text = root.data.slice(scope.startOffset, scope.endOffset); "
        "for (const match of text.matchAll(/\\S+/g)) { const range = document.createRange(); range.setStart(root, scope.startOffset + match.index); range.setEnd(root, scope.startOffset + match.index + match[0].length); ranges.push(range); } } "
        "else while (walker.nextNode()) { const node = walker.currentNode; if (!scope.intersectsNode(node)) continue; "
        "const start = node === scope.startContainer ? scope.startOffset : 0; const end = node === scope.endContainer ? scope.endOffset : node.length; "
        "for (const match of node.data.slice(start, end).matchAll(/\\S+/g)) { const range = document.createRange(); range.setStart(node, start + match.index); range.setEnd(node, start + match.index + match[0].length); ranges.push(range); } } "
        "window.__leafReadingWordRanges = ranges; window.__leafSetReadingWord = (index) => { if (!CSS.highlights || !ranges.length) return; "
        "const range = ranges[Math.max(0, Math.min(index, ranges.length - 1))]; CSS.highlights.set('leaf-reading-word', new Highlight(range)); "
        "range.startContainer.parentElement?.scrollIntoView({block:'center', behavior:'smooth'}); }; "
        "if (!document.getElementById('__leaf_reading_highlight_style')) { const style = document.createElement('style'); style.id='__leaf_reading_highlight_style'; "
        "style.textContent='::highlight(leaf-reading-word){background:var(--leaf-reader-accent-soft,#55b87999);color:inherit}'; (document.head || document.documentElement).appendChild(style); } "
        "return scope.toString(); })()",
        [this](const QVariant &result) { speakText(result.toString()); });
}

bool ReaderWindow::piperIsActive() const {
    return piperProcess->state() != QProcess::NotRunning || audioProcess->state() != QProcess::NotRunning;
}

void ReaderWindow::stopSpeech() {
    clearSpeechHighlight();
    speech->stop();
    if (piperProcess->state() != QProcess::NotRunning) piperProcess->kill();
    if (audioProcess->state() != QProcess::NotRunning) audioProcess->kill();
    speakButton->setText("Read aloud");
}

void ReaderWindow::startPiper(const QString &text) {
    const QString dataRoot = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/leafreader";
    const QString python = dataRoot + "/piper-venv/bin/python";
    const QString selected = voiceBox->currentData().toString();
    const QString model = selected.startsWith("piper:") ? selected.mid(6) : QString();
    if (!QFileInfo::exists(python) || !QFileInfo::exists(model) || !speechTemp || !speechTemp->isValid()) {
        speakButton->setToolTip("Piper or the Lessac voice model is not installed");
        return;
    }

    const double lengthScale = std::clamp(1.0 - rateSlider->value() / 16.0, 0.55, 1.6);
    speechIsPdf = QFileInfo(currentPath).suffix().toLower() == "pdf";
    speechWordWeights.clear();
    double cumulativeWeight = 0;
    auto words = QRegularExpression("\\S+").globalMatch(text);
    const QRegularExpression longPause("[.!?][\\\"')\\]]*$");
    const QRegularExpression shortPause("[,;:][\\\"')\\]]*$");
    while (words.hasNext()) {
        const QString word = words.next().captured();
        cumulativeWeight += std::max(1, int(word.size()));
        if (longPause.match(word).hasMatch()) cumulativeWeight += 6;
        else if (shortPause.match(word).hasMatch()) cumulativeWeight += 2.5;
        speechWordWeights.append(cumulativeWeight);
    }
    const QString output = speechTemp->filePath("speech.wav");
    piperProcess->setProgram(python);
    piperProcess->setArguments({"-m", "piper", "-m", model, "-f", output,
        "--length-scale", QString::number(lengthScale), "--", text});
    piperProcess->start();
    speakButton->setText("Stop reading");
    speakButton->setToolTip("Generating speech with Piper…");
}

void ReaderWindow::refreshVoices() {
    const QString selected = voiceBox->currentData().toString();
    voiceBox->clear();
    const QString voicesRoot = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + "/leafreader/voices";
    QStringList piperModels;
    QDirIterator models(voicesRoot, {"*.onnx"}, QDir::Files, QDirIterator::Subdirectories);
    while (models.hasNext()) piperModels.append(models.next());
    std::sort(piperModels.begin(), piperModels.end());
    for (const QString &model : std::as_const(piperModels))
        if (QFileInfo::exists(model + ".json"))
            voiceBox->addItem(piperVoiceLabel(model), "piper:" + model);
    for (const QVoice &voice : speech->availableVoices())
        voiceBox->addItem(voice.name(), QVariant::fromValue(voice));

    const bool hasVoices = voiceBox->count() > 0;
    if (!hasVoices)
        voiceBox->addItem(speech->state() == QTextToSpeech::Error
            ? QStringLiteral("Speech unavailable") : QStringLiteral("Loading voices…"));
    voiceBox->setEnabled(hasVoices);
    const int previous = voiceBox->findData(selected);
    if (previous >= 0) voiceBox->setCurrentIndex(previous);
}

void ReaderWindow::speechStateChanged(QTextToSpeech::State state) {
    speakButton->setText(state == QTextToSpeech::Speaking ? "Stop reading" : "Read aloud");
    speakButton->setEnabled(state != QTextToSpeech::Error);
    if (state == QTextToSpeech::Ready) {
        speakButton->setToolTip(QString("Speech engine: %1").arg(speech->engine()));
        refreshVoices();
    } else if (state == QTextToSpeech::Error) {
        const QString error = speech->errorString().isEmpty() ? QStringLiteral("Speech backend failed to initialize") : speech->errorString();
        speakButton->setToolTip(error);
        refreshVoices();
    }
}

void ReaderWindow::applyAppearance() {
    reader->setZoomFactor(fontSlider->value() / 20.0);
    pdfReader->setZoomFactor(fontSlider->value() / 20.0);
    applyDocumentAppearance();
}

void ReaderWindow::applyDocumentAppearance() {
    if (!themeBox || !reader || !pdfReader) return;

    const QString theme = themeBox->currentData().toString();
    const ReadingColors colors = colorsForTheme(theme, qApp->palette());
    const bool dark = colors.page.lightnessF() < 0.5;
    const QString accentSoft = cssRgba(colors.accent, 105);
    const QString accentFaint = cssRgba(colors.accent, 50);

    QPalette pdfPalette = pdfReader->palette();
    pdfPalette.setColor(QPalette::Window, colors.background);
    pdfReader->setPalette(pdfPalette);
    pdfReader->setAccentColor(colors.accent);
    reader->page()->setBackgroundColor(colors.page);

    const QString css = QString(R"CSS(
        :root {
          color-scheme: %1;
          --leaf-reader-page: %2;
          --leaf-reader-ink: %3;
          --leaf-reader-muted: %4;
          --leaf-reader-accent: %5;
          --leaf-reader-accent-soft: %6;
          --leaf-reader-accent-faint: %7;
        }
        html, body {
          background-color: var(--leaf-reader-page) !important;
          color: var(--leaf-reader-ink) !important;
        }
        body { accent-color: var(--leaf-reader-accent); }
        a:any-link { color: var(--leaf-reader-accent) !important; }
        hr, blockquote { border-color: var(--leaf-reader-muted) !important; }
        ::selection { background: var(--leaf-reader-accent-soft); color: %8; }
    )CSS").arg(dark ? "dark" : "light", colors.page.name(), colors.ink.name(),
                 colors.muted.name(), colors.accent.name(), accentSoft, accentFaint,
                 colors.selectedInk.name());

    reader->page()->runJavaScript(QString(R"JS(
        (() => {
          if (!document.documentElement) return;
          let style = document.getElementById('__leaf_reader_appearance');
          if (!style) {
            style = document.createElement('style');
            style.id = '__leaf_reader_appearance';
            (document.head || document.documentElement).appendChild(style);
          }
          style.textContent = `%1`;
        })();
    )JS").arg(css));
}

void ReaderWindow::updateProgress() {
    if (chapters.isEmpty()) return;
    int percent = int(100.0 * currentChapter / chapters.size());
    progressLabel->setText(QString("%1%  •  %2 of %3").arg(percent).arg(currentChapter + 1).arg(chapters.size()));
}

QString ReaderWindow::bookKey() const {
    return QString::fromLatin1(QCryptographicHash::hash(currentPath.toUtf8(), QCryptographicHash::Sha256).toHex().left(20));
}

void ReaderWindow::savePosition() {
    if (currentPath.isEmpty() || chapters.isEmpty()) return;
    QSettings s;
    s.setValue("books/" + bookKey() + "/chapter", currentChapter);
}

void ReaderWindow::restorePosition() {
    QSettings s;
    int chapter = std::clamp(s.value("books/" + bookKey() + "/chapter", 0).toInt(), 0, int(chapters.size()) - 1);
    chaptersList->setCurrentRow(chapter);
}

void ReaderWindow::setError(const QString &message) {
    bookTitle->setText("Couldn't open book"); progressLabel->setText(message);
    reader->setHtml(QString("<div style='margin:15%%'><h2>That book didn't open</h2><p>%1</p></div>").arg(message.toHtmlEscaped()));
}

void ReaderWindow::changeEvent(QEvent *event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ApplicationPaletteChange && themeBox && reader
        && pdfReader && fontSlider
        && themeBox->currentData().toString() == "system")
        applyAppearance();
}

void ReaderWindow::closeEvent(QCloseEvent *event) {
    savePosition();
    stopSpeech();
    QSettings s; s.setValue("window/geometry", saveGeometry()); s.setValue("appearance/fontSize", fontSlider->value());
    s.setValue("appearance/readingTheme", themeBox->currentData().toString()); s.setValue("speech/rate", rateSlider->value());
    s.setValue("speech/readingCursor", cursorButton->isChecked());
    QMainWindow::closeEvent(event);
}

void ReaderWindow::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void ReaderWindow::dropEvent(QDropEvent *event) {
    if (!event->mimeData()->urls().isEmpty()) openBook(event->mimeData()->urls().first().toLocalFile());
}
