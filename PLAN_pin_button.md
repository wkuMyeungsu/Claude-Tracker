# Plan: LED → Pin 토글 버튼 교체

## 목표
타이틀바 좌측의 `StatusLed` 위젯을 제거하고,
동일한 자리에 **항상-위 고정(WindowStaysOnTopHint) 토글 버튼**을 배치한다.

## 결과 UI

```
[📌] Claude Code Usage          [−][x]   ← 핀 ON (강조색)
[📌] Claude Code Usage          [−][x]   ← 핀 OFF (회색)
```

---

## 변경 파일

| 파일 | 작업 |
|---|---|
| `statusled.h` | 삭제 |
| `statusled.cpp` | 삭제 |
| `CMakeLists.txt` | `statusled.cpp` 소스 목록에서 제거 |
| `usagepopup.h` | `StatusLed *m_led` → `QPushButton *m_pinBtn`, `#include "statusled.h"` 제거 |
| `usagepopup.cpp` | 핀 버튼 생성·배치, LED 호출 제거, 핀 토글 로직 추가 |

---

## 세부 구현

### 1. `usagepopup.h`

```cpp
// 제거
#include "statusled.h"
class StatusLed;
StatusLed *m_led = nullptr;

// 추가
class QPushButton;
QPushButton *m_pinBtn = nullptr;
```

### 2. `usagepopup.cpp` — 생성자

```cpp
// 제거
m_led = new StatusLed;
titleRow->addWidget(m_led);
titleRow->addSpacing(6);

// 추가
m_pinBtn = new QPushButton("📌");
m_pinBtn->setFixedSize(22, 22);
m_pinBtn->setCheckable(true);
m_pinBtn->setChecked(true);   // 초기값: 핀 ON (현재 WindowStaysOnTopHint 기본값과 동일)
m_pinBtn->setCursor(Qt::PointingHandCursor);
m_pinBtn->setToolTip("항상 위에 표시");
// 스타일: checked = 강조색(오렌지), unchecked = 투명/회색
m_pinBtn->setStyleSheet(R"(
    QPushButton {
        background: transparent;
        color: #888;
        border: none;
        font-size: 12px;
        border-radius: 3px;
    }
    QPushButton:checked {
        color: #f39c12;
    }
    QPushButton:hover {
        background: #555;
    }
)");

connect(m_pinBtn, &QPushButton::toggled, this, [this](bool pinned) {
    Qt::WindowFlags flags = windowFlags();
    if (pinned)
        flags |= Qt::WindowStaysOnTopHint;
    else
        flags &= ~Qt::WindowStaysOnTopHint;
    setWindowFlags(flags);
    show();   // setWindowFlags() 가 창을 숨기므로 반드시 재호출
    raise();
});

titleRow->addWidget(m_pinBtn);
titleRow->addSpacing(6);
```

### 3. `usagepopup.cpp` — `setActive()`

```cpp
void UsagePopup::setActive()
{
    m_idleMode      = false;
    m_opacityAtIdle = false;
    // m_led->setActive();  ← 제거
    m_activityPill->show();
    if (windowOpacity() >= 1.0 && m_opacityAnim->endValue().toDouble() >= 1.0)
        return;
    animateOpacityTo(1.0);
}
```

### 4. `usagepopup.cpp` — `setIdle()`

```cpp
void UsagePopup::setIdle()
{
    if (m_idleMode) return;
    m_idleMode = true;
    m_activityPill->hide();
    // m_led->setIdle();  ← 제거
    // 10초 후 투명화 로직은 그대로 유지
    QTimer::singleShot(10'000, this, [this]() {
        if (!m_idleMode) return;
        m_opacityAtIdle = true;
        if (isVisible())
            animateOpacityTo(0.6);
    });
}
```

### 5. `CMakeLists.txt`

```cmake
# 제거
statusled.cpp
statusled.h
```

---

## 주의사항

- `setWindowFlags()` 호출 시 Qt가 창을 내부적으로 재생성하여 **자동으로 숨김** → `show()` + `raise()` 필수
- 초기 `setChecked(true)` 는 생성자의 `Qt::WindowStaysOnTopHint` 플래그와 일치해야 함
- `TrayApp` 및 다른 파일은 `StatusLed` / `m_led` 를 직접 참조하지 않으므로 **수정 불필요**
- `setActive()` / `setIdle()` 시그니처 변경 없음 → `trayapp.cpp` 호출부 수정 불필요
- opacity 페이드, activity pill, 드래그 억제 로직은 **그대로 유지**
