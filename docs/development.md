# OzMaker 개발 문서

[← 처음으로](../README.md)

[![Build](https://github.com/ejonghyuck/OzMaker/actions/workflows/build.yml/badge.svg)](https://github.com/ejonghyuck/OzMaker/actions/workflows/build.yml)

## 동작 방식

| 부분 | 구현 |
|---|---|
| 캡처 | Windows.Graphics.Capture. 창 캡처(가려져도 녹화), 모니터 캡처, 모니터 캡처 후 영역 크롭. ‘자동’이면 전체화면 게임 창은 모니터 캡처 후 창 영역 크롭. Windows 11은 시작할 때 따로 된 스레드에서 `GraphicsCaptureAccess.RequestAccessAsync(Borderless)`로 한 번 허락을 받고 `IsBorderRequired(false)`로 노란 테두리를 끔(허락 없이 끄면 무시됨). Windows 10은 끌 수 없어(`IsBorderRequired`가 없음), Windows 11도 허락되지 않았으면 모니터 캡처를 DXGI 데스크톱 복제(`DuplicateOutput1`, BGRA)로 함: 바뀔 때마다 `desk_`에 복사하고 프레임 레이트에 맞춰 넘김, 커서는 빠져 있어 GDI 호환 텍스처에 `DrawIconEx`로 그림(반전 커서 포함), 접근을 잃으면(모드 변경 · UAC · 잠금) 다시 엶. 모니터를 다른 GPU가 맡거나 회전된 화면이면 WGC로. 모니터를 담아 창으로 자를 때 그 창이 앞에 없으면 위에 올라온 다른 프로세스의 창 자리는 그 창이 가려지지 않았던 마지막 그림(`clean_`)으로 채움(그런 그림이 없을 때만 검게) |
| 여러 창 녹화 | `App::recs_`: 녹화하는 것마다 `RecSession`(대상 · 이름 · 자기 `Capture` · 결과) 하나. 여러 창이면 창마다 `Capture`를 따로 열어(늘 창 캡처, 창마다 프로세스 루프백, 마이크는 각자 공유 모드로 엶) 파일 이름을 미리 정해 겹치지 않게 함. 각 `Capture`는 `WM_APP_CAPTURE_CLOSED`의 lParam에 자기 번호(1부터)를 실어 보내고, 하나가 끝나면 `EndRecSession`이 그것만 멈춰 결과를 받아 둠(마지막 하나면 `StopRecording`). 멈출 때는 모두 일시정지한 뒤 하나씩 닫아 길이를 맞춤. 미리 보기 격자는 칸마다 DWM 썸네일(`thumbCells_` · `thumbMore_`), 칸마다 `NativeLayer` |
| 축소 | GPU에서 밉맵 + 4탭 박스 필터(녹화와 MP4 내보내기. 움짤 · WebM 내보내기는 CPU의 면적 리샘플) |
| 움짤 임시 저장 | 이전 프레임과 XOR 후 Windows 내장 XPRESS 압축, `%TEMP%\OzMaker`의 임시 파일(키프레임 30장마다)과 디스크에 쓴 프레임을 하나씩 적는 작은 기록(`.idx`). 버리거나 저장하면 둘 다 지우고, 프로세스가 죽어 남은 것은 다음 실행 때 되살림. 1프레임 지연 읽기로 GPU가 멈추지 않음 |
| 동영상 | 화면이 바뀔 때만 오는 프레임을 1/fps마다 샘플링해 고정 프레임 레이트로 만들고(가변은 아래), GPU 비디오 프로세서로 NV12(BT.709) 변환 → Media Foundation 싱크 라이터(H.264/HEVC 하드웨어 인코더 + AAC) → MP4. 영상이 GPU 메모리를 벗어나지 않음. 녹화하는 동안은 조각(fragmented) MP4로 써서 비정상 종료돼도 다 쓴 조각까지 남고(H.264만. Windows의 조각 MP4 싱크는 HEVC를 받지 않아 HEVC는 일반 MP4로 쓰며, 비정상 종료되면 남지 않음), 멈추면 그 자리에서 일반 MP4로 바꿈(모든 조각의 샘플 테이블을 담은 moov를 끝에 붙이고 조각 상자는 `free`로, 데이터는 옮기지 않음). 쓰는 중인 파일은 `%TEMP%\OzMaker`의 표시 파일이 가리키고, 남은 표시 파일은 다음 실행 때 같은 방법으로 바꿈 |
| 가변 프레임 레이트 | WGC는 그대로인 창의 프레임도 보내므로, 틱마다 그림을 떠서(`cand_`) 마지막으로 쓴 프레임(`prev_`)과 GPU에서 비교: `DiffPS`의 출력 한 픽셀이 16×16 블록이고 같은 블록은 discard, 오클루전 쿼리로 남은 블록이 있는지 봄. 바뀌었거나 키프레임 차례(키프레임 간격)일 때만 인코더로 보내고, 다음 프레임의 시각을 알 때 그 간격을 길이로 씀(`Mp4Writer::HoldVideo`, 마지막 프레임은 멈춘 시각까지). 키프레임 차례에는 인코더가 받은 프레임을 다 인코딩한 뒤 `CODECAPI_AVEncVideoForceKeyFrame`. B 프레임은 끔(Microsoft 소프트웨어 인코더의 B 프레임은 프레임마다 제 길이만큼 늦게 보이게 함). 다 쓰면 `mp4::SetVideoTiming`이 쓴 길이대로 stts를 다시 써서, 시험해 보지 못한 인코더도 시각이 맞게 함. 내보내기는 같은 프레임이 이어지는 틱을 한 프레임으로 묶음(`PlanVideoRuns`, MP4는 키프레임 간격 · WebM은 5초마다 키프레임). 편집 탭에서 읽을 때는 소스 리더의 비디오 프로세서가 평균 프레임 레이트로 다시 맞추지 않게 `MF_XVP_DISABLE_FRC` |
| 소리 | 프로세스 루프백: 창은 그 프로세스 트리만(`INCLUDE_TARGET_PROCESS_TREE`), 전체 화면 · 영역은 OzMaker의 프로세스 트리만 빼고(`EXCLUDE_TARGET_PROCESS_TREE`, 알림음이 녹음되지 않음. Windows 10 2004 전에는 WASAPI 시스템 루프백). OzMaker에서 여는 플레이어 · 브라우저는 탐색기를 통해 열어 OzMaker의 자식이 되지 않으므로 녹음에서 빠지지 않음. QPC 타임스탬프로 영상과 맞추고, 아무 소리도 없는 구간은 무음으로 채움. 마이크는 같은 시간축의 WASAPI 캡처를 표본 단위로 더해 한 트랙으로 |
| 순간 리플레이 | `src/Replay.cpp`: 조각 MP4 싱크 라이터를 파일 대신 메모리의 `IStream`(`RingStream`)에 쓰게 하고(H.264만), 들어오는 moov와 각 moof · mdat를 샘플 단위로 읽어 둠(Windows의 싱크는 tfdt를 쓰지 않고 0.3초쯤의 조각을 키프레임과 관계없이 끊으므로 시각은 샘플 길이를 더해 셈). 길이 + 키프레임 간격 + 2초보다 오래된 조각은 버림. 저장은 누른 시각까지의 조각이 들어오기를 기다린 뒤, 그 시각 − 길이 이하의 마지막 키프레임부터의 영상과 그 사이에 보이는 소리만으로 조각을 다시 만들어 파일에 쓰고 일반 MP4로 바꿈(다시 인코딩 없음). 소프트웨어 인코더의 B 프레임 지연(첫 프레임의 합성 시각)만큼 소리를 옮겨 맞춤. 대상 고르기 · 저장 · 알림은 `src/AppReplay.cpp`, 1초마다 창 목록에서 목록의 exe와 같은 프로그램을 찾음(앞에 있는 것, 담던 것, 가장 최근에 앞에 있던 것 순). 잠금(`WTSRegisterSessionNotification`) · 절전(`PBT_APMSUSPEND`)이면 메모리를 비움 |
| 동영상 편집 | MP4의 샘플 테이블(stts/ctts/stss)에서 프레임 시각과 키프레임을 바로 읽어 디코딩 없이 색인(5분 영상 7 ms). 미리보기는 별도 스레드에서 GPU 디코딩(소스 리더 + D3D11)하며 가장 최근에 요청한 프레임만, 가능하면 앞으로 이어서, 아니면 직전 키프레임부터 디코딩. 내보내기는 자르기 · 크기 조절 · 자막을 GPU에서 하고(`GpuResizer`: 크롭만 올려 밉맵 + 4탭 박스 필터, 같은 크기면 그대로 복사, 자막은 미리 곱한 알파로 섞고 투명한 곳은 흰색. 만들 수 없으면 CPU) 고정 또는 가변 프레임 레이트로 다시 인코딩하고 소리는 같은 구간을 잘라 AAC로. 인코딩 없이 자르기는 구간 앞 키프레임부터의 샘플만 새 moov(파일 앞쪽)와 mdat 하나로 복사(영상·소리를 약 0.5초씩 번갈아). Media Foundation은 편집 목록(elst)의 media_time을 건너뛰지 않고 그만큼 뒤로 미루므로 media_time은 쓰지 않음: 영상은 합성 시각 차이(ctts)를 키프레임 기준으로 옮기고, 소리는 잘린 자리에서 가장 가까운 AAC 프레임 경계부터 시작(11 ms 안) |
| 자막 | 미리보기는 DirectWrite로 화면에 바로 그리고, 내보낼 때는 같은 글자를 출력 크기의 WIC 비트맵에 한 번 그려 각 프레임의 글자가 있는 줄에만 섞음. 짧은 변의 7.5% 크기 굵은 글꼴, 그림자 + 검은 테두리 + 흰 글자, 너비의 92%를 넘으면 줄임 |
| 코덱 자동 | H.264 인코더는 긴 변 4096, HEVC는 8192까지 받음. 자동은 해상도 상자를 적용한 크기가 4096을 넘고 HEVC 인코더(`MFTEnumEx` 인코더 분류)와 HEVC 디코더(디코더 분류, 보통 HEVC 비디오 확장)가 둘 다 있을 때만 HEVC, 아니면 H.264로 4096에 맞춰 줄임. 녹화(`App::PlannedSize` → `BeginCapture`의 `maxEdge`)와 MP4 내보내기(`App::MakeJob`)가 같은 규칙 |
| 단축키 | 평소에는 `RegisterHotKey`. 다른 프로그램이 가진 조합은 사용자가 ‘그래도 OzMaker가 쓰기’를 누른 것만, 그리고 `RegisterHotKey`가 거절하는 동안만 저수준 키보드 후크(`src/KeyHook.cpp`, 전용 스레드의 `WH_KEYBOARD_LL`)로 받음. 후크는 그 조합의 누름을 `WM_HOTKEY`로 창에 보내고 그 누름 · 반복 · 뗌을 삼킴(수식 키가 정확히 같을 때만). Win · Alt가 든 조합은 시작 메뉴 · 메뉴 막대가 열리지 않도록 뜻 없는 키(vkE8)를 한 번 눌러 둠. 단축키를 지정하는 동안에도 켜서, OzMaker 창이 앞에 있을 때 단축키가 될 만한 조합을 칸으로 넘김(Alt+Tab · Ctrl+Esc 등은 그대로 둠). 키를 기록하지 않음 |
| UI | Direct2D/DirectWrite로 직접 그린 즉시 모드 UI. 입력·애니메이션이 있을 때만 그려서 대기 중 CPU 0%. 스왑 체인은 flip이 아닌 비트 블록 전송(`DXGI_SWAP_EFFECT_DISCARD`): 캡처에서 뺀(`WDA_EXCLUDEFROMCAPTURE`) flip 모델 창을 Windows 10이 왼쪽 위로 밀고 오른쪽 · 아래를 희게 그리며 깜박였음. 캡처 제외는 화면을 담는 녹화 중에만 |
| 테마 | 색은 모두 `theme::P`(`src/Theme.h`)에서 가져옴. 다크 · 라이트 · 대비 테마(`GetSysColor`로 만듦) 중 하나로, `WM_SETTINGCHANGE` · `WM_SYSCOLORCHANGE` · `WM_THEMECHANGED`가 오면 다시 고름. 녹화할 화면 위에 뜨는 것(`src/Overlays.cpp`)은 대비 테마가 아니면 늘 다크. Windows의 애니메이션 효과가 꺼져 있으면 `Ui::AnimFrom`이 바로 목표값으로 감 |
| 화면 낭독기 | UI Automation(`src/Access.cpp`): 창이 루트이고, 지난 프레임에 그린 노드(`Ui::Nodes`)가 그 아래 요소. 낭독기가 누르거나 바꾼 것은 다음 프레임의 입력으로 들어감 |
| 번역 | 화면의 글은 한국어 원문을 그대로 `tr(L"...")`로 감싸고, 영어는 `src/Lang.cpp`의 표에서 원문으로 찾음(없으면 한국어 그대로). 배열에 든 글은 쓰는 자리에서 `tr()`. printf 서식 문자열은 영어도 같은 필드를 같은 순서로 씀. settings.ini에 저장되는 값(`{날짜}` 같은 파일 이름 형식)은 감싸지 않음 |
| GIF | 자체 인코더: 바뀐 픽셀만 다시 인코딩(투명 + 영역 크롭). 바뀐 것으로 치는 문턱을 화면의 노이즈 정도에 맞춰 올려서 그레인이 있는 배경이 반짝이지 않음. Wu 양자화 + k-means 보정 팔레트를 만들되 앞 프레임의 팔레트로 충분하면 그대로 씀. 바뀐 영역에만 Floyd–Steinberg 디더링. 손실 압축(gifsicle `--lossy`와 같은 방식)은 LZW 문자열을 비슷한 색까지 이어 붙여 용량을 줄임. 누적 반올림으로 정확한 재생 시간 |
| WebP / AVIF | libwebp(WebPAnimEncoder, 색 경계가 번지지 않게 sharp YUV) / libavif + aom(품질 90 이상이면 색을 줄이지 않는 4:4:4). vcpkg가 있을 때만 빌드에 포함됩니다 |
| WebM | `src/WebmWriter.cpp`: libvpx VP9(CPU, `cpu-used` 5, 행 단위 멀티스레드, 화면 내용 튜닝, 품질 → CQ 4–63, 용량 맞추기는 VBR) + Opus(48 kHz, 20 ms. 44.1 kHz는 윈도 sinc로 바꿈) → libwebm(mkvmuxer)으로 파일에 씀(탐색용 큐 포함). 소리 패킷은 시각 순서대로 영상 사이에 넣고, 마지막 패킷의 남는 부분은 DiscardPadding으로 표시. 색은 libyuv BT.601 제한 범위 |
| 움짤 열기 | `src/AnimClip.cpp`: GIF는 WIC로 읽고 처리 방식(disposal)을 직접 합성, WebP는 WebPAnimDecoder, AVIF는 libavif. 프레임은 미리 곱한 알파의 BGRA로 임시 저장(기록 없이, 닫으면 지워지는 파일)에 넣고, 내보낼 때 축소 · 자막도 알파를 지킴. 움짤 형식은 알파를 되돌려 쓰고, MP4 · WebM은 흰 바탕에 합성 |
| 업데이트 | 시작할 때와 하루 한 번 GitHub 최신 릴리스의 `update.txt`(버전 · 크기 · SHA-256)를 WinHTTP로 읽고, 새 버전이면 exe를 옆에 `OzMaker.exe.new`로 받아 검증해 둠. 누르면 실행 중인 exe를 `.old`로 이름 바꾸고 새 exe를 넣은 뒤 다시 시작(쓰기 권한이 없는 폴더면 릴리스 페이지를 엶) |
| 충돌 기록 | 처리하지 못한 예외 · abort · CRT 오류가 나면 작은 미니덤프(스레드와 스택만, 화면 내용 없음)와 짧은 로그를 설정 파일이 있는 폴더의 `crash`에 남기고 Windows 오류 창 없이 끝냄. 아무 데도 보내지 않고 다음 실행 때 한 번 알림(설정 › 정보 › 충돌 기록, 최근 5개만 남김). 덤프는 그 릴리스의 `OzMaker-<버전>-pdb.zip`으로 읽음 |

## 빌드

Visual Studio 2022(C++ 데스크톱 워크로드, Windows SDK 10.0.22621)가 필요합니다.

```bash
msbuild OzMaker.vcxproj /p:Configuration=Release /p:Platform=x64
```

결과물: `bin\Release\OzMaker.exe` (런타임 정적 링크, 단일 실행 파일). MP4와 소리 녹음은 Windows에 들어 있는 Media Foundation·WASAPI만 쓰므로 추가 라이브러리가 필요 없습니다.

### WebP / AVIF / WebM 포함하기

`..\vcpkg`(또는 `VCPKG_ROOT`)에 부트스트랩된 vcpkg가 있으면 `vcpkg.json`의 라이브러리(libwebp, libavif + aom, libvpx, Opus, libwebm)를 자동으로 받아 함께 빌드합니다. 첫 빌드는 aom(AV1 인코더)과 libvpx 컴파일 때문에 오래 걸립니다.

```bash
git clone https://github.com/microsoft/vcpkg ../vcpkg
../vcpkg/bootstrap-vcpkg.bat -disableMetrics
```

vcpkg가 없으면 GIF와 MP4만 들어간 빌드가 만들어지고, WebP·AVIF·WebM 버튼은 비활성화되며 WebP·AVIF 파일은 열 수 없습니다. `/p:OzNoVcpkg=true`로 강제로 끌 수도 있습니다. 함께 들어가는 라이브러리는 모두 BSD 계열이며 [THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md)에 정리되어 있습니다.

## 배포용 zip 만들기

```bash
powershell -ExecutionPolicy Bypass -File tools\package.ps1
```

Release로 빌드한 뒤 `dist\OzMaker-<버전>-win-x64.zip`을 만듭니다. 안에는 `OzMaker.exe`, `LICENSE.txt`, `README.txt`, `THIRD-PARTY-NOTICES.md`가 들어가고, vcpkg로 빌드했다면 라이브러리 라이선스 전문이 `licenses` 폴더에 함께 들어갑니다. 이미 빌드했다면 `-NoBuild`를 붙이세요.

`README.txt`는 `tools\zip-readme.txt`의 `{version}`을 채워 만듭니다. 받은 사람이 메모장으로 여는 짧은 안내라서 마크다운이나 그림을 넣지 않고, 자세한 내용은 저장소 주소로 보냅니다.

`v0.6.0` 같은 태그를 푸시하면 GitHub Actions가 같은 zip과 업데이트용 `OzMaker.exe` · `update.txt`를 릴리스로 올립니다. 그 전에 `tools\check-version.ps1`이 태그가 `src\Version.h`의 버전과 같은지, 버전을 되풀이하는 `res\app.manifest` · `vcpkg.json`이 같은지 보고, 다르면 아무것도 올리지 않고 멈춥니다.

## 테스트용 명령줄

```bash
OzMaker.exe --selftest <dir>          # 합성 클립으로 저장소 왕복 + 모든 인코더 검사(WebM은 libwebm · libvpx · Opus로 다시 읽어 프레임 · 소리 확인)
OzMaker.exe --screenshot <dir> [light|contrast|en]   # 각 화면을 PNG로 렌더링(창 미리보기 자리는 파란색), 레이어 오류나 이름 없는 단추가 있으면 실패
                                      # light · contrast · en: 라이트 테마 · 대비 테마 · 영어로. en은 영어가 없는 글이 있어도 실패
OzMaker.exe --readme-media <dir>      # README 그림(hero.gif와 기능 PNG 4장)을 화면 밖에서 그려 저장(아래 참고)
OzMaker.exe --record-test <title> <sec> <dir> [fps] [maxEdge] [window|display|monitor|region] [clip|mp4|mp4hevc|mp4sw] [audio|mute] [pause] [mic] [click] [vfr] [dda|wgc]
                                      # mic: 기본 마이크를 섞어 녹음(방 소리가 녹음되므로 직접 실행할 때만)
                                      # click: 가짜 클릭 하나를 넣고 프레임에 클릭 강조가 보이는지 확인
                                      # vfr: 가변 프레임 레이트로 녹화하고 프레임 수 · 가장 긴 프레임 · 키프레임 간격 · 길이 확인
OzMaker.exe --test-window <sec> [capture]   # ‘OzMaker Test Window’를 띄움(--record-test의 대상)
OzMaker.exe --test-window <sec> cover <x> <y> <w> <h>   # 그 자리에 흰 창(맨 위, 포커스 없음): --replay-test가 다른 프로그램의 창으로 씀
                                      # capture: 녹화할 수 있는 창으로, 회색 바탕에 1.5초마다 깜박이는 흰 네모
OzMaker.exe --analyze <file.mp4> <dir> [freq ...]   # MP4 길이·프레임 간격·테스트 톤 크기 측정
OzMaker.exe --video-test <dir> [w h]  # 합성 MP4로 색인·디코딩·구간 자르기(프레임 번호·소리 경계, MP4와 WebM) 검사
OzMaker.exe --encoder-test <dir>      # 녹화용 MP4를 HEVC · CBR · 2474×1392 등 설정마다 1초씩 써 보고, 고른 인코더와 실패한 호출을 기록
OzMaker.exe --editor-test <file.mp4> <dir>   # 실제 창에서 저장된 동영상 → 편집 → 키보드(S로 프레임 이미지) → 잘라서 저장
OzMaker.exe --region-test <dir>       # 영역 지정 후 다듬기(손잡이·비율·키보드), 막대(카운트다운·영역만 저장·녹화), 직전 영역
OzMaker.exe --preview-test <dir>      # 화면에서 확인: 파일 목록이 창 미리보기 위에 보이는지, 다른 프로그램 창이 목록에 바로 반영되는지
OzMaker.exe --replay-test <dir>       # 순간 리플레이: 합성 영상으로 링 버퍼의 자르기(길이 · 첫 키프레임 · 소리 맞춤, 하드웨어와 소프트웨어),
                                      # 그리고 --test-window를 대상으로 켜기 → 담기 → 저장 → 창이 닫힌 뒤 남은 것 저장 → 끄기(테스트 창만 담음), 단축키로 스크린샷,
                                      # 창이 여럿일 때 잠깐 본 창은 옮기지 않고 리플레이 길이만큼 앞에 있으면 옮김, 닫힌 뒤 남은 창은 기다리고 새 창은 바로 담음
OzMaker.exe --multi-test <dir>        # 여러 창 녹화: --test-window 두 개를 체크해 움짤로(둘 다 남고 첫 창이 열림), 그리고 MP4로
                                      # 녹화하며 첫 창이 도중에 닫힘(그 파일만 먼저 끝남, REC 표시가 다음 창으로, 카드에 두 파일),
                                      # 스크린샷(창마다 한 장, 줄인 녹화에서도 창 크기, 일시정지 중에도, 끝나면 키를 놓음)
OzMaker.exe --safety-test <dir>       # 긴 녹화 안전장치: --test-window를 녹화하며 깨어 있기 요청이 녹화 중에만 걸리는지, 앱에 보낸 잠금 · 절전 메시지로
                                      # 빈 구간이 결과에 남는지, 인코더가 프레임을 안 받으면 5초쯤 뒤에 멈추고 파일이 남는지(마무리까지 실패해도) 확인
OzMaker.exe --update-test <dir> [url|github]  # 가짜 릴리스 폴더로 업데이트 확인 → 받기 → 검증 → 교체(--selftest에도 포함)
OzMaker.exe --exit                    # 실행 중인 OzMaker 종료
```

테스트 명령은 Windows 설정과 관계없이 한국어 · 다크 테마로 그립니다(CI는 영어 Windows에서 돌아감). `--selftest`는 세 테마의 글씨 · 바탕 대비(4.5:1 이상)와 영어 표의 서식 필드도 검사합니다. 화면의 글을 바꾸면 `src/Lang.cpp`의 영어도 고치고 `--screenshot <dir> en`으로 확인하세요.

`--selftest`는 프레임 이미지의 저장(PNG · JPG · WebP를 다시 읽어 픽셀 비교, 자르기 · 자막 · 투명)과 순간 리플레이의 링 버퍼 자르기와 리플레이 설정의 저장 · 읽기, 데스크톱 복제용 커서 그리기(화살표와 반전되는 글자 커서), Windows 11에서 테두리 끄기 허락에 답이 오는지도 봅니다(앱 전체 흐름과 데스크톱 복제는 `--replay-test`: 맨 위에 띄운 테스트 창 영역을 클립과 MP4로 담아 색 블록 · 깜박이는 네모 · 캡처에서 뺀 창이 안 보이는지 확인. 가려진 창도 봄: 창 캡처는 GDI 창과 flip 모델 창 모두 가려진 부분이 그린 대로, 화면 캡처는 다른 프로그램의 창이 올라온 자리가 마지막 그림으로 나오는지). 화면을 캡처하는 테스트는 그 자리에 OzMaker가 아닌 창이 하나라도 있으면 아무것도 캡처하지 않고 실패로 남깁니다. `--selftest`는 MP4 내보내기의 GPU 크기 조절을 CPU 결과와 비교하고(같은 크기는 ±1, 줄이면 PSNR 38 dB 이상, 1920×1080 → 1280×720 한 장의 시간도 기록), 가변 프레임 레이트로 쓴 MP4 · WebM을 다시 읽어 프레임 시각과 키프레임을 봅니다. 0.6.4까지의 설정(최대 녹화 길이 → 녹화 타이머, H.264 → 코덱 자동)을 읽는 것, 설정 초기화가 남기는 것과 되돌리는 것, 코덱 자동이 고르는 코덱과 크기(가짜 인코더로)도 봅니다. 단축키 가져오기는 메시지 전용 창이 등록한 F23을 `SendInput`으로 눌러, 후크가 없을 때는 그 창이, 후크가 있을 때는 OzMaker만 받는지 확인합니다. 키 입력이 닿지 않는 데스크톱(잠긴 화면 등)에서는 건너뜁니다.

환경 변수 `OZMAKER_SETTINGS`에 경로를 주면 `%APPDATA%\OzMaker\settings.ini`(포터블이면 exe 옆의 `settings.ini`) 대신 그 파일을 씁니다(테스트용). `OZMAKER_UPDATE_URL`은 업데이트를 확인할 곳(주소 또는 폴더)을 바꿉니다.

## README 그림 다시 만들기

README의 움짤과 기능 그림은 `--readme-media`가 만듭니다. 화면이 바뀌면 다시 만들어 함께 커밋하세요.

```bash
set OZMAKER_SETTINGS=%TEMP%\oz-media.ini
OzMaker.exe --readme-media docs\images
```

- 화면을 찍지 않습니다. `--screenshot`처럼 앱의 화면을 화면 밖에서 그리고, 창 목록에는 만든 창 세 개(‘데모 게임’ 등)만 둡니다. 창 미리보기 자리와 녹화한 클립에는 코드로 그린 2D 게임 장면(`DemoFrame`)이 들어갑니다.
- `hero.gif`: 정해 둔 순서대로 포인터가 움직입니다(창 고르기 → 녹화 → 구간 끝 끌기 → 자막 → 10MB → 저장 → 복사). 버튼 위치는 그 순간의 화면에서 찾으므로 배치가 바뀌어도 따라갑니다. 녹화 · 저장 · 복사는 실제로 하지 않고 화면만 그렇게 바꿉니다(결과 카드의 용량은 그 구간을 실제로 저장해 본 값). 15fps로 그린 프레임을 OzMaker의 GIF 인코더가 저장합니다.
- PNG는 192 DPI(2배)로 그려 필요한 부분만 잘라 냅니다. README에서는 `width`로 줄여 보입니다.
- 같은 화면이면 같은 파일이 나옵니다. 끝나면 `readme-media.log`에 파일마다 크기가 남고(커밋하지 않음), README의 움짤 설명에 적은 용량은 이 값으로 고칩니다.
