#ifndef QUOTAGAUGE_H
#define QUOTAGAUGE_H

#include <QColor>
#include <QWidget>
#include "UsageTypes.h"

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
    void setDisableShimmer(bool disable);
    // 임계 팔레트(초록/주황/빨강) 대신 고정 색을 쓴다. 크레딧 게이지처럼
    // "얼마나 위험한가"가 아니라 "얼마나 썼는가"를 보여주는 막대에 쓴다.
    void setAccentColor(const QColor &color);

    float shimmerPos()   const     { return m_shimmerPos; }
    void  setShimmerPos(float v)   { m_shimmerPos = v; update(); }
    float shimmerAlpha() const     { return m_shimmerAlpha; }
    void  setShimmerAlpha(float v) { m_shimmerAlpha = v; update(); }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    int                 m_value          = 0;
    QColor              m_accent;        // 유효하면 임계 색 대신 이걸 쓴다
    bool                m_active         = false;
    bool                m_disableShimmer = false;
    float               m_shimmerPos     = 0.0f;
    float               m_shimmerAlpha   = 1.0f;
    QPropertyAnimation *m_shimmerAnim    = nullptr;
    QPropertyAnimation *m_fadeAnim       = nullptr;
};

class QuotaGauge : public QWidget
{
    Q_OBJECT
public:
    explicit QuotaGauge(const QString &title, QWidget *parent = nullptr);

    void setData(const QuotaInfo &info);
    void setCountdown(const QString &text);
    void setActive(bool active);
    void setDisableShimmer(bool disable);

private:
    QLabel       *m_pctLabel;
    ThresholdBar *m_bar;
    QLabel       *m_resetLabel;
};

#endif // QUOTAGAUGE_H
