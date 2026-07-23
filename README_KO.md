# iSH-AOK

iSH-AOK는 [ish-app/ish](https://github.com/ish-app/ish)의 포크로, 이 트리에서의 일상적인 개발을 위한 자체 제품, 툴링, 플랫폼 변경 사항을 포함하고 있습니다.

Testflight: https://testflight.apple.com/join/X1flyiqE

이 포크는 단순한 리브랜딩이 아닙니다. 포크 전용 동작, 번들된 루트 파일시스템, 진단 작업, File Provider 통합, 그리고 실험적인 amd64/x86_64 게스트 구현 작업을 포함하고 있습니다. 업스트림 iSH를 원한다면 `ish-app/ish`를 사용하세요. 이 저장소에서 작업 중이라면, 이 README가 참고해야 할 문서입니다.

## 이 포크가 추가한 것

- 포크 전용 앱 아이덴티티:
  - 제품명 `iSH-AOK`
  - 번들 루트 `app.ish.iSH-AOK`
- 앱 빌드에 번들된 루트 파일시스템:
  - `i386`용 `root.tar.gz` (`Devuan5(Debian12)`)
  - `alpine-minirootfs-3.23.3-x86.tar.gz` (`Alpine3.23.3`)
  - `alpine-minirootfs-3.23.3-x86_64.tar.gz` (`Alpine3.23.3(x86_64)`)
- iOS를 통해 게스트 파일을 노출하는 File Provider 지원.
- 이 포크 전용의 추가 진단 및 운영 관련 변경 사항.
- 진행 중인 amd64 인터프리터, 로더, 시스템 콜 작업.

## 현재 amd64 상태

이 저장소의 amd64 작업은 실험적입니다.

- 앱은 `x86_64` 게스트 루트를 가져와서 부팅을 시도할 수 있습니다.
- 인터프리터, ELF64 로더, amd64 시스템 콜 경로는 활발히 구현 중입니다.
- 초기 부팅 실패, 디코드 공백, 부분적인 유저랜드 실행이 예상됩니다.
- 해당 작업의 현재 개발 브랜치는 보통 `amd64`입니다.

관련 파일:

- [amd64_port_plan.md](docs/amd64_port_plan.md)
- [emu/amd64_interp.c](emu/amd64_interp.c)
- [kernel/exec.c](kernel/exec.c)
- [kernel/calls.c](kernel/calls.c)

## 저장소 구조

- `app/`: iOS 앱, UI, 루트 선택, 진단, File Provider 통합.
- `emu/`: amd64 인터프리터 작업을 포함한 게스트 CPU 에뮬레이션.
- `kernel/`: 시스템 콜 변환, 프로세스 모델, exec, 시그널, 메모리 관리.
- `fs/`: 파일시스템 계층 및 fakefs 통합.
- `jit/`: iSH로부터 계승된 스레드 코드 JIT 메커니즘.
- `tests/`: 수동 및 자동 테스트 도우미.
- `tools/`: 개발자 도구 및 호스트 측 헬퍼.

## 클론

이 저장소는 서브모듈을 사용합니다.

```bash
git clone --recurse-submodules git@github.com:emkey1/ish-AOK.git
cd ish-AOK
```

서브모듈 없이 이미 클론했다면:

```bash
git submodule update --init --recursive
```

## 빌드 요구 사항

로컬 개발을 위해 일반적으로 다음이 필요합니다:

- Xcode
- Python 3
- Meson
- Ninja
- Clang/LLVM 툴체인
- sqlite3
- libarchive

macOS에서 일반적인 설정:

```bash
brew install meson ninja llvm libarchive
```

`sqlite3`은 보통 이미 설치되어 있습니다.

## iOS 앱 빌드

[iSH-AOK.xcodeproj](iSH-AOK.xcodeproj)를 Xcode에서 열고 `iSH` 스킴을 빌드하세요.

포크 전용 주요 설정:

- 번들 ID는 [app/iSH.xcconfig](app/iSH.xcconfig)에서 관리됩니다.
- `ROOT_BUNDLE_IDENTIFIER`의 기본값은 `app.ish.iSH-AOK`입니다.
- 프로젝트는 이미 포크 전용 디버그 구성인 `Debug-ApplePleaseFixFB19282108`을 사용합니다.

커맨드 라인 빌드:

```bash
xcodebuild \
  -project iSH-AOK.xcodeproj \
  -scheme iSH \
  -sdk iphonesimulator \
  -configuration Debug-ApplePleaseFixFB19282108 \
  build CODE_SIGNING_ALLOWED=NO
```

iOS 빌드 스크립트는 저장소 루트에 다음 아카이브가 있으면 앱 번들로 복사합니다:

- `root.tar.gz`
- `alpine-minirootfs-3.23.3-x86.tar.gz`
- `alpine-minirootfs-3.23.3-x86_64.tar.gz`

해당 파일이 없으면 대응하는 번들 루트는 동작하지 않습니다.

## 릴리스 자동화 (점진적)

이 저장소에는 간단한 헬퍼 스크립트가 포함되어 있습니다:

- [`tools/release-aok.sh`](tools/release-aok.sh)

먼저 안전한 부분부터 시작하세요:

```bash
./tools/release-aok.sh preflight
./tools/release-aok.sh archive
./tools/release-aok.sh export latest /tmp/iSH-AOK-export
```

이렇게 하면 수동 업로드를 유지하면서도 반복 가능한 아카이브 + IPA 익스포트 흐름을 얻을 수 있습니다.

전체 TestFlight 자동화가 준비되면 다음을 사용하세요:

```bash
./tools/release-aok.sh upload-fastlane
```

`upload-fastlane`은 기존 `fastlane upload_build` 레인을 사용하며, Ruby/Bundler/Fastlane 설정과 서명/인증 시크릿이 필요합니다.

`preflight`에서 Ruby가 너무 오래되었다고 나오면:

```bash
brew install ruby@3.3
echo 'export PATH="/opt/homebrew/opt/ruby@3.3/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc
bundle install
```

## 네이티브 CLI / 에뮬레이터 빌드

에뮬레이터 측 작업에는 보통 전체 Xcode 빌드보다 Meson 빌드가 더 빠릅니다.

초기 설정:

```bash
meson setup build
```

증분 빌드:

```bash
ninja -C build
```

유용한 타겟:

- `build/ish`
- `build/libish.a`

대부분의 에뮬레이터 변경에는 이것으로 충분합니다:

```bash
ninja -C build libish.a
```

## 회귀 테스트

이 트리에는 `tests/e2e/` 아래의 엔드투엔드 테스트와 `tests/manual/` 아래의 집중형 게스트 측 프로브가 모두 있습니다.

가장 관련 있는 atomic/JIT 회귀 프로브는 다음과 같습니다:

- [tests/manual/atomics32.c](tests/manual/atomics32.c)
- [tests/manual/atomic_xadd32.c](tests/manual/atomic_xadd32.c)
- [tests/manual/atomic_cmpxchg32.c](tests/manual/atomic_cmpxchg32.c)
- [tests/manual/atomic_cmpxchg8b.c](tests/manual/atomic_cmpxchg8b.c)
- [tests/manual/atomic_logic32.c](tests/manual/atomic_logic32.c)
- [tests/manual/futex_core.c](tests/manual/futex_core.c)
- [tests/manual/signal_core.c](tests/manual/signal_core.c)
- [tests/manual/signal_restart.c](tests/manual/signal_restart.c)
- [tests/manual/signal_realtime.c](tests/manual/signal_realtime.c)
- [tests/manual/signal_altstack.c](tests/manual/signal_altstack.c)
- [tests/manual/signal_poll.c](tests/manual/signal_poll.c)
- [tests/manual/eventfd_interrupt.c](tests/manual/eventfd_interrupt.c)
- [tests/manual/amd64_regress.c](tests/manual/amd64_regress.c)
- [tests/manual/test_common.h](tests/manual/test_common.h)

`atomics32.c`는 상위 프로브입니다. 분리된 프로그램들은 게스트 내부에서 컴파일되도록 되어 있으며, 불일치 시 0이 아닌 값으로 종료하므로 다음 항목들에 대한 반복 가능한 회귀 타겟으로 사용할 수 있습니다:

- 잠긴 `xadd`
- 잠긴 `cmpxchg`
- 잠긴 `cmpxchg8b`
- 잠긴 논리 연산 및 인접 플래그 소비자
- futex 대기/깨우기, 타임아웃, 시그널 인터럽트, 재시작 동작
- 시그널 전달, 대기 마스크, `sigtimedwait`, `signalfd`, `sigsuspend`, 스레드 대상 시그널
- `poll`/`select`/`pselect`/`ppoll` 시그널 인터럽트 및 EINTR 시맨틱
- `SA_RESTART` 하의 블로킹 시스템 콜 재시작 동작
- 페이로드가 있는 큐잉된 실시간 시그널
- 대체 스택 시그널 핸들러 전달
- eventfd 읽기 인터럽트 및 공유 대기 경로를 통한 `SA_RESTART` 동작
- 페이지 경계를 넘는 COW 쓰기, exec 로더 제로잉, `fcntl` 락 수명 경합, 게스트 `gcc`에서의 `cc1` 스트레스 등 amd64 관련 회귀

앱에 번들된 루트나 iSH-AOK에서 실행 중인 가져온 루트의 경우, 게스트 측 설정 헬퍼는 다음과 같습니다:

- [tests/manual/setup-regressions.sh](tests/manual/setup-regressions.sh)

게스트 내부에서는 동일한 `/AOK/tests/setup-regressions.sh`로 집중형 스위트를 준비, 빌드, 실행할 수 있습니다.

집중형 게스트 측 회귀는 모두 `-v` 또는 `--verbose`를 받습니다. 없으면 실패 항목과 최종 스위트 `PASS`/`FAIL` 라인만 출력합니다.

## 루트 파일시스템 다루기

이 포크는 현재 앱에서 세 가지 번들 선택지를 제공합니다:

- `i386`용 `Devuan5(Debian12)`
- `i386`용 `Alpine3.23.3`
- `amd64`용 `Alpine3.23.3(x86_64)`

루트 선택 UI 및 메타데이터 처리는 다음에 있습니다:

- [app/Roots.m](app/Roots.m)
- [app/RootsTableViewController.m](app/RootsTableViewController.m)

참고 사항:

- `x86_64` 루트는 일반 사용자용이 아니라 구현 작업용입니다.
- 앱은 가져온 루트별로 게스트 ABI를 기록합니다.
- File Provider 도메인은 관리되는 루트에 대해 동기화됩니다.

## 로깅 및 진단

로깅은 [app/iSH.xcconfig](app/iSH.xcconfig)의 `ISH_LOG`로 제어됩니다.

예시:

```xcconfig
ISH_LOG = verbose strace
```

현재 로거 기본값:

- iPhone / 시뮬레이터: `nslog`
- macOS: `dprintf`

자주 쓰는 채널:

- `strace`
- `verbose`
- `instr`

에뮬레이터 구현 작업에는 보통 다음 루프가 가장 빠릅니다:

1. 인터프리터 또는 커널 코드를 패치합니다.
2. `ninja -C build libish.a`를 실행합니다.
3. iOS 앱을 다시 빌드합니다.
4. 시뮬레이터에서 실행합니다.
5. 결함이 발생한 RIP, opcode 윈도우, 레지스터 상태를 콘솔 로그에서 확인합니다.

## File Provider

이 포크는 게스트 파일을 시스템 파일 API를 통해 노출하기 위한 iOS File Provider 익스텐션을 포함합니다.

관련 코드:

- [app/FileProvider/FileProviderExtension.m](app/FileProvider/FileProviderExtension.m)
- [app/FileProvider/FileProviderEnumerator.m](app/FileProvider/FileProviderEnumerator.m)
- [app/FileProvider/FileProviderItem.m](app/FileProvider/FileProviderItem.m)

이것은 포크 전용 기능이며 이곳에서 유지 관리되는 제품 표면의 일부로 취급되어야 합니다.

## amd64 개발 워크플로

이 저장소에서 amd64 작업을 한다면:

- 먼저 인터프리터를 우선시하고, JIT 경로가 아직 관련 있다고 가정하지 마세요.
- 수정 사항은 작고 되돌릴 수 있게 유지하세요.
- 다음 두 가지로 검증하세요:
  - `ninja -C build libish.a`
  - 시뮬레이터 `xcodebuild`
- 게스트가 실패하면 다음을 캡처하세요:
  - 결함 유형
  - 게스트 RIP
  - opcode 윈도우
  - 게스트 레지스터
  - 추가한 타겟 트레이스 출력

현재 amd64 작업은 다음을 자주 다룹니다:

- [emu/amd64_interp.c](emu/amd64_interp.c)의 명령어 디코드 및 실행
- [jit/jit.c](jit/jit.c)의 JIT 핸드오프 및 디스패치
- [kernel/exec.c](kernel/exec.c)의 ELF64 및 프로세스 시작
- [kernel/calls.c](kernel/calls.c)의 시스템 콜 디스패치 및 ABI 처리

## 브랜치

작성 시점 기준:

- `working`은 이 포크의 활성 통합 브랜치입니다. 버그 수정, 기능 작업, 릴리스 후보가 먼저 여기에 반영됩니다.
- `main`은 병합되어 안정화된 코드를 추적하며, 릴리스가 나갈 때 `working`으로부터 업데이트됩니다.
- `amd64`는 x86_64 게스트 구현 작업을 위한 활성 브랜치입니다.
- `aarch64`는 네이티브 ARM64 게스트 구현 작업을 위한 활성 브랜치입니다 ([aarch64_guest_plan.md](docs/aarch64_guest_plan.md) 참고).

여러 브랜치에 걸친 문서를 업데이트할 때는 관련 브랜치들을 모두 동기화하세요.

## 업스트림과의 관계

iSH-AOK는 업스트림 iSH를 기반으로 하지만, 의도적으로 갈라져 나왔습니다.

즉:

- 업스트림 README의 지침은 이 포크에서는 불완전하거나 맞지 않을 수 있습니다
- 브랜치 이름과 빌드 구성이 다를 수 있습니다
- 번들 루트 및 운영 동작은 이 포크에 특화되어 있습니다
- 여기서의 실험적인 amd64 지원이 업스트림에도 존재한다고 가정해서는 안 됩니다

## 감사의 말

`aarch64` 브랜치의 네이티브 ARM64 게스트 작업은 [OpenMinis/ish-arm64](https://github.com/OpenMinis/ish-arm64)에서 영감을 받았고 일부는 그 코드를 참고했습니다. 이는 동일한 기능을 독립적으로 구현한 `ish-app/ish`의 GPLv3 포크입니다.
파일 단위 크레딧은 [CREDITS-aarch64.md](docs/CREDITS-aarch64.md)를 참고하세요.

## 라이선스

다음을 참고하세요:

- [LICENSE.md](LICENSE.md)
- [LICENSE.IOS](LICENSE.IOS)
