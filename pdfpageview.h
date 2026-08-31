#pragma once

#include <QImage>
#include <QWidget>
#include <memory>
#include <vector>

namespace Poppler { class Document; class Page; class TextBox; }

class PdfPageView : public QWidget {
    Q_OBJECT
public:
    explicit PdfPageView(QWidget *parent = nullptr);
    ~PdfPageView() override;

    bool load(const QString &path);
    int pageCount() const;
    void setPage(int page);
    void setCursorEnabled(bool enabled);
    void setZoomFactor(double factor);
    QString speechText() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    void renderPage();
    QRectF pageRectangle() const;
    QPointF toPagePoint(const QPointF &widgetPoint) const;

    std::unique_ptr<Poppler::Document> document;
    std::unique_ptr<Poppler::Page> page;
    std::vector<std::unique_ptr<Poppler::TextBox>> words;
    QImage pageImage;
    int currentPage = 0;
    int cursorWord = 0;
    bool cursorEnabled = true;
    double zoomFactor = 1.0;
    double renderedScale = 1.0;
};
