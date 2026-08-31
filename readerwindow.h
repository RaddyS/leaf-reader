#pragma once

#include <QMainWindow>
#include <QElapsedTimer>
#include <QTextToSpeech>
#include <QVector>

class QLabel;
class QListWidget;
class QComboBox;
class QPushButton;
class QSlider;
class QWebEngineView;
class QStackedWidget;
class QTemporaryDir;
class QProcess;
class PdfPageView;

struct Chapter { QString title; QString html; QString source; };

class ReaderWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit ReaderWindow(QWidget *parent = nullptr);
    ~ReaderWindow() override;
    void openBook(const QString &path);

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void chooseBook();
    void showChapter(int index);
    void toggleSpeech();
    void speechStateChanged(QTextToSpeech::State state);
    void applyAppearance();
    void updateProgress();

private:
    bool loadEpub(const QString &path);
    bool loadPdf(const QString &path);
    bool loadText(const QString &path);
    void populateBook(const QString &title);
    void savePosition();
    void restorePosition();
    void setError(const QString &message);
    QString bookKey() const;
    void refreshVoices();
    void startPiper(const QString &text);
    void stopSpeech();
    bool piperIsActive() const;
    void speakText(const QString &text);
    void setReadingCursorEnabled(bool enabled);
    void updateSpeechCursor();
    void clearSpeechHighlight();

    QWebEngineView *reader;
    PdfPageView *pdfReader;
    QStackedWidget *contentStack;
    QListWidget *chaptersList;
    QLabel *bookTitle;
    QLabel *progressLabel;
    QPushButton *speakButton;
    QPushButton *cursorButton;
    QSlider *fontSlider;
    QSlider *rateSlider;
    QComboBox *themeBox;
    QComboBox *voiceBox;
    QTextToSpeech *speech;
    QProcess *piperProcess;
    QProcess *audioProcess;
    QTimer *speechCursorTimer;
    QElapsedTimer speechClock;
    QVector<double> speechWordWeights;
    qint64 speechDurationMs = 0;
    bool speechIsPdf = false;
    QString activeSpeechText;
    std::unique_ptr<QTemporaryDir> speechTemp;
    QVector<Chapter> chapters;
    QString currentPath;
    int currentChapter = 0;
    std::unique_ptr<QTemporaryDir> epubTemp;
};
