#include "readerwindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QDomDocument>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <algorithm>

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

ReaderWindow::ReaderWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("Leaf Reader");
    setMinimumSize(820, 560);
    resize(1180, 780);
    setAcceptDrops(true);

    const QStringList speechEngines = QTextToSpeech::availableEngines();
    const QString speechEngine = speechEngines.contains("flite") ? QStringLiteral("flite") : QString();
    speech = new QTextToSpeech(speechEngine, this);
    connect(speech, &QTextToSpeech::stateChanged, this, &ReaderWindow::speechStateChanged);
    piperProcess = new QProcess(this);
    audioProcess = new QProcess(this);
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
    themeBox->addItems({"Paper", "Sepia", "Night"});
    connect(themeBox, &QComboBox::currentIndexChanged, this, &ReaderWindow::applyAppearance);
    bar->addWidget(themeBox);

    auto *splitter = new QSplitter;
    auto *sidebar = new QWidget;
    sidebar->setMinimumWidth(190); sidebar->setMaximumWidth(330);
    auto *sideLayout = new QVBoxLayout(sidebar);
    bookTitle = new QLabel("Your library starts here");
    bookTitle->setWordWrap(true);
    bookTitle->setObjectName("bookTitle");
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
    reader->setHtml("<div style='margin:15%; text-align:center'><h1>Leaf Reader</h1><p>A quiet place for your books.</p><p>Open an EPUB, PDF, TXT, HTML, or Markdown file.</p></div>");
    splitter->addWidget(sidebar); splitter->addWidget(reader);
    splitter->setStretchFactor(1, 1); splitter->setSizes({250, 900});
    setCentralWidget(splitter);

    QSettings settings;
    restoreGeometry(settings.value("window/geometry").toByteArray());
    fontSlider->setValue(settings.value("appearance/fontSize", 20).toInt());
    themeBox->setCurrentIndex(settings.value("appearance/theme", 0).toInt());
    rateSlider->setValue(settings.value("speech/rate", 0).toInt());
    cursorButton->setChecked(settings.value("speech/readingCursor", true).toBool());
    applyAppearance();

    connect(reader, &QWebEngineView::loadFinished, this, [this](bool loaded) {
        if (loaded) setReadingCursorEnabled(cursorButton->isChecked());
    });

    connect(piperProcess, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        if (status == QProcess::NormalExit && exitCode == 0 && speechTemp && speechTemp->isValid()) {
            audioProcess->start("pw-play", {speechTemp->filePath("speech.wav")});
            speakButton->setText("Stop reading");
            speakButton->setToolTip("Playing with Piper neural voice");
        } else if (status != QProcess::NormalExit || exitCode != 0) {
            const QString error = QString::fromUtf8(piperProcess->readAllStandardError()).trimmed();
            speakButton->setText("Read aloud");
            speakButton->setToolTip(error.isEmpty() ? "Piper could not generate speech" : error);
        }
    });
    connect(audioProcess, &QProcess::finished, this, [this] {
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
    QProcess process;
    process.start("pdftotext", {"-layout", path, "-"});
    if (!process.waitForFinished(30000) || process.exitCode() != 0) return false;
    QString text = QString::fromUtf8(process.readAllStandardOutput());
    if (text.trimmed().isEmpty()) return false;
    const QStringList pages = text.split(QChar::FormFeed);
    int page = 1;
    for (const QString &content : pages)
        if (!content.trimmed().isEmpty()) chapters.push_back({QString("Page %1").arg(page++), QString("<pre style='white-space:pre-wrap'>%1</pre>").arg(content.toHtmlEscaped()), QFileInfo(path).absoluteFilePath()});
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
        QUrl url = QUrl::fromLocalFile(currentPath);
        url.setFragment(QString("page=%1").arg(index + 1));
        reader->load(url);
    } else if (!chapter.source.isEmpty()) {
        reader->load(QUrl::fromLocalFile(chapter.source));
    } else {
        reader->setHtml(chapter.html, QUrl::fromLocalFile(QFileInfo(currentPath).absolutePath() + "/"));
    }
    updateProgress();
}

void ReaderWindow::speakText(const QString &rawText) {
    const QString text = rawText.trimmed().left(12000);
    if (text.isEmpty()) return;
    if (voiceBox->currentData().toString().startsWith("piper:")) startPiper(text);
    else speech->say(text);
}

void ReaderWindow::setReadingCursorEnabled(bool enabled) {
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
              background: #55b879 !important; box-shadow: 0 0 0 2px #55b87933 !important;
              animation: leaf-reader-blink 1.1s step-end infinite !important;
            }
            @keyframes leaf-reader-blink { 50% { opacity: .25; } }`;
            (document.head || document.documentElement).appendChild(style);
          }
          if (!window.__leafCursorHandlerInstalled) {
            document.addEventListener('click', (event) => {
              if (!window.__leafCursorEnabled || event.button !== 0) return;
              if (event.target.closest('a, button, input, select, textarea')) return;
              const position = document.caretPositionFromPoint
                ? document.caretPositionFromPoint(event.clientX, event.clientY) : null;
              const legacy = !position && document.caretRangeFromPoint
                ? document.caretRangeFromPoint(event.clientX, event.clientY) : null;
              const node = position ? position.offsetNode : legacy ? legacy.startContainer : null;
              const offset = position ? position.offset : legacy ? legacy.startOffset : 0;
              if (!node || (node.nodeType !== Node.TEXT_NODE && node.nodeType !== Node.ELEMENT_NODE)) return;
              document.getElementById('__leaf_reader_cursor')?.remove();
              const marker = document.createElement('span');
              marker.id = '__leaf_reader_cursor';
              marker.setAttribute('aria-label', 'Reading starts here');
              const range = document.createRange();
              try {
                range.setStart(node, Math.min(offset, node.length ?? node.childNodes.length));
                range.collapse(true);
                range.insertNode(marker);
                marker.scrollIntoView({ block: 'center', behavior: 'smooth' });
              } catch (_) { marker.remove(); }
            }, true);
            window.__leafCursorHandlerInstalled = true;
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
        QTextDocument document;
        document.setHtml(chapters[currentChapter].html);
        speakText(document.toPlainText());
        return;
    }
    reader->page()->runJavaScript(
        "(() => { const selected = window.getSelection().toString().trim(); if (selected) return selected; "
        "const marker = document.getElementById('__leaf_reader_cursor'); "
        "if (marker && document.body) { const range = document.createRange(); range.setStartAfter(marker); "
        "range.setEndAfter(document.body.lastChild || document.body); return range.toString(); } "
        "return document.body ? document.body.innerText : ''; })()",
        [this](const QVariant &result) { speakText(result.toString()); });
}

bool ReaderWindow::piperIsActive() const {
    return piperProcess->state() != QProcess::NotRunning || audioProcess->state() != QProcess::NotRunning;
}

void ReaderWindow::stopSpeech() {
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
    QString bg, page, ink, muted, accent;
    if (themeBox->currentIndex() == 2) { bg="#171918"; page="#202321"; ink="#e7e3d8"; muted="#9da49d"; accent="#8fbf9f"; }
    else if (themeBox->currentIndex() == 1) { bg="#d9c9a7"; page="#efe2c2"; ink="#443828"; muted="#796b55"; accent="#8a5a32"; }
    else { bg="#deded9"; page="#faf9f5"; ink="#292c29"; muted="#747a74"; accent="#477a59"; }
    qApp->setStyleSheet(QString(R"(
        QMainWindow, QToolBar, QWidget { background: %1; color: %3; }
        QToolBar { border: none; spacing: 8px; padding: 8px; }
        QTextBrowser { background: %2; color: %3; border: none; padding: 42px; selection-background-color: %5; }
        QListWidget { background: transparent; border: none; outline: none; }
        QListWidget::item { padding: 9px 8px; border-radius: 5px; }
        QListWidget::item:selected { background: %5; color: white; }
        QPushButton, QComboBox { background: %2; border: 1px solid %4; border-radius: 6px; padding: 6px 10px; }
        QPushButton:hover, QComboBox:hover { border-color: %5; }
        QLabel#bookTitle { font-size: 18px; font-weight: 700; padding: 8px 4px; }
    )").arg(bg, page, ink, muted, accent));
    reader->setZoomFactor(fontSlider->value() / 20.0);
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

void ReaderWindow::closeEvent(QCloseEvent *event) {
    savePosition();
    stopSpeech();
    QSettings s; s.setValue("window/geometry", saveGeometry()); s.setValue("appearance/fontSize", fontSlider->value());
    s.setValue("appearance/theme", themeBox->currentIndex()); s.setValue("speech/rate", rateSlider->value());
    s.setValue("speech/readingCursor", cursorButton->isChecked());
    QMainWindow::closeEvent(event);
}

void ReaderWindow::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void ReaderWindow::dropEvent(QDropEvent *event) {
    if (!event->mimeData()->urls().isEmpty()) openBook(event->mimeData()->urls().first().toLocalFile());
}
