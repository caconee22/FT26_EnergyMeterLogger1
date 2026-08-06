# FT26 EnergyMeter Logger Firmware Handoff Prompt

이 문서는 다음 AI 또는 다음 작업자가 `D:\FOURTOR\FT26_EnergyMeterLogger` 프로젝트를 이어받을 때 먼저 읽어야 하는 인수인계 프롬프트입니다.

## 기본 응답/작업 규칙

- 사용자가 영어로 말해도 항상 한국어로 답합니다.
- 작업 위치는 기본적으로 `D:\FOURTOR\FT26_EnergyMeterLogger`입니다.
- 펌웨어 작업 위치는 `D:\FOURTOR\FT26_EnergyMeterLogger\firmware`입니다.
- 실제 하드웨어 KiCad 프로젝트는 `D:\FOURTOR\FT26_E_LOG`입니다.
- 원본 STM32 펌웨어 참고 위치는 `C:\Users\CACONEE\Desktop\fsk-energymeter-main\device\firmware`입니다.
- 사용자가 명시적으로 요청하기 전에는 commit/push 하지 않습니다.
- 사용자가 "푸시해", "커밋해"라고 명시하면 그때만 커밋/푸시합니다.
- 코드에 함수나 구조체 같은 선언을 추가할 때는 간단한 한국어 주석을 답니다.
- 원본 로그 뷰어와 호환되어야 하므로 binary log format은 함부로 바꾸지 않습니다.

## 현재 Git 상태

- 현재 브랜치: `codex/firmware-1`
- 원격 브랜치: `origin/codex/firmware-1`
- 마지막 확인 시 작업트리는 clean 상태였습니다.
- 최근 커밋:
  - `8ebcd7f Add nonblocking measurement recorder`
  - `1736f54 Add firmware hardware startup and sensor calculations`
  - `d11efaf Add compatible binary log format`
  - `6c33df1 Add FT26 firmware pin settings`
  - `1e147b8 Add PlatformIO firmware scaffold`

## 프로젝트 목표

ESP32-C3 기반 FT26 EnergyMeter Logger 펌웨어를 작성합니다. 원본 STM32 펌웨어의 파일 포맷과 로그 뷰어 호환성을 최대한 유지하되, 이번 하드웨어의 회로와 안전 요구사항에 맞춰 새로 구현합니다.

USB 연결/USB Mass Storage 기능은 버립니다. 이 제품은 SD 카드에 binary log를 기록하고, 이후 기존 로그 뷰어 프로그램에서 같은 형식으로 읽히는 것이 목표입니다.

## 현재 구현된 주요 기능

### 부팅/초기화

파일:
- `firmware/src/main.cpp`
- `firmware/src/boot.cpp`
- `firmware/include/boot.h`

동작:
- `setup()`에서 `ft26::boot::initializeHardware()` 실행
- LED 초기화
- Serial 115200 시작
- ESP32 MAC 주소를 읽어 원본 로그 헤더의 96비트 UID 영역에 저장
- 내장 ADC GPIO0으로 전원 감시 초기화 및 1회 읽기
- I2C 시작: SDA GPIO8, SCL GPIO9, 400kHz, timeout 5ms
- RTC DS3231M 주소 확인 및 시작
- ADS1115 주소 확인 및 시작
- SD 카드 마운트
- 하드웨어 준비 상태를 `HardwareStatus`에 저장

### 상태 LED

파일:
- `firmware/src/status_led.cpp`
- `firmware/include/status_led.h`

정책:
- 부팅 중: LED solid on
- 정상 RAM buffering: 1초 주기, 0.2초 on slow pulse
- SD write 때: write 이벤트마다 LED toggle
- power fail 때: LED off
- fault latch 이후 일반 LED 명령은 무시

Fault group:
- Power = 1 pulse
- RTC = 2 pulses
- ADC = 3 pulses
- SD = 4 pulses
- Range = 5 pulses
- Unknown = 6 pulses

Fault pattern:
- 0.2초 on / 0.2초 off를 fault 번호만큼 반복
- 이후 3초 off
- 이 패턴 반복

### 원본 호환 로그 포맷

파일:
- `firmware/include/log_format.h`
- `firmware/src/log_format.cpp`

원본 호환 구조:
- Header: 32 bytes
- Record/Event log packet: 16 bytes
- Magic: `0xAA`
- Header type: 0
- Record type: 1
- Event type: 2
- checksum: 16비트 word XOR, checksum field를 0으로 만든 뒤 계산
- little endian 구조체 packing 유지

Record payload:
- HV voltage: int16, 0.1V
- HV current: int16, 0.1A
- LV voltage: int16, 0.01V
- temperature: int16, 0.01도C

파일명 규칙은 원본과 같은 형태입니다:

```text
20YY-MM-DD-HH-MM-SS-ms UID0-UID1-UID2.log
```

현재 Arduino SD API에서는 root 경로 `/`가 붙습니다.

### 측정/기록 루프

파일:
- `firmware/include/recorder.h`
- `firmware/src/recorder.cpp`
- `firmware/include/storage.h`
- `firmware/src/storage.cpp`
- `firmware/include/sensors.h`
- `firmware/src/sensors.cpp`

현재 동작:
- `setup()`에서 `ft26::recorder::begin()` 호출
- `loop()`에서 계속 `ft26::recorder::tick()` 호출
- tick 맨 앞에서 내장 ADC 전원 감시
- 전원 차단 감지 시:
  - ADS 읽기 중단
  - 파일이 열려 있으면 남은 batch write, flush, close
  - LED off
- ADS1115는 blocking read를 쓰지 않고 non-blocking 상태머신으로 동작
- ADS 변환 시작 후 최소 1200us 전에는 완료 확인하지 않음
- 5000us 이상 완료되지 않으면 ADC fault 처리
- 10ms마다 record 생성
- 20초 전까지 파일을 만들지 않고 RAM prebuffer에 저장
- 20초 이후 파일 생성, header write, prebuffer write
- 이후 100ms 단위로 10개 record batch write 및 flush

현재 sampling 정책:
- 내장 ADC 전원 감시: 매 tick
- HV voltage: ADS scheduler에서 빠르게 반복 측정
- HV current: ADS scheduler에서 빠르게 반복 측정
- LV voltage: 100ms마다 측정
- Temperature: 100ms마다 측정

ADS task 선택 우선순위:
1. LV가 100ms 이상 갱신되지 않았으면 LV
2. Temperature가 100ms 이상 갱신되지 않았으면 Temperature
3. 나머지는 HV voltage / HV current 번갈아 측정

Record 생성 시점에는 ADS를 기다리지 않습니다. 최신 계산값만 넣습니다.

## 핀 배치

파일: `firmware/include/config.h`

- LED: GPIO3
- SD SCK: GPIO4
- SD MISO: GPIO5
- SD MOSI: GPIO6
- SD CS: GPIO7
- I2C SDA: GPIO8
- I2C SCL: GPIO9
- Power sense internal ADC: GPIO0
- UART RX: GPIO20
- UART TX: GPIO21
- DS3231M I2C address: `0x68`
- ADS1115 I2C address: `0x48`

ADS1115 channels:
- AIN0: HV voltage
- AIN1: HV current
- AIN2: LV voltage
- AIN3: external temperature

## 하드웨어/안전 요구사항

이번 하드웨어에는 백업 슈퍼커패시터가 있습니다. 전원 차단 후 3.3V MCU 라인은 몇 초 살아 있을 수 있지만, 5V 라인은 바로 죽을 수 있습니다.

중요:
- 전원 기준점은 ADS1115나 5V 센서 쪽으로 잡으면 안 됩니다.
- 5V가 죽으면 ADS1115, level shifter, 센서 출력이 깨질 수 있습니다.
- 전원 차단 감지는 ESP32-C3 내장 ADC GPIO0 `/2.5V_ADC`로 합니다.
- 전원 차단이 감지되면 즉시 파일을 마무리하고 ADS/I2C 의존 작업을 피해야 합니다.

초기 기록 정책:
- 부팅 후 약 1초 동안 초기화/교정 예정
- 부팅 직후 파일을 만들지 않습니다.
- 20초 전까지 record를 RAM에 쌓습니다.
- 20초 이후부터 SD 파일을 만들고, 그동안 쌓은 20초치도 저장합니다.
- 20초 전에 전원이 꺼지면 파일 자체가 없으므로 깨진 파일을 만들지 않는 것이 목표입니다.

## 계산식/센서 상수

파일:
- `firmware/include/sensors.h`
- `firmware/src/sensors.cpp`

### ADS1115

현재 설정:
- Gain: `GAIN_TWOTHIRDS`
- Data rate: `RATE_ADS1115_860SPS`
- 1 count = 187.5uV
- 코드 상수: `ADS1115_GAIN_TWOTHIRDS_UV_PER_COUNT_X10 = 1875`

### HV current

센서:
- QNHCK1-21 400A open loop Hall sensor
- 공급 5V
- 출력 중앙값 2.5V
- 0.5V = -400A
- 2.5V = 0A
- 4.5V = +400A
- 감도 = 5mV/A

현재 식:
- `current_A = (sensor_uV - zero_offset_uV) / 5000`
- log value = 0.1A 단위

Fault:
- sensor output <= 0.4V 또는 >= 4.6V 상태가 3초 이상 유지되면 range fault

### HV voltage

회로 결론:
- `HV:ADC = 150.29:1`
- `HV = ADC_V * 150.29`

Fault:
- ADC > 2.0V 또는 계산 HV > 300.0V 상태가 1초 이상 유지되면 range fault

### LV voltage

회로 결론:
- `VBUS:ADC = 5.7:1`
- `VBUS = ADC_V * 5.7`

현재 LV는 100ms마다 측정하고, 10ms record에는 마지막 값을 재사용합니다.

### Temperature

센서:
- NTC 100k
- R0 = 100k
- T0 = 25도C = 298.15K
- Beta = 3950
- 기준 분압 공급 전압 기본값 = 5V

회로 식:
- `R_NTC = R_FIXED * (supply_uV - adc_uV) / adc_uV`
- `T_K = 1 / (1/T0 + ln(R_NTC/R0)/BETA)`
- `T_C = T_K - 273.15`

함수:
- `calculateTemperature(raw)`는 기본 5V 기준
- `calculateTemperature(raw, supply_uv)`는 나중에 실제 ADC/분압 공급 전압을 넣어 보정 가능

Temperature range fault:
- ADC <= 1.2V 또는 ADC >= 4.8V이면 range fault

## 원본 STM32 펌웨어에서 계속 참고할 것

원본 위치:
- `C:\Users\CACONEE\Desktop\fsk-energymeter-main\device\firmware`

특히 참고할 파일:
- `Core\Inc\energymeter.h`
- `Core\Src\energymeter.c`
- `Core\Src\energymeter_record.c`

원본 주요 흐름:
- HAL/clock/GPIO/DMA/ADC/RTC/TIM/SDIO/FATFS/UART 초기화
- `energymeter_init()`
- RTC read
- header 생성
- 약 270ms wait
- 16-sample zero calibration
- LV 전압 판단으로 USB/record 모드 선택
- USB 모드는 이번 프로젝트에서 버림
- record 모드는 SD mount/open/header write 후 TIM5 10ms timer로 100Hz 기록
- 원본은 16바이트 record를 계속 쓰고, sync flag에서 `f_sync()` 호출

원본 파일명:

```c
sprintf(filename, "20%02d-%02d-%02d-%02d-%02d-%02d-%03d %08lX-%08lX-%08lX.log",
        header.year, header.month, header.day,
        header.hour, header.minute, header.second, header.millisecond,
        header.uid[0], header.uid[1], header.uid[2]);
```

## 다음 작업자가 먼저 확인해야 할 파일

우선순위 순서:

1. `firmware/src/main.cpp`
   - 전체 진입점 확인
2. `firmware/src/boot.cpp`
   - 부팅 초기화 순서, 에러 처리, 전원 감시 초기값 확인
3. `firmware/src/recorder.cpp`
   - 현재 핵심 로직
   - ADS non-blocking scheduler
   - 10ms record 생성
   - 20초 prebuffer
   - SD write/flush/close
4. `firmware/src/sensors.cpp`
   - 계산식, ADS wrapper
5. `firmware/src/storage.cpp`
   - 파일명, header write, batch write
6. `firmware/src/status_led.cpp`
   - LED fault latch 동작
7. `firmware/include/config.h`
   - 핀/주기/주소/상수
8. `firmware/include/log_format.h`
   - 원본 호환 포맷. 함부로 바꾸면 안 됨.

## 현재 코드의 주의점과 아직 남은 일

현재 구현은 빌드 성공했지만 실제 하드웨어 장시간 검증 전입니다.

주의할 점:
- `storage::openLogFile()`은 `FILE_APPEND`를 사용합니다. 같은 이름 파일이 있으면 뒤에 append될 수 있습니다. 원본도 append를 사용하지만, 중복 파일명 상황을 나중에 검토해야 합니다.
- `FILE_SYNC_INTERVAL_MS`가 현재 100ms입니다. SD flush 부담이 크면 원본 주석처럼 1초 sync로 완화할 수 있습니다.
- ADS scheduler는 non-blocking이지만 `conversionComplete()`와 `readAdsLastRaw()` 자체는 짧은 I2C transaction을 수행합니다.
- `tick()`가 SD flush/write에 걸리면 10ms record 시점이 밀릴 수 있습니다. 현재는 밀리면 다음 tick을 재정렬하는 방식입니다.
- HV current zero calibration은 아직 실제 1초 평균 교정으로 구현되지 않았고, 현재 기본 2.5V 상수입니다.
- Header의 `v_cal`, `c_cal` 값은 아직 임시입니다. `c_cal`은 현재 `current_zero_uv / 1000` 형태로 들어갑니다.
- 전원 차단 감지 threshold는 `POWER_SENSE_PRESENT_MV_MIN = 1800`입니다. 실제 회로에서 조정 필요할 수 있습니다.
- range fault는 LED latch로 표시하지만, 상세 에러 이벤트 record 저장은 아직 구현되지 않았습니다.
- RTC lostPower 상태에서 시간 세팅/복구 정책은 아직 없음.
- 실제 하드웨어에서 ADS1115 860SPS + 400kHz I2C + SD write 타이밍을 시리얼 로그 또는 GPIO toggle로 측정해야 합니다.

## 빌드/검증 명령

펌웨어 빌드:

```powershell
cd D:\FOURTOR\FT26_EnergyMeterLogger\firmware
platformio run
```

시리얼 모니터 기본 속도:

```text
115200
```

현재 마지막 빌드 확인:
- `platformio run` 성공
- RAM 약 14.4%
- Flash 약 26.3%

## 다음 추천 작업

1. 실제 하드웨어에서 부팅 로그 확인
2. SD 카드 없이 부팅 시 SD fault LED 패턴 확인
3. ADS1115 없을 때 ADC fault LED 패턴 확인
4. RTC 없을 때 RTC fault LED 패턴 확인
5. 정상 연결 상태에서 20초 후 파일 생성 확인
6. 생성된 `.log` 파일을 기존 로그 뷰어에서 열어 포맷 호환 확인
7. GPIO toggle 또는 `micros()` 통계로 10ms record jitter 측정
8. HV current 1초 zero calibration 구현
9. error event record 저장 규칙 구현
10. power fail 순간 파일 close 실제 검증

## 다음 AI에게 줄 짧은 시작 프롬프트

```text
너는 FT26 EnergyMeter Logger 펌웨어 작업을 이어받는다.
반드시 D:\FOURTOR\FT26_EnergyMeterLogger\NEXT_AI_HANDOFF_PROMPT.md 를 먼저 읽고,
그 다음 firmware/src/main.cpp, firmware/src/boot.cpp, firmware/src/recorder.cpp,
firmware/src/sensors.cpp, firmware/src/storage.cpp, firmware/include/config.h,
firmware/include/log_format.h 를 확인해라.
원본 STM32 펌웨어는 C:\Users\CACONEE\Desktop\fsk-energymeter-main\device\firmware 에 있다.
원본 binary log format과 파일명 규칙은 반드시 호환 유지한다.
사용자가 명시하기 전에는 commit/push 하지 마라.
모든 답변은 한국어로 한다.
```
