# FOURTOR FT26 EnergyMeter Logger

FT26 EnergyMeter Logger is an ESP32-C3 based vehicle energy-meter logger and
web viewer. It records the original FSK energy-meter compatible binary log
format to a microSD card, then transfers and analyzes the log through the FT26
viewer.

This repository contains both parts of the project:

- `firmware/`: ESP32-C3 PlatformIO firmware
- `viewer/`: Vue/Vite log analyzer and Web Serial device viewer

## Features

- 100 Hz binary logging
  - HV bus voltage
  - HV bus current
  - LV supply voltage
  - external temperature
  - RTC based timestamp in the original 32-byte header / 16-byte record format
- 20 second startup buffer policy
  - samples are buffered in RAM during startup
  - no SD log file is created if power is lost before the 20 second mark
  - after 20 seconds, a log file is created and the buffered records are written
- microSD storage with recovery behavior
  - regular sync while logging
  - if SD access fails after logging has started, measurement continues into RAM
  - after remount, recovery data is written to a new `.LOG1`, `.LOG2`, ... file
- Web Serial device control
  - connect at `921600 bps`
  - list log files
  - receive selected logs
  - save received `.log` files from the browser
  - sync RTC from the PC clock
  - delete selected source log files from the SD card
  - serial console with TX/RX event view and manual commands
- Python virtual device for PC-side viewer testing
  - exposes a serial port as if it were the hardware
  - maps a local folder as the device SD card
  - supports `HELLO`, `LIST`, `READ`/`REED`, `RTC`/`TIME`, and `DEL`

## Current Hardware Target

The firmware target is `esp32-c3-devkitm-1` with Arduino framework through
PlatformIO.

| Function | Pin |
| --- | --- |
| LED | GPIO3 |
| SD SCK | GPIO4 |
| SD MISO | GPIO5 |
| SD MOSI | GPIO6 |
| SD CS | GPIO7 |
| I2C SDA | GPIO8 |
| I2C SCL | GPIO9 |
| Power sense ADC | GPIO0 |
| UART RX | GPIO20 |
| UART TX | GPIO21 |

I2C devices:

- ADS1115: `0x48`
- DS3231M RTC: `0x68`

Serial device protocol runs at `921600 bps` over USB CDC and physical UART
only when the device boots into COM mode.

## Logging Behavior

On vehicle power-up, the firmware initializes hardware, runs HV zero
calibration, then starts the recorder.

The vehicle startup sequence is expected to take around 20 seconds before useful
driving data exists. Because power loss can shut the board down almost
immediately, FT26 intentionally does not create an SD file before the 20 second
startup delay has passed. This avoids creating partially written or corrupted
startup logs.

After the 20 second delay:

1. A new `.log` file is created on the SD card.
2. The RAM startup buffer is written to the file.
3. New 100 Hz records are queued and written in small storage batches.
4. The file is synced periodically.

If power is lost after the file has started, the firmware tries to flush and
sync the open file. This improves the chance of a valid log, but it cannot
guarantee a clean close if the board loses power instantly.

## Firmware Operation Flow

아래 트리는 전원 상태와 주변 장치 상태에 따라 펌웨어가 어느 루트로
진입하는지 빠르게 예상하기 위한 요약입니다.

```text
Boot
|- LED, Serial, UID, Power sense 초기화
|
|- LV/drive power 없음
|  |- 1초 동안 다시 확인
|  |- 계속 없음 -> COM mode
|  |  |- USB CDC / UART 921600 bps 명령 대기
|  |  |- HELLO, LIST, READ/REED, RTC/TIME, DEL 처리
|  |  `- Record mode로 자동 전환하지 않음
|  `- 전원 감지됨 -> Record boot 계속 진행
|
`- LV/drive power 있음
   |- I2C, RTC, ADS1115, microSD 초기화
   |- HV zero calibration 수행
   |
   |- calibration 실패 또는 전원 차단
   |  `- 기록 시작하지 않고 Fault/Power-off 상태
   |
   `- calibration 성공
      |- 100 Hz 측정 시작
      |- 처음 20초: SD 파일 생성 없이 RAM에 prebuffer 저장
      |
      |- 20초 전에 전원 차단
      |  `- SD 파일을 만들지 않고 RAM buffer 폐기
      |
      `- 20초 이후
         |- 새 .log 파일 생성
         |- prebuffer를 파일에 기록
         |- 이후 record는 RAM queue를 거쳐 SD에 batch 기록
         |
         |- SD write/sync 실패
         |  |- 측정은 RAM queue에 계속 보관
         |  |- 1초 간격으로 SD remount 시도
         |  `- 복구되면 기존 파일은 건드리지 않고 .LOG1/.LOG2 파일에 이어 기록
         |
         `- 전원 차단
            |- 가능한 경우 남은 buffer/queue flush 및 sync
            `- LED off
```

### LED Indicators

LED 오류 표시는 한 번 latch되면 일반 LED 모드보다 우선합니다. 전원 차단이
감지되면 오류 표시보다 우선해서 LED가 꺼집니다.

| LED pattern | Meaning | Typical route |
| --- | --- | --- |
| Solid on | 부팅 초기화 중 | `Boot` 직후 LED 초기화 완료 |
| Slow pulse: 1초 주기, 약 0.2초 켜짐 | 정상 대기/동작 | COM mode 대기 또는 Record mode buffering/recording |
| SD write toggle: SD 기록 직후 0.1초 간격 점멸 | SD 기록 활동 | prebuffer dump, record batch write, power-loss flush |
| Off | 꺼짐 또는 전원 차단 처리 | 전원 차단 감지 후 `powerFailOff()` |
| 1 pulse + 3초 pause 반복 | Power fault | 부팅 또는 기록 중 LV/drive power 없음 |
| 2 pulses + 3초 pause 반복 | RTC fault | DS3231 미검출, begin 실패, RTC lostPower |
| 3 pulses + 3초 pause 반복 | ADC fault | ADS1115 미검출, begin 실패, ADS read/calibration 실패 |
| 4 pulses + 3초 pause 반복 | SD fault | SD mount/open/write/sync 실패, prebuffer overflow |
| 5 pulses + 3초 pause 반복 | Range fault | HV voltage/current 측정값이 일정 시간 범위를 벗어남 |
| 6 pulses + 3초 pause 반복 | Unknown fault | 분류되지 않은 오류용 예약 표시 |

## Serial Protocol

All commands are ASCII lines ending in `\n`.

| Command | Response | Description |
| --- | --- | --- |
| `HELLO` | `OK HELLO <uid> <time> sd=<0/1> rtc=<0/1> files=<n> mode=<mode>` | Device identity and status |
| `LIST` | `OK LIST <n>` + file rows + `END` | List available SD log files |
| `READ <index>` | `OK READ <index> <bytes>`, then `CHUNK <offset> <size>` followed by exactly `<size>` raw binary bytes, repeated until `OK DONE <bytes>` | Transfer a binary log file |
| `REED <index>` | same as `READ` | Legacy compatibility spelling |
| `RTC yyyy-mm-dd-hh-mm-ss-ms` | `OK RTC` | Set RTC from viewer/PC time |
| `TIME yyyy-mm-dd-hh-mm-ss-ms` | `OK RTC` | Alias for `RTC` |
| `DEL <index>` | `OK DEL <index> <filename>` | Delete one selected source log from SD |

Unsupported commands return `ERR COMMAND`. `FORMAT` is intentionally not
implemented.

COM mode is selected only during boot when the LV/drive power condition indicates
that the logger should expose the serial device interface. If the firmware boots
into Record mode, it does not later switch back into COM mode; serial output is
used only for firmware log messages.

## Viewer

The viewer has two main areas:

- Log Analyzer: open a local FT26/FSK-compatible binary `.log` file, inspect
  graphs, metadata, and export analysis output.
- Device: connect to the hardware through Web Serial, receive logs, save the
  received file, sync RTC, delete selected SD logs, and inspect serial TX/RX
  events.

Web Serial requires a compatible Chromium-based browser such as Chrome or Edge.
The viewer is intended to run locally during development or as a built static
site/single HTML file.

### Viewer Development

```powershell
cd D:\FOURTOR\FT26_EnergyMeterLogger\viewer
npm install
npm run dev
```

The Vite development server defaults to the configured local port shown in the
terminal output.

Build commands:

```powershell
npm run build
npm run build:single
```

## Firmware Development

Install PlatformIO, then build from the firmware directory:

```powershell
cd D:\FOURTOR\FT26_EnergyMeterLogger\firmware
platformio run -e esp32-c3-devkitm-1
```

Native protocol tests:

```powershell
platformio test -e native
```

Upload and serial monitor use the normal PlatformIO workflow for the selected
ESP32-C3 board. The configured monitor speed is `921600`.

## Virtual Device

The Python virtual device is useful when the real hardware is not connected. It
opens a serial port and treats a local folder as the device SD card.

Example:

```powershell
cd D:\FOURTOR\FT26_EnergyMeterLogger\viewer
python tools\ft26_virtual_device.py --port COM2 --sd-dir samples
```

Dry-run without opening a port:

```powershell
python tools\ft26_virtual_device.py --sd-dir samples --dry-run
```

For browser testing on Windows, use a virtual serial port pair and connect the
viewer to the paired port.

## Important Notes

- FT26 no longer uses USB Mass Storage as the primary transfer path. Use the
  viewer Device tab and Web Serial while the device is booted in COM mode.
- Logs use the original binary compatibility contract. Do not change the
  32-byte header or 16-byte record layout unless the viewer/parser is updated
  together.
- RTC should be synchronized from the viewer before use if the RTC backup
  battery was removed or discharged.
- `DEL <index>` permanently removes the selected SD log file. There is no undo.
- `FORMAT` is not available by design.
- The firmware has been build-tested and protocol-tested in software. Physical
  validation is still required for final vehicle use, especially:
  - actual UART/USB CDC transfer reliability at `921600 bps`
  - SD removal/remount recovery
  - RTC hardware behavior
  - real power-loss behavior after the 20 second startup boundary

## Repository Status

This project is adapted from the original Formula Student Korea Electric Energy
Meter concept, but the hardware communication path has been changed for FT26.
The viewer keeps the original log-analysis purpose while adding a dedicated
device-control layer for serial file transfer.
