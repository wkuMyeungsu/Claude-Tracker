# ClaudeTray

Windows 시스템 트레이에서 Claude Code 사용량(quota)을 실시간으로 모니터링하는 Qt6 앱.

![badge](https://img.shields.io/badge/Qt-6.x-green) ![badge](https://img.shields.io/badge/platform-Windows-blue) ![badge](https://img.shields.io/badge/language-C%2B%2B17-orange)

---

## 스크린샷

트레이 아이콘 클릭 시 팝업 (프레임리스 드래그 가능):

```
┌─────────────────────────────────────┐
│ ● Claude Code Usage             [−] │  ← ●: 핀 버튼 (주황=고정, 회색=해제)
├╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌┤  ← 토큰 사용 중일 때 3px 활성 라인
├─────────────────────────────────────┤
│  5h 사용량                     62%  │
│  ████████████░░░│░░░│░░░░          │  ← 임계선: 주황(71%), 빨강(86%)
│  1h 26m 후 초기화 (18:00)          │
├─────────────────────────────────────┤
│  7d 사용량                     18%  │
│  ███░░░░░░░░░░░░│░░░│░░░░          │
│  2d 3h 후 초기화 (4/17 목 18:00)   │
├─────────────────────────────────────┤
│  🟢 18:35 갱신 예정            ⚙   │  ← 다음 갱신 예정 시각 표시
└─────────────────────────────────────┘
```

설정 패널 (⚙ 클릭 시 확장):

```
├─────────────────────────────────────┤
│  핀 고정 시 자동 투명화    [●──]   │  ← 모바일 스타일 토글 버튼
└─────────────────────────────────────┘
```

---

## UI 구성

### 핀 버튼 (타이틀바 좌측)

| 상태 | 색상 |
|------|------|
| 고정 ON | 주황 동그라미 |
| 고정 OFF | 회색 동그라미 |

- **고정 ON**: 창이 항상 위에 표시되고, idle 10초 후 투명도 0.6 적용 (자동 투명화 옵션 ON 시)
- **고정 OFF**: 투명도 효과 없음, 창이 일반 Z-order로 전환되며 작업표시줄 기준으로 위치 자동 보정

### 자동 투명화 설정

⚙ 버튼으로 설정 패널을 열면 **모바일 스타일 토글 버튼**으로 ON/OFF 조작 가능.

- **ON**: 핀 고정 + idle 10초 → 팝업 투명도 0.6으로 fade
- **OFF**: 즉시 불투명으로 복원, 이후 idle 상태에서도 투명화 없음
- 토글 변경 시 현재 idle 상태에 즉시 반영

### 활성 라인 (타이틀바 하단)

토큰 사용이 감지되면 타이틀바 하단에 **3px 가로 라인**이 fade in, 사용 종료 2초 후 fade out.

라인 색상은 사용량에 따라 자동 변경:

| 사용량 | 색상 |
|--------|------|
| 0 ~ 70% | 초록 |
| 71 ~ 85% | 주황 |
| 86% 이상 | 빨강 |

색상 기준: `7d% >= 5h%`이면 5h 기준, `7d% < 5h%`이면 7d 기준으로 선택.

### 진행 바

- 두께: 12px
- 색상 코딩: 초록(0~70%) → 주황(71~85%) → 빨강(86~100%)
- 임계 실선: 71% / 86% 위치에 희미한 세로선 표시

### 트레이 아이콘

- Claude 방사형 디자인 (44×44 @ 2x DPR, 고해상도 지원)
- 우측 하단 점: 사용량 기반 색상 (초록/주황/빨강)

### 카운트다운 포맷

```
86m 후 초기화 (18:00)            ← 1시간 미만
1h 26m 후 초기화 (18:00)         ← 1시간 이상
2d 3h 후 초기화 (4/17 목 18:00)  ← 1일 이상
```

### 창 조작

| 동작 | 결과 |
|------|------|
| 타이틀바 드래그 | 창 이동 |
| `[−]` 버튼 클릭 | 현재 위치 저장 후 숨김 |
| 트레이 아이콘 클릭 | 현재 위치 저장 후 숨김 / 마지막 위치로 복원 |
| 트레이 아이콘 우클릭 | 종료 메뉴 |

---

## 동작 방식

### 1순위: OAuth API (정확한 quota %)

Claude Code가 내부적으로 사용하는 Anthropic 엔드포인트를 통해 서버 측 quota를 직접 조회합니다.

- **엔드포인트**: `GET https://api.anthropic.com/api/oauth/usage`
- **헤더**: `Authorization: Bearer <token>`, `anthropic-beta: oauth-2025-04-20`
- **폴링 간격**: 5분 (앱 시작 0.5초 후 첫 호출)
- **반영 범위**: Claude Code CLI + 웹 채팅 모두 포함
- **인증**: `~/.claude/.credentials.json`의 OAuth 토큰 자동 사용

### 2순위: 로컬 JSONL 폴백 (API 실패 시)

API 호출 실패 시 로컬 파일을 직접 파싱해 사용량을 추정합니다.

- **경로**: `~/.claude/projects/**/*.jsonl`
- **반영 범위**: Claude Code CLI 사용량만 (웹 채팅 미포함)
- **실시간 감지**: `QFileSystemWatcher`로 파일 변경 즉시 반영
- **초기 스캔**: 앱 시작 200ms 후 기존 JSONL 즉시 파싱 (파일 변경 없이도 초기값 표시)
- **새 폴더 감지**: 5분마다 자동으로 watch 목록 갱신 + 폴백 스캔 (Windows watcher 누락 대응)
- **중복 제거**: `requestId` 우선, 없으면 `uuid` 기준 (thinking/tool_use 블록 이중 집계 방지)
- **표시**: 팝업 하단에 `🟡 로컬 추적중` 표시

### 갱신 상태 표시

팝업 하단 한 줄로 현재 데이터 출처와 다음 갱신 예정 시각을 표시합니다.

| 표시 | 의미 |
|------|------|
| 🔄 갱신 중... | API 요청 진행 중 |
| 🟢 방금 갱신 | API 성공 후 1분 이내 |
| 🟢 18:35 갱신 예정 | API 성공, 1분 경과 — 다음 예정 시각 표시 |
| 🟡 로컬 추적중 | API 오류, 로컬 JSONL 폴백 사용 중 |
| 🔴 연결 오류 | 네트워크 연결 불가 |

### 성능 최적화

- **파일 변경 디바운스**: JSONL write 이벤트를 300ms간 묶어 스캔 빈도 억제
- **백그라운드 스캔**: `QtConcurrent::run()`으로 파일 I/O를 메인 스레드 분리
- **드래그 중 UI 억제**: 드래그 시 UI 업데이트를 pending으로 저장, 종료 시 일괄 반영
- **투명화 타이머 관리**: `QTimer` 멤버로 idle→active 전환 시 stale 타이머 즉시 취소

### 활성 상태 감지 흐름

```
JSONL 파일 write 감지 (QFileSystemWatcher)
  → UsageScanner::activityDetected() 즉시 emit
  → TrayApp::onActivityDetected()
      → UsagePopup::setActive()
          → 투명화 대기 타이머(m_fadeTimer) 취소
          → 타이틀바 하단 활성 라인 fade in (300ms)
          → 팝업 투명도 1.0

activityStopped emit (300ms 디바운스 후)
  → UsagePopup::setIdle()
      → m_fadeTimer 시작 (10초)
      → 2초 후 활성 라인 fade out (600ms)
      → 10초 후 팝업 투명도 0.6 (핀 고정 + 자동 투명화 ON 시에만)
```

### 병합 전략

API 성공 이력이 있는 경우, 마지막 API 응답 이후의 로컬 증분 토큰을 더해 더 정확한 추정값을 계산합니다.

```
표시값 = API 마지막 기준값 + 그 이후 로컬 증분
```

---

## 경고 임계값

`usagedata.h`에 공통 상수로 정의되어 진행 바, 트레이 아이콘, 활성 라인 모두 동일하게 참조합니다.

```cpp
#define USAGE_WARN_PCT 71   // 71% 이상 → 주황
#define USAGE_CRIT_PCT 86   // 86% 이상 → 빨강
```

---

## 요구사항

- Qt 6.x (Widgets, Network, Concurrent 모듈)
- CMake 3.5+
- C++17 컴파일러 (MinGW 64-bit 권장)
- Windows 10/11
- Claude Code 설치 및 로그인 상태 (`~/.claude/.credentials.json` 존재)

---

## 빌드

```bash
cmake -B build -S .
cmake --build build --config Debug
```

Qt Creator에서 `CMakeLists.txt`를 열어 빌드해도 됩니다.

---

## 파일 구조

```
ClaudeTray/
├── CMakeLists.txt
├── main.cpp                ← 진입점, 트레이 가용성 체크
├── usagedata.h             ← QuotaInfo / UsageData 구조체 + 경고 임계값 상수
├── credentialsreader.h/cpp ← OAuth 토큰·플랜 타입 읽기
├── usageapiclient.h/cpp    ← OAuth API 비동기 폴링 (Qt::Network)
├── usagescanner.h/cpp      ← 로컬 JSONL 파싱 + 파일 감시
├── quotapanel.h/cpp        ← ThresholdBar + 사용량 패널 위젯
├── toggleswitch.h/cpp      ← 모바일 스타일 토글 버튼 위젯
├── trayapp.h/cpp           ← 메인 오케스트레이터
└── usagepopup.h/cpp        ← 프레임리스 드래그 팝업 창
```

### 주요 구조체 (`usagedata.h`)

```cpp
struct QuotaInfo {
    double    utilization  = 0.0;  // 0.0 ~ 1.0
    QDateTime resetsAt;
    qint64    rawTokens   = 0;     // 실제 토큰 수 (표시용)
    qint64    limitTokens = 0;     // 플랜 한도 (표시용)
    bool      valid       = false;
};

struct UsageData {
    QuotaInfo fiveHour;
    QuotaInfo sevenDay;
    QuotaInfo sevenDaySonnet;
    QDateTime fetchedAt;
    bool      fromApi = false;   // true=API 정확값, false=로컬 추정
};
```

---

## 주의사항

| 항목 | 내용 |
|------|------|
| OAuth 토큰 | 로그/UI에 절대 노출되지 않음 |
| 폴링 간격 | API rate limit 대응을 위해 최소 5분 고정 |
| 웹 채팅 사용량 | API 실패(폴백 모드) 시 로컬 추정값에 포함되지 않음 |
| 재귀 감시 | `QFileSystemWatcher` Windows 재귀 불가 → 5분마다 자동으로 새 폴더 추가 |
| 플랜 한도 | 로컬 추정 한도는 커뮤니티 추정치 (공식 미공개) |

---

## 플랜별 추정 한도 (로컬 폴백용)

| 플랜 | 5시간 한도 | 7일 한도 |
|------|-----------|---------|
| Pro | 18M tokens | 144M tokens |
| Max 5x | 90M tokens | 720M tokens |
| Max 20x | 360M tokens | 2,880M tokens |

> 공식 미공개 수치입니다. API 연결 시에는 Anthropic 서버의 정확한 값을 사용합니다.

---

## 라이선스

MIT
