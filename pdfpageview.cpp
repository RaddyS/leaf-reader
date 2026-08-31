#include "pdfpageview.h"

#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <poppler-qt6.h>
#include <algorithm>
#include <limits>

PdfPageView::PdfPageView(QWidget *parent) : QWidget(parent) {
    setMinimumSize(320, 300);
    setAutoFillBackground(true);
    setCursor(Qt::IBeamCursor);
}

PdfPageView::~PdfPageView() = default;

bool PdfPageView::load(const QString &path) {
    document = Poppler::Document::load(path);
    if (!document || document->isLocked() || document->numPages() == 0) {
        document.reset();
        return false;
    }
    document->setRenderHint(Poppler::Document::Antialiasing, true);
    document->setRenderHint(Poppler::Document::TextAntialiasing, true);
    setPage(0);
    return true;
}

int PdfPageView::pageCount() const {
    return document ? document->numPages() : 0;
}

void PdfPageView::setPage(int number) {
    if (!document) return;
    currentPage = std::clamp(number, 0, document->numPages() - 1);
    page = document->page(currentPage);
    words = page ? page->textList() : std::vector<std::unique_ptr<Poppler::TextBox>>{};
    cursorWord = 0;
    renderPage();
}

void PdfPageView::setCursorEnabled(bool enabled) {
    cursorEnabled = enabled;
    update();
}

void PdfPageView::setZoomFactor(double factor) {
    zoomFactor = std::clamp(factor, 0.7, 1.7);
    renderPage();
}

void PdfPageView::setAccentColor(const QColor &color) {
    if (!color.isValid() || accentColor == color) return;
    accentColor = color;
    update();
}

QString PdfPageView::speechText() {
    playbackStartWord = cursorEnabled ? cursorWord : 0;
    QString result;
    for (int i = playbackStartWord; i < int(words.size()); ++i) {
        result += words[i]->text();
        if (words[i]->hasSpaceAfter()) result += ' ';
    }
    return result.trimmed();
}

void PdfPageView::setPlaybackWord(int offset) {
    if (words.empty()) return;
    cursorWord = std::clamp(playbackStartWord + offset, 0, int(words.size()) - 1);
    update();
}

QRectF PdfPageView::pageRectangle() const {
    if (pageImage.isNull()) return {};
    QSizeF size = pageImage.size();
    const double fit = std::min((width() - 24.0) / size.width(), (height() - 24.0) / size.height());
    size *= std::max(0.05, fit);
    return QRectF((width() - size.width()) / 2.0, (height() - size.height()) / 2.0, size.width(), size.height());
}

QPointF PdfPageView::toPagePoint(const QPointF &point) const {
    const QRectF target = pageRectangle();
    if (!page || !target.contains(point)) return {-1, -1};
    const QSizeF points = page->pageSizeF();
    return {(point.x() - target.left()) * points.width() / target.width(),
            (point.y() - target.top()) * points.height() / target.height()};
}

void PdfPageView::renderPage() {
    if (!page || width() < 10 || height() < 10) return;
    const QSizeF points = page->pageSizeF();
    const double fit = std::min((width() - 24.0) / points.width(), (height() - 24.0) / points.height());
    renderedScale = std::max(0.5, fit * zoomFactor);
    pageImage = page->renderToImage(72.0 * renderedScale, 72.0 * renderedScale);
    update();
}

void PdfPageView::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), palette().window());
    const QRectF target = pageRectangle();
    if (pageImage.isNull()) return;
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(target, pageImage);

    if (!cursorEnabled || cursorWord < 0 || cursorWord >= int(words.size()) || !page) return;
    const QRectF word = words[cursorWord]->boundingBox();
    const QSizeF points = page->pageSizeF();
    const double sx = target.width() / points.width();
    const double sy = target.height() / points.height();
    QRectF highlight(target.left() + word.left() * sx, target.top() + word.top() * sy,
                     std::max(3.0, word.width() * sx), std::max(8.0, word.height() * sy));
    QColor fill = accentColor;
    fill.setAlpha(65);
    painter.fillRect(highlight, fill);
    painter.setPen(QPen(accentColor, 3));
    painter.drawLine(highlight.topLeft(), highlight.bottomLeft());
}

void PdfPageView::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    renderPage();
}

void PdfPageView::mousePressEvent(QMouseEvent *event) {
    if (!cursorEnabled || event->button() != Qt::LeftButton || words.empty()) {
        QWidget::mousePressEvent(event);
        return;
    }
    const QPointF point = toPagePoint(event->position());
    if (point.x() < 0) return;
    int nearest = -1;
    double nearestDistance = std::numeric_limits<double>::max();
    for (int i = 0; i < int(words.size()); ++i) {
        const QRectF box = words[i]->boundingBox().adjusted(-2, -2, 2, 2);
        if (box.contains(point)) { nearest = i; break; }
        const QPointF delta = box.center() - point;
        const double distance = delta.x() * delta.x() + delta.y() * delta.y();
        if (distance < nearestDistance) { nearestDistance = distance; nearest = i; }
    }
    if (nearest >= 0) {
        cursorWord = nearest;
        update();
    }
}
