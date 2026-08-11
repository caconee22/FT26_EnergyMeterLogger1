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
- Log analyzer export and review tools
  - open `.log`, `.json`, and `.csv` files
  - export JSON, CSV, and graph images
  - switch between 80 kW and 10 kW power-limit checks
  - reverse current polarity for review when the source log direction is inverted
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

## Technical Architecture

FT26 is split into a small firmware runtime that owns the hardware and a local
viewer that owns transfer, parsing, and review. The firmware keeps the original
binary log contract stable; the viewer treats the device as a serial file server
when the logger is in COM mode.

```text
Vehicle power / sensors
        |
        v
ESP32-C3 firmware
  |- Power sense: GPIO0
  |- ADS1115: HV voltage, HV current, LV voltage, temperature
  |- DS3231M: boot timestamp / RTC sync target
  |- microSD: original-compatible binary logs
  `- USB CDC / UART: COM protocol at 921600 bps
        |
        v
FT26 viewer
  |- Device: list, receive, save, RTC sync, delete
  `- Log Analyzer: parse, graph, metadata, export
```

### Firmware Modules

| Module | Responsibility |
| --- | --- |
| `main.cpp` | Entry point. Selects COM mode or starts calibration and recording. |
| `boot.cpp` | Initializes LED, Serial, UID, power sense, I2C, RTC, ADS1115, and SD. Selects Record/COM mode. |
| `calibration.cpp` | Performs HV zero calibration before the recorder starts. |
| `ads_scheduler.cpp` | Runs non-blocking ADS1115 conversions and keeps latest channel readings. |
| `measurements.cpp` | Converts raw ADC values into log units and range-status flags. |
| `recorder.cpp` | Owns 100 Hz capture, 20 second prebuffer, SD writer queue, power-loss flush, and SD recovery. |
| `storage.cpp` | Mounts microSD, creates `.log` / `.LOGn` files, writes headers and records, and syncs files. |
| `com_mode.cpp` | Handles USB CDC and UART commands: `HELLO`, `LIST`, `READ`/`REED`, `RTC`/`TIME`, and `DEL`. |
| `com_protocol.cpp` | Validates indexed commands, RTC timestamps, and log-file ordering. |
| `log_format.cpp` | Generates the 32-byte header and 16-byte record/event packets with checksum. |
| `status_led.cpp` | Owns normal LED modes, SD-write activity blink, latched fault pulses, and power-fail off. |

### Runtime States

| State | Entry condition | Main behavior | Next route |
| --- | --- | --- | --- |
| Boot | Reset or power-up | Initialize basic hardware and read power sense | COM mode or Record boot |
| COM mode | LV/drive power remains absent for 1 second | Wait for serial commands over USB CDC/UART | Reset required to enter Record mode |
| Record boot | LV/drive power is present | Initialize I2C, RTC, ADS1115, SD | Calibration |
| Calibration | Record boot completed | Wait briefly, sample HV zero points | Buffering or Fault |
| Buffering | Calibration success | Capture 100 Hz records into RAM without creating an SD file | FileLogging after 20 seconds |
| FileLogging | 20 second delay elapsed | Create log file, write prebuffer, batch-write new records, sync periodically | SD Recovery, PowerFail, or Fault |
| SD Recovery | SD open/write/sync failure | Keep records in RAM queue while retrying SD remount | New `.LOGn` file when SD returns |
| PowerFail | Power sense confirms input loss | Flush and sync if a file has started, then turn LED off | Shutdown/reset |
| Fault | Hardware, SD, ADC, RTC, or range error | Latch the first fault group on LED; some recovery work may continue | Manual reset or power cycle |

### Data Path

| Stage | Firmware behavior | Output |
| --- | --- | --- |
| Sample | ADS scheduler updates fast HV channels and slower LV/temperature channels. | Latest validated readings |
| Record | Recorder emits one original-compatible record every `10 ms` when fresh readings are available. | 16-byte `LOG_TYPE_RECORD` |
| Prebuffer | First 20 seconds are held in RAM to avoid early corrupted files. | RAM prebuffer |
| File start | Header is written after the startup boundary. | 32-byte `LOG_TYPE_HEADER` |
| Storage | Prebuffer and queued records are written in small batches. | `.log` file on microSD |
| Recovery | On SD failure, existing files are not renamed or appended blindly. | New `.LOG1`, `.LOG2`, ... file |
| Transfer | Viewer requests a file by index and receives chunked binary data. | Saved `.log` on PC |
| Analysis | Viewer parses the original binary format and calculates graph metadata. | Graphs, JSON, CSV, image export |

### Error Handling Policy

| Error class | Detection point | Firmware action | LED indication |
| --- | --- | --- | --- |
| Power loss | GPIO0 power sense below threshold for confirmation window | Stop normal capture, flush/sync if possible, close file | Off |
| RTC fault | DS3231 missing, `begin()` failure, or `lostPower()` | Mark RTC invalid; header time may fall back to default | 2 pulses |
| ADC fault | ADS1115 missing, conversion timeout, or read failure | Stop valid measurement path and latch ADC fault | 3 pulses |
| SD fault | Mount, open, write, sync, or prebuffer overflow failure | Enter recovery, queue records in RAM, retry remount | 4 pulses |
| Range fault | HV/current/temperature range held beyond configured time | Keep recording but latch range fault | 5 pulses |
| Unknown fault | Reserved for unclassified future errors | Reserved behavior | 6 pulses |

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
         |  |- RAM queue 용량 안에서 측정 record 보관
         |  |- queue가 오래 가득 차면 오래된 record부터 drop 가능
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

### Firmware Timing Summary

| Item | Value | Meaning |
| --- | --- | --- |
| COM mode power wait | `1000 ms` | 부팅 직후 LV/drive power가 없을 때 1초 더 확인 |
| Power-loss confirm | `10 ms` | 순간 노이즈가 아니라 전원 차단인지 확인하는 시간 |
| HV zero calibration wait | `300 ms` | calibration sample을 잡기 전 안정화 대기 |
| Record interval | `10 ms` | 100 Hz log record 생성 주기 |
| Slow ADS channel interval | `100 ms` | LV voltage와 temperature 갱신 목표 주기 |
| File start delay | `20000 ms` | 부팅 후 20초 동안 SD 파일 없이 RAM prebuffer 사용 |
| SD sync interval | `500 ms` | queue가 비었을 때 열린 log file flush 주기 |
| SD remount retry | `1000 ms` | SD 오류 후 remount 재시도 간격 |
| Storage queue capacity | `1000 records` | SD 장애 중 RAM queue에 유지할 수 있는 최대 record 수 |

### Log File Naming

Normal log files use the original-compatible session name:

```text
20YY-MM-DD-HH-MM-SS-ms UID0-UID1-UID2.log
```

If a file with the same session name already exists, the firmware creates the
next numbered file, such as `.LOG1`, `.LOG2`, and so on. During SD recovery,
the existing file is closed and left untouched; recovered queued data is written
to a newly numbered `.LOGn` file.

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

Command availability:

| Command | Record mode | COM mode / recorder inactive |
| --- | --- | --- |
| `HELLO` | allowed for status | allowed |
| `LIST` | allowed if SD is available | allowed |
| `READ` / `REED` | blocked with `ERR BUSY RECORDING` | allowed |
| `DEL` | blocked with `ERR BUSY RECORDING` | allowed |
| `RTC` / `TIME` | blocked with `ERR BUSY RECORDING` | allowed if RTC is present |

COM mode is selected only during boot when the LV/drive power condition indicates
that the logger should expose the serial device interface. If the firmware boots
into Record mode, it does not later switch back into COM mode; serial output is
used only for firmware log messages.

## Viewer

The viewer has two main areas:

- Log Analyzer: open a local FT26/FSK-compatible binary `.log` file, inspect
  graphs, metadata, power-limit violations, and export analysis output.
- Device: connect to the hardware through Web Serial, receive logs, save the
  received file, sync RTC, delete selected SD logs, and inspect serial TX/RX
  events.

The Log Analyzer also accepts exported `.json` and `.csv` files, can export the
current graph image, can toggle the power-limit check between 80 kW and 10 kW,
and can reverse current polarity for review. A log received from the Device tab
is automatically loaded into the analyzer.

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

`DEL <index>` in the virtual device deletes the selected file from the mapped
folder. The `--delete-enabled` option is kept only as a compatibility flag and
does not act as a safety lock.

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

## License

This project follows the dual-license model used by the upstream Formula Student
Korea Electric Energy Meter project:

- Non-commercial use: Beer-Ware License, Revision 42, with the original upstream
  notice retained.
- Commercial use: separate commercial permission is required from the applicable
  rights holders. For upstream-derived portions, follow the original project
  notice and contact `mail@luftaquila.io` for commercial licensing options. For
  FT26-specific modifications and integration work, obtain separate written
  permission from the FT26 EnergyMeter Logger rights holder as well.

See [LICENSE](LICENSE) and [viewer/NOTICE.md](viewer/NOTICE.md) for details.

## Repository Status

This project is adapted from the original Formula Student Korea Electric Energy
Meter concept, but the hardware communication path has been changed for FT26.
The viewer keeps the original log-analysis purpose while adding a dedicated
device-control layer for serial file transfer.
