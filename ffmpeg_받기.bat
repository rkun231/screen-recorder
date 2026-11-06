@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo.
echo  ffmpeg.exe(녹화 엔진)를 내려받는 중이에요. 약 200MB라서 1~3분 걸려요...
echo.
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'; [Net.ServicePointManager]::SecurityProtocol='Tls12'; $u='https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip'; $z=Join-Path $env:TEMP 'krec_ffmpeg.zip'; $d=Join-Path $env:TEMP 'krec_ffmpeg'; Invoke-WebRequest -Uri $u -OutFile $z -UseBasicParsing; if(Test-Path $d){Remove-Item $d -Recurse -Force}; Expand-Archive -Path $z -DestinationPath $d; $f=Get-ChildItem $d -Recurse -Filter ffmpeg.exe | Select-Object -First 1; Copy-Item $f.FullName (Join-Path (Get-Location) 'ffmpeg.exe') -Force; $l=Get-ChildItem $d -Recurse -Filter LICENSE.txt | Select-Object -First 1; if($l){Copy-Item $l.FullName (Join-Path (Get-Location) 'ffmpeg_LICENSE.txt') -Force}; Remove-Item $z -Force; Remove-Item $d -Recurse -Force"
echo.
if exist ffmpeg.exe (
  echo  완료! 이제 Recorder.exe를 실행하면 돼요.
) else (
  echo  내려받지 못했어요. 사용법.txt의 "직접 받는 방법"을 따라 주세요.
)
echo.
pause
