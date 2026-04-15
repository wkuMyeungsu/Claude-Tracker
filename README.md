# ClaudeTray

Windows 시스템 트레이에서 Claude Code 사용량(quota)을 실시간으로 모니터링하는 Qt6 앱.

![badge](https://img.shields.io/badge/Qt-6.x-green) ![badge](https://img.shields.io/badge/platform-Windows-blue) ![badge](https://img.shields.io/badge/language-C%2B%2B17-orange)

---

## 스크린샷

트레이 아이콘 클릭 시 팝업 (프레임리스 드래그 가능):

```
┌─────────────────────────────────┐
│ ●  Claude Code Usage        [x] │  ← iOS 스타일 상태 LED
├─────────────────────────────────┤
│  5시간 사용량              62%  │
│  ████████████░░░░░░░            │
│  리셋까지 1h 42m  (15:42 리셋)  │
├─────────────────────────────────┤
│  7일 사용량                18%  │
│  ███░░░░░░░░░░░░░░░░            │
│  리셋까지 4d 12h  (4/20 월 09:00 리셋) │
├─────────────────────────────────┤
│  API 기준값                     │
│  마지막 API 성공: 14:32:01      │
│  다음 자동 갱신: 14:37:01  [종료]│
└─────────────────────────────────┘
```

트레이 아이콘 색상: 녹색(~50%) → 노란색(~80%) → 빨간색(80%+)

### 상태 LED (타이틀바 좌측)

| 상태 | 색상 | 동작 |
|------|------|------|
| 토큰 생성 중 | 🟢 iOS 그린 | breathing 펄스 (~2초 주기) |
| 작업 종료 후 | 🟢→⚫ | 30초에 걸쳐 회색으로 페이드 |
| 장시간 대기 | ⚫ iOS 회색 | 정적 |

시간 감쇠 방식으로 "얼마나 최근에 작업했는지"를 자연스럽게 표현합니다.

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

API 호출 실패(429 등) 시 로컬 파일을 직접 파싱해 사용량을 추정합니다.

- **경로**: `~/.claude/projects/**/*.jsonl`
- **반영 범위**: Claude Code CLI 사용량만 (웹 채팅 미포함)
- **실시간 감지**: `QFileSystemWatcher`로 파일 변경 즉시 반영
- **새 폴더 감지**: 5분마다 자동으로 watch 목록 갱신 (재귀 감시 불가 우회)
- **중복 제거**: `uuid` 필드로 동일 레코드 중복 집계 방지
- **표시**: 추정값임을 UI 상태 레이블에 명시

### 성능 최적화

- **파일 변경 디바운스**: JSONL write 이벤트를 2초간 묶어 스캔 빈도 억제
- **백그라운드 스캔**: `QtConcurrent::run()`으로 파일 I/O를 메인 스레드 분리
- **드래그 중 UI 억제**: 드래그 시 UI 업데이트를 pending으로 저장, 종료 시 일괄 반영

### 병합 전략

API 성공 이력이 있는 경우, 마지막 API 응답 이후의 로컬 증분 토큰을 더해 더 정확한 추정값을 계산합니다.

```
표시값 = API 마지막 기준값 + 그 이후 로컬 증분
```

상태 표시:
- `API 기준값` — 최신 API 응답
- `API 기준 + 로컬 증분 추정` — API 성공 이력 있고 현재 폴백 중
- `로컬 추정값` — API 성공 이력 없음

---

## 요구사항

- Qt 6.x (Widgets, Network 모듈)
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
├── usagedata.h             ← QuotaInfo / UsageData 공통 구조체
├── credentialsreader.h/cpp ← OAuth 토큰·플랜 타입 읽기
├── usageapiclient.h/cpp    ← OAuth API 비동기 폴링 (Qt::Network)
├── usagescanner.h/cpp      ← 로컬 JSONL 파싱 + 파일 감시
├── statusled.h/cpp         ← iOS 스타일 상태 LED 위젯
├── quotapanel.h/cpp        ← QProgressBar 사용량 패널 위젯
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
