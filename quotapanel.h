#ifndef QUOTAPANEL_H
#define QUOTAPANEL_H

#include <QWidget>
#include "usagedata.h"

class QLabel;
class QPropertyAnimation;

class ThresholdBar : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(float shimmerPos   READ shimmerPos   WRITE setShimmerPos)
    Q_PROPERTY(float shimmerAlpha READ shimmerAlpha WRITE setShimmerAlpha)
public:
    explicit ThresholdBar(QWidget *parent = nullptr);
    void setValue(int pct);
    void setActive(bool active);

    float shimmerPos()   const     { return m_shimmerPos; }
    void  setShimmerPos(float v)   { m_shimmerPos = v; update(); }
    float shimmerAlpha() const     { return m_shimmerAlpha; }
    void  setShimmerAlpha(float v) { m_shimmerAlpha = v; update(); }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    int                 m_value       = 0;
    bool                m_active      = false;
    float               m_shimmerPos  = 0.0f;
    float               m_shimmerAlpha = 1.0f;
    QPropertyAnimation *m_shimmerAnim = nullptr;
    QPropertyAnimation *m_fadeAnim    = nullptr;
};

class QuotaPanel : public QWidget
{
    Q_OBJECT
public:
    explicit QuotaPanel(const QString &title, QWidget *parent = nullptr);

    void setData(const QuotaInfo &info);
    void setCountdown(const QString &text);
    void setCompact(bool compact);
    void setActive(bool active);

private:
    QLabel       *m_titleLabel;
    QLabel       *m_pctLabel;
    ThresholdBar *m_bar;
    QLabel       *m_resetLabel;
};

#endif // QUOTAPANEL_H
