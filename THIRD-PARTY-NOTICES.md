# 서드파티 고지 (Third-party notices)

OzMaker 자체는 [MIT 라이선스](LICENSE)로 배포됩니다.

## 기본 빌드 (GIF · MP4)

vcpkg 없이 빌드한 `OzMaker.exe`에는 외부 라이브러리가 들어 있지 않습니다. 화면 캡처, 동영상 인코딩(H.264 / HEVC / AAC), 소리 녹음, 화면 그리기는 Windows에 들어 있는 구성 요소(Windows.Graphics.Capture, Media Foundation, WASAPI, Direct3D 11, Direct2D, DirectWrite)를 호출해서 씁니다. 동영상 코덱도 Windows와 그래픽카드 드라이버가 제공하는 것을 쓰고, 프로그램 안에 넣지 않습니다.

## vcpkg로 빌드할 때 (WebP · AVIF 포함)

| 라이브러리 | 용도 | 라이선스 | 홈페이지 |
|---|---|---|---|
| libwebp | WebP 인코딩 | BSD-3-Clause | https://github.com/webmproject/libwebp |
| libavif | AVIF 인코딩 | BSD-2-Clause (일부 파일 Apache-2.0) | https://github.com/AOMediaCodec/libavif |
| aom (libaom) | AV1 인코더 | BSD-2-Clause 등, Alliance for Open Media Patent License 1.0 | https://aomedia.googlesource.com/aom |
| libyuv | 색 공간 변환 (libavif가 사용) | BSD-3-Clause | https://chromium.googlesource.com/libyuv/libyuv |
| libjpeg-turbo | libyuv가 사용 | IJG, BSD-3-Clause, zlib | https://libjpeg-turbo.org |

이 라이브러리가 들어간 실행 파일을 배포할 때는 각 라이브러리의 라이선스 전문을 함께 줘야 합니다. `tools\package.ps1`이 vcpkg가 설치한 라이선스 파일(`vcpkg_installed\…\share\<라이브러리>\copyright`)을 배포 zip의 `licenses` 폴더에 자동으로 넣습니다.

This software is based in part on the work of the Independent JPEG Group.
