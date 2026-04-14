# ClaudeTray - Qt 시스템 트레이 Claude Code 사용량 모니터

## 목표 UI (Claude.ai 스타일)

```
트레이 아이콘 우클릭 팝업:

  ┌─────────────────────────────────┐
  │  Claude Code Usage              │
  ├─────────────────────────────────┤
  │  5시간 사용량                    │
  │  ████████████░░░░░░░  62%       │
  │  리셋까지: 1h 42m               │
  ├─────────────────────────────────┤
  │  7일 사용량                      │
  │  ███░░░░░░░░░░░░░░░░  18%       │
  │  리셋까지: 4d 12h               │
  ├─────────────────────────────────┤
  │  새로고침 (마지막: 14:32)        │
  │  종료                           │
  └─────────────────────────────────┘
```

트레이 아이콘 툴팁:
```
5h: 62%  |  7d: 18%
리셋: 1h 42m 후
```

트레이 아이콘 색상: 녹색(~50%) → 노란색(~75%) → 빨간색(90%+)

---

## 데이터 소스 전략

### 1순위: OAuth API (정확한 % 직접 제공)

Claude Code `/usage` 명령어가 내부적으로 사용하는 엔드포인트.
실제로 Anthropic 서버가 계산한 quota % 를 반환.

```
GET https://api.anthropic.com/api/oauth/usage
Authorization: Bearer <accessToken>
anthropic-beta: oauth-2025-04-20
```

응답 포맷:
```json
{
  "five_hour": {
    "utilization": 0.62,
    "resets_at": "2026-04-14T16:00:00Z"
  },
  "seven_day": {
    "utilization": 0.18,
    "resets_at": "2026-04-21T00:00:00Z"
  },
  "seven_day_sonnet": {
    "utilization": 0.35,
    "resets_at": "2026-04-21T00:00:00Z"
  }
}
```

인증 토큰 위치 (실제 확인):
```
~/.claude/.credentials.json
→ claudeAiOauth.accessToken    (Bearer 토큰)
→ claudeAiOauth.refreshToken   (만료 시 갱신용)
→ claudeAiOauth.expiresAt      (만료 시각 ms)
→ claudeAiOauth.subscriptionType  ("pro" / "max_5x" / "max_20x")
```

**한계**: 429 rate_limit_error가 매우 공격적 → 최소 5분 간격 필수
**대응**: 실패 시 2순위 로컬 계산으로 폴백

---

### 2순위: 로컬 JSONL 계산 (폴백)

API 실패 시 로컬 파일 직접 파싱으로 추정치 계산.

경로:
```
~/.claude/projects/**/*.jsonl
```

실제 확인된 레코드 구조:
```json
{
  "type": "assistant",
  "model": "claude-sonnet-4-6",
  "timestamp": "2026-04-14T03:44:06.590Z",
  "message": {
    "usage": {
      "input_tokens": 3,
      "output_tokens": 114,
      "cache_creation_input_tokens": 4962,
      "cache_read_input_tokens": 12098
    }
  }
}
```

계산 방식:
- 현재 시각 기준 최근 5시간 이내 레코드 합산
- subscriptionType으로 한도 추정 → % 계산
- 실제 이 대화 기준 5h 토큰: **1,870,831 tokens**

플랜별 커뮤니티 추정 한도:
| 플랜 | 5h 토큰 추정 한도 |
|------|-----------------|
| Pro  | ~7M tokens      |
| Max 5x | ~35M tokens  |
| Max 20x | ~140M tokens |

> 공식 미공개 수치 - 추정임을 UI에 표시

---

## 프로젝트 파일 구조

```
02_ClaudeTray/
├── CMakeLists.txt
├── main.cpp
├── usagedata.h                ← 공통 데이터 구조체
├── credentialsreader.h/.cpp   ← OAuth 토큰 읽기
├── usageapiclient.h/.cpp      ← OAuth API 호출 (QNetworkAccessManager)
├── usagescanner.h/.cpp        ← 로컬 JSONL 파싱 (폴백)
├── trayapp.h/.cpp             ← QSystemTrayIcon + QMenu
└── usagepopup.h/.cpp          ← 상세 팝업 위젯 (QProgressBar)
```

---

## 클래스 설계

### `usagedata.h` (구조체만)
```cpp
struct QuotaInfo {
    double    utilization = 0.0;  // 0.0 ~ 1.0
    QDateTime resetsAt;
    bool      valid = false;
};

struct UsageData {
    QuotaInfo fiveHour;
    QuotaInfo sevenDay;
    QuotaInfo sevenDaySonnet;
    QDateTime fetchedAt;
    bool      fromApi;     // true=API 정확값, false=로컬 추정
};
```

### `CredentialsReader`
```
역할: ~/.claude/.credentials.json 파싱

메서드:
  - static QString accessToken()
  - static QString subscriptionType()  // "pro", "max_5x", ...
  - static bool    isExpired()
  - static QString claudeDir()         // 플랫폼별 경로 반환
```

### `UsageApiClient` (QObject)
```
역할: OAuth API 비동기 호출 + 5분 자동 폴링

멤버:
  - QNetworkAccessManager *nam
  - QTimer *pollTimer          // 5분(300s) 간격

시그널:
  - usageFetched(UsageData)
  - fetchFailed(QString reason)

슬롯:
  - fetchUsage()               // 수동 + 타이머 트리거
  - onReplyFinished(QNetworkReply*)
```

### `UsageScanner` (QObject)
```
역할: 로컬 JSONL로 사용량 추정 (폴백)

멤버:
  - QFileSystemWatcher *watcher
  - QMap<QString, qint64> fileOffsets  // 파일별 읽기 오프셋

메서드:
  - UsageData calcFromLocal()
  - void      watchProjects()

시그널:
  - localUsageUpdated(UsageData)
```

### `TrayApp` (QObject)
```
역할: 앱 진입점 + UI 총괄

흐름:
  1. CredentialsReader로 토큰 확인
  2. UsageApiClient 시작 (5분 폴링)
  3. usageFetched → 메뉴 갱신 (정확값)
  4. fetchFailed  → UsageScanner 폴백 → 메뉴 갱신 (추정값)
  5. QTimer 1분마다 리셋 카운트다운 텍스트 갱신

메뉴 구성:
  - QWidgetAction으로 QProgressBar 삽입
  - 색상: utilization 기준 녹/황/적 자동 변경
```

---

## UI 구현 핵심: QMenu에 QProgressBar 넣기

```cpp
// QWidgetAction으로 커스텀 위젯을 메뉴에 삽입
QWidget     *container = new QWidget;
QVBoxLayout *layout    = new QVBoxLayout(container);

QLabel      *label = new QLabel("5시간 사용량  62%");
QProgressBar *bar  = new QProgressBar;
bar->setRange(0, 100);
bar->setValue(62);
bar->setStyleSheet("QProgressBar::chunk { background: #f0a500; }");

QLabel *reset = new QLabel("리셋까지: 1h 42m");

layout->addWidget(label);
layout->addWidget(bar);
layout->addWidget(reset);

QWidgetAction *action = new QWidgetAction(menu);
action->setDefaultWidget(container);
menu->addAction(action);
```

---

## 구현 단계

### Phase 1 - 인증 토큰 읽기
- [ ] `CredentialsReader` 구현
- [ ] `%USERPROFILE%/.claude/.credentials.json` 파싱
- [ ] subscriptionType, accessToken, expiresAt 추출

### Phase 2 - OAuth API 클라이언트
- [ ] `QNetworkAccessManager` 비동기 GET
- [ ] 헤더 설정 (`Authorization: Bearer`, `anthropic-beta: oauth-2025-04-20`)
- [ ] JSON 응답 파싱 → `UsageData`
- [ ] 5분 간격 `QTimer` 자동 폴링
- [ ] 429 실패 시 `fetchFailed` emit

### Phase 3 - 로컬 JSONL 폴백
- [ ] `QDirIterator`로 전체 JSONL 스캔
- [ ] 5시간 윈도우 토큰 합산
- [ ] subscriptionType별 한도 → % 계산
- [ ] `QFileSystemWatcher` 변경 감지

### Phase 4 - 트레이 UI
- [ ] `QSystemTrayIcon` + 아이콘 색상 로직
- [ ] `QWidgetAction` + `QProgressBar` 메뉴
- [ ] 1분마다 카운트다운 갱신
- [ ] 툴팁: "5h: 62% | 7d: 18%"

### Phase 5 - 상세 팝업 (선택)
- [ ] 별도 `QWidget` 팝업창
- [ ] 3개 `QProgressBar` (5h / 7d / 7d-sonnet)
- [ ] API/추정 구분 레이블

---

## CMakeLists.txt

```cmake
find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

target_link_libraries(ClaudeTray PRIVATE
    Qt6::Widgets   # QSystemTrayIcon, QMenu, QProgressBar
    Qt6::Network   # QNetworkAccessManager (OAuth API 호출)
)
```

---

## 주의사항

| 항목 | 내용 |
|------|------|
| OAuth 토큰 보안 | UI/로그에 토큰 절대 노출 금지 |
| 폴링 간격 | 최소 5분. 429 연속 시 backoff 적용 |
| 추정값 표시 | 로컬 계산 시 "~추정" 문구 표시 |
| Windows 경로 | `QDir::homePath() + "/.claude"` 사용 |
| 재귀 감시 | `QFileSystemWatcher`는 재귀 불가 → 새 프로젝트 폴더 생성 시 수동 추가 |
