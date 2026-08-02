# ClaudeTray

Windows 시스템 트레이에서 Claude Code의 사용량(quota)과 추가 결제 크레딧을 실시간으로 추적하는 Qt6 기반 모니터링 프로그램입니다.

[![Download Windows](https://img.shields.io/badge/Download-Windows_v1.1.0-0078D6?style=for-the-badge&logo=windows&logoColor=white)](https://github.com/wkuMyeungsu/Claude-Tracker/releases/tag/v1.1.0)
![badge](https://img.shields.io/badge/version-1.1.0-blue) ![badge](https://img.shields.io/badge/Qt-6.x-green) ![badge](https://img.shields.io/badge/platform-Windows-blue) ![badge](https://img.shields.io/badge/language-C%2B%2B17-orange)

---

## 주요 기능

- **실시간 사용량 모니터링**: 5시간 및 7일 토큰 사용률을 실시간 게이지바로 시각화하고 초기화(리셋)까지 남은 시간을 카운트다운으로 보여줍니다.
- **추가 결제 크레딧 추적**: 기본 제공량을 모두 소모한 후 사용하는 추가 결제용 크레딧($)의 남은 한도와 사용액을 표시합니다.
- **오프라인 로컬 추적**: API 응답 지연이나 네트워크 오프라인 상태에서도 로컬에 저장되는 대화 로그 파일들을 분석하여 소모 비용(Delta)을 실시간으로 계산해 병합합니다.
- **최근 사용 모델 표시**: 대시보드 창 하단에 가장 최근에 대화를 나눈 모델 식별자(예: `sonnet-5`)를 감지하여 표시하며, 마우스 호버 시 풀네임을 제공합니다.
- **스마트 투명도 전환**: 팝업창을 화면에 항상 위에 고정(Pin)해 둔 경우, Claude 대화 활동이 10초간 없으면 방해되지 않도록 창이 자동으로 반투명하게 전환됩니다. (키보드·마우스 입력이 아니라 대화 로그 파일의 변경 여부를 기준으로 합니다.)
- **직관적인 한도 경고**: 안전(초록), 경고(주황), 임계(빨강) 등 한도 상태에 맞춰 트레이 아이콘과 게이지바 색상이 변경됩니다.

---

## 화면 구성 예시

```
┌─────────────────────────────────────┐
│ (P) Claude Code Usage           [−] │
├─────────────────────────────────────┤
│ 5h 사용량                       62% │
│ [██████████████░░░░░░░░░░░░░░░]     │
│ 1h 26m 후 초기화 (18:00)            │
├─────────────────────────────────────┤
│ 7d 사용량                       18% │
│ [████░░░░░░░░░░░░░░░░░░░░░░░░░]     │
│ 2d 3h 후 초기화 (4/17 목 18:00)     │
├─────────────────────────────────────┤
│ 추가 결제 크레딧                47% │
│ [█████████████░░░░░░░░░░░░░░░░]     │
│ $9.51 / $20.00                      │
├─────────────────────────────────────┤
│ 🟢 18:35 갱신 예정     sonnet-5  ⚙ │
└─────────────────────────────────────┘
```

- **`(P)` (Pin 버튼)**: 클릭 시 밝은 색(고정), 회색(해제)으로 변하며 항상 위에 팝업을 고정합니다.
- **`[−]` (숨기기 버튼)**: 클릭 시 현재 대시보드 창을 트레이 아이콘으로 숨깁니다.
- **게이지바**: 사용률에 따라 초록(~70%) → 주황(71~85%) → 빨강(86%~)으로 색이 바뀝니다.
  대화가 진행 중일 때는 채워진 영역 위로 빛이 흐르는 shimmer 애니메이션이 나타나 사용 중임을 표시합니다.
- **상태줄**: API 갱신 상태를 이모지로 나타냅니다. 🔄 갱신 중 / 🟢 정상(다음 갱신 예정 시각) / 🟡 로컬 추적중 / 🔴 연결 오류.
- **`⚙` (설정 버튼)**: 클릭 시 하단으로 설정 패널이 확장됩니다.
- **추가 결제 크레딧 패널**: 추가 결제가 활성화된 계정에서만 표시됩니다.

---

## 설치 (v1.1.0)

[릴리즈 페이지](https://github.com/wkuMyeungsu/Claude-Tracker/releases/tag/v1.1.0)에서 받으세요.

| 파일 | 용도 |
| --- | --- |
| `claude-tracker-1.1.0-win64.msi` | 설치 프로그램 (권장) |
| `claude-tracker-1.1.0-win64.zip` | 무설치 휴대용. 압축 해제 후 `bin\ClaudeTray.exe` 실행 |

**Qt를 따로 설치할 필요가 없습니다.** 실행에 필요한 Qt 런타임과 플러그인이 모두 포함되어 있습니다.

> 트레이 아이콘이 보이지 않으면 작업 표시줄의 `^`(숨겨진 아이콘 표시)를 펼쳐 주세요.
> Windows는 처음 실행하는 앱의 트레이 아이콘을 기본으로 숨김 영역에 넣습니다.

---

## 실행 요구사항

- Windows 10 / 11 (64비트)
- Claude Code가 로컬에 설치되어 있고 로그인된 상태 (`~/.claude/.credentials.json` 자격 증명 파일 필요)

---

## 빌드 방법

소스 코드를 로컬에서 직접 컴파일하여 빌드하는 방법입니다.

Qt 설치 경로는 PC마다 다르므로 `$QtRoot` 한 곳만 자기 환경에 맞게 바꾸면 됩니다.
(Qt Creator에서 열어 빌드해도 동일합니다. 그때는 아래 과정이 필요 없습니다.)

```powershell
# 자기 Qt 설치 경로로 바꿀 것. 버전/컴파일러 폴더명도 설치본에 맞춰 조정.
$QtRoot  = "C:\Qt"
$QtVer   = "6.11.1"
$MinGW   = "mingw1310_64"

$QtKit = "$QtRoot\$QtVer\mingw_64"
# Qt 키트를 PATH 앞에 둔다. 여러 Qt가 설치된 환경에서는
# 이 설정 없이 빌드하면 의도하지 않은 Qt가 잡힐 수 있다.
$env:PATH = "$QtKit\bin;$QtRoot\Tools\$MinGW\bin;" +
            "$QtRoot\Tools\Ninja;$QtRoot\Tools\CMake_64\bin;$env:PATH"

# CMake 인자에는 슬래시(/) 경로를 넘긴다. 역슬래시는 CMake가 이스케이프로 해석한다.
$Kit = $QtKit.Replace('\','/'); $Tools = $QtRoot.Replace('\','/') + "/Tools/$MinGW/bin"

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_PREFIX_PATH="$Kit" `
    -DCMAKE_CXX_COMPILER="$Tools/g++.exe"

cmake --build build
./build/ClaudeTray.exe
```

### 테스트 실행

시간·집계·병합 로직에는 단위 테스트가 있습니다. Qt Test 가 설치돼 있으면
빌드에 자동으로 포함되고, 없으면 조용히 건너뜁니다.

```powershell
cd build
ctest --output-on-failure
```

GUI·네트워크·자격 증명 없이 도는 순수 로직 테스트라 CI 에 그대로 넣을 수 있습니다.
테스트를 빼고 빌드하려면 `-DCLAUDETRAY_BUILD_TESTS=OFF` 를 주세요.

### 배포 패키지 생성

`cpack`은 windeployqt로 Qt 런타임을 모아 담고, MinGW·서드파티 DLL까지 함께 동봉합니다.
MSI 생성에는 [WiX Toolset 3.x](https://github.com/wixtoolset/wix3/releases)가 필요합니다.

```powershell
cd build
cpack          # claude-tracker-<버전>-win64.msi / .zip 생성
```

빌드 트리의 `ClaudeTray.exe`는 Qt DLL 없이 단독으로는 실행되지 않습니다.
배포본 검증은 반드시 `cpack` 산출물로 하세요.

### 아이콘 수정

앱/트레이 아이콘은 두 곳에 같은 도형이 정의되어 있어 **함께** 고쳐야 합니다.

```bash
python tools/gen_icon.py    # appicon.ico 재생성 (pillow 필요)
```

- `tools/gen_icon.py` → `appicon.ico` (exe·시작 메뉴 아이콘)
- `trayapp.cpp`의 `makeIcon()` → 트레이 아이콘 (사용량에 따라 채움량·색 변화)

---

## 파일 구조

- `model_pricing.json`: 최신 Claude 모델들의 단가 요율 설정 파일 (비용 계산용)
- `credentialsreader.h/cpp`: Claude Code의 로그인 정보를 읽어오는 역할
- `usageapiclient.h/cpp`: Claude API 서버와 통신하여 실시간 사용량을 조회하는 역할
- `usagescanner.h/cpp`: 로컬 JSONL 대화 로그 파일을 실시간으로 감시하고 비용을 역산하는 역할
- `usagemerge.h/cpp`: 마지막 API 정확값에 로컬 증분을 얹는 병합 로직 (GUI 비의존 순수 함수)
- `usagecalibrator.h/cpp`: API 실제 사용률을 정답지 삼아 "토큰 1개당 할당량" 계수를 모델 계열별로 온라인 학습하는 보정기 (쓸수록 로컬 추정이 정확해짐)
- `trayapp.h/cpp`: 시스템 트레이 아이콘을 구성하고 프로그램 전체 흐름을 관리하는 역할
- `usagepopup.h/cpp`: 트레이 아이콘 클릭 시 발생하는 사용량 대시보드 창 UI
- `quotapanel.h/cpp`: 사용량 게이지바와 임계 경고선 작업을 처리하는 위젯
- `toggleswitch.h/cpp`: ON/OFF 스위치 컴포넌트
- `appicon.ico` / `appicon.rc`: exe에 삽입되는 아이콘 리소스 (탐색기·시작 메뉴 표시용)
- `tools/gen_icon.py`: `appicon.ico` 생성 스크립트
- `tests/`: 시간 윈도우 집계·병합·요율 매칭 단위 테스트 (Qt Test)

---

## 라이선스

MIT License
