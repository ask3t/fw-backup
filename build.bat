@echo off
rem Сборка fw-backup.exe (нужен g++ из MinGW-w64 в PATH)
g++ -O2 -s -mwindows fw-backup.cpp -o fw-backup.exe -lshlwapi -static -static-libgcc -static-libstdc++
if %errorlevel%==0 (
  echo OK: fw-backup.exe
) else (
  echo Ошибка сборки
  exit /b 1
)
