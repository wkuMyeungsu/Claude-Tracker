#ifndef TOGGLESWITCH_H
#define TOGGLESWITCH_H

#include <QPropertyAnimation>
#include <QWidget>

class ToggleSwitch : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(qreal knobPos READ knobPos WRITE setKnobPos)

public:
    explicit ToggleSwitch(QWidget *parent = nullptr);

    bool     isChecked() const { return m_checked; }
    void     setChecked(bool checked);
    QSize    sizeHint() const override { return {38, 22}; }

signals:
    void toggled(bool checked);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    qreal knobPos() const        { return m_knobPos; }
    void  setKnobPos(qreal pos)  { m_knobPos = pos; update(); }

    bool                 m_checked = false;
    qreal                m_knobPos = 0.0;   // 0.0 = off(left), 1.0 = on(right)
    QPropertyAnimation  *m_anim;
};

#endif // TOGGLESWITCH_H
