# Proxmark5 Companion Suite for Flipper Zero (`fmps_cxt.fap`)

> [!IMPORTANT]
> ### MrBoogus Personal Fork — Version 2.0 Full Suite Overview
> This repository transforms the basic Proxmark5 FAP companion into an enterprise-grade portable RFID/NFC lab for the Flipper Zero:
>
> 1. **Driver & Transport Foundation**:
>    - Expanded [pm3_cmd.h](lib/proxmark5_driver/pm3_cmd.h) with complete command opcodes for ISO14443A reader/sniffer/chkkeys/readsc, MIFARE Classic, ISO15693, iCLASS, LF EM410x, HID Prox, T55xx, and PM5 hardware control.
>    - Implemented `proxmark5_com_set_break_loop()` sending `0x55` sync poll bytes to cleanly break continuous PM5 operations without desynchronizing the bus.
>    - Added an ANSI-stripped `Dbprintf` ring buffer to capture and render Proxmark5 firmware debug logs directly on the Flipper OLED.
>
> 2. **Modular 10-Page Architecture**:
>    - **HF 14a & Magic Lab**: Magic card detection (`Gen1a`, `Gen1b`, `Gen2/CUID`, `Gen3 APDU`, `Gen4 GTU/GDM`, `Magic NTAG`, `SuperCard`), static nonce detection, native Flipper `.nfc` export, and Magic cloning.
>    - **MFC Key Auditor (1K)**: Hardware-accelerated 16-sector Key A/B dictionary attack with real-time visual matrix and full sector dump export.
>    - **14a Air Sniffer**: Passive FPGA capture with `BigBuf` trace download, protocol frame decoder (`R>` Reader / `T<` Tag), and raw trace saving.
>    - **HF 15693 / iCLASS**: Dual-mode scanner for ISO15693 8-byte UID and HID iCLASS / PicoPass CSN & e-Purse CC.
>    - **T5577 & EM4100 Master**: 7-mode workstation with EM4100 cloning + parity generator, block dumping, authenticated writes, SD dictionary cracker, Atmel test-mode recovery (`r0`..`r3`), and downlink password sniffer.
>    - **LF Multi-Cloner Lab**: High-voltage LF cloner for EM410x and HID Prox with Block 0 initialization and native `.rfid` saving.
>    - **LF Hitag2 Reader**: 32-bit transponder reader with NXP PCF transponder chip identification.
>    - **Live RF Field Scope**: 16.7 Hz real-time scrolling RF carrier oscilloscope with peak voltage tracking and load drop detection.
>    - **Antenna Tuner & Graph**: Graphical 85–250 kHz antenna resonance sweep with frequency divisor markers.
>    - **PM5 Hardware Control**: Interactive controls for smart antenna LF frequency banks, Q-factor toggle, coil LEDs, WS2812 RGB lighting, and power-save modes.
>
> *Upstream PR branch: `v2.0-full-suite` (contains only code patches, excluding this README note).*

---

A modular, high-performance companion application for the **Flipper Zero** that interfaces directly with the **Proxmark5** (AT32F437 + Gowin FPGA) via the **Companion Expansion Port (CEP)** over high-speed SPI/USART.

Version 2.0 leverages the Proxmark5's high-voltage RF drivers, FPGA downlink synthesis, and analog front-end, turning the Flipper Zero into a standalone portable field controller, visual analyzer, and SD card credential vault.

---

## Architecture Overview

```
┌─────────────────────────────────┐                 ┌──────────────────────────────────┐
│   Flipper Zero (Host Controller)│                 │   Proxmark5 (Hardware Engine)    │
│                                 │                 │                                  │
│  • ViewPort & Canvas GUI (128x64)│      CEP        │  • AT32F437 ARM Cortex-M4 (288MHz)│
│  • SD Card Keys/Dicts/Traces    │ ◄─────────────► │  • Gowin FPGA RF Demod & Synth   │
│  • Tag Sequence & State Machine │  SPI1 + USART1  │  • High-Voltage LF Driver (36V+) │
│  • Native .nfc & .rfid Exporter │                 │  • Fast MCU Demod & Raw BigBuf   │
└─────────────────────────────────┘                 └──────────────────────────────────┘
```

---

## Feature Matrix (10 Modular Pages)

| # | Page Name | Scope & Capabilities | Proxmark3 Command Interface |
| :-: | :--- | :--- | :--- |
| **0** | **HF 14a & Magic Lab** | ISO14443A reader, UID/BCC detection, Magic Card classifier (`Gen1a`, `Gen1b`, `Gen2/CUID`, `Gen3 APDU`, `Gen4 GTU/GDM`, `Magic NTAG21x`, `SuperCard`), Static Nonce PRNG testing, Magic cloner, and direct Flipper native `/ext/nfc/PM5_<UID>.nfc` exporter. | `0x0385`, `0x0607`, `0x0731` |
| **1** | **MFC Key Auditor (1K)** | Hardware-accelerated MIFARE Classic 1K dictionary attack across all 16 sectors (Key A & Key B) with an interactive 16-sector visual matrix (`A`/`B`/`*`/`.`/`x`) and complete sector dump export. | `0x0623`, `0x0621` |
| **2** | **14a Air Sniffer** | Passive FPGA air sniffer for ISO14443A reader-to-tag interactions with `BigBuf` trace download, protocol frame parser (`R>` Reader / `T<` Tag), and raw trace saving to SD card. | `0x0383`, `0x0207` |
| **3** | **HF 15693 / iCLASS** | Dual-mode high-frequency scanner for ISO15693 (NFC-V) 8-byte UID + IC manufacturer and HID iCLASS / PicoPass CSN and e-Purse configuration. | `0x0313`, `0x0394` |
| **4** | **T5577 & EM4100 Master** | Dedicated 7-mode T5577/EM4100 workstation: Universal EM4100 clone with parity synthesis, Block 0–7 reader/dump, password-authenticated writes, SD dictionary cracker (`/ext/apps_data/pm5/dicts/t5577_pass.txt`), Atmel transient test-mode unbricker (`r0`..`r3`), downlink password sniffer, and oscilloscope. | `0x0211`, `0x0212`, `0x0213`, `0x0217`, `0x0218` |
| **5** | **LF Multi-Cloner Lab** | High-voltage LF reader and cloner: EM410x watch/clone, HID Prox watch/clone, T5577 Block 0 reset to standard EM410x (`0x00148040`), and Flipper native `/ext/lfrfid/PM5_EM_<UID>.rfid` saving. | `0x021C`, `0x020B`, `0x0210`, `0x0215` |
| **6** | **LF Hitag2 Reader** | 32-bit Hitag2 transponder reader with hardware transponder chip identification (`PCF 7936`, `7946`, `7947`, `7942/44`, `7941`, `7952`, `7961`). | `0x0215` (`HT2F_UID_ONLY`), `0x0372` |
| **7** | **Live RF Field Scope** | Real-time ~16.7 Hz scrolling RF carrier envelope oscilloscope on the Flipper OLED for 13.56 MHz, 125 kHz, and 134.8 kHz with live peak tracking and percentage load modulation drop. | `0x0401`, `0x0402`, `0x0170` |
| **8** | **Antenna Tuner & Graph** | Full LF + HF antenna frequency sweep with tabular voltage readouts and a live graphical 85–250 kHz resonance curve plot with 125 kHz and 134.8 kHz divisor markers. | `0x0400` |
| **9** | **PM5 Hardware Control** | Interactive hardware controller for smart antenna LF frequency banks (`125`, `134`, `250`, `375`, `500 kHz`), Q-Factor toggle, coil LEDs, WS2812 RGB lighting, and MCU power-saving states. | `0x0170`, `0x0171`, `0x0178`, `0x0180` |

---

## Hardware Connection & Pinout

Connect the Proxmark5 to the Flipper Zero GPIO header via the CEP interface:

| Flipper Pin | Pin Name | Signal | PM5 CEP Header |
| :---: | :---: | :---: | :---: |
| **Pin 9** | `3V3` | 3.3V Logic Reference | `3V3` |
| **Pin 11** | `GND` | Ground | `GND` |
| **Pin 13** | `USART_TX` / `PA9` | USART TX (Handshake) | `USART_RX` |
| **Pin 14** | `USART_RX` / `PA10` | USART RX (Handshake) | `USART_TX` |
| **Pin 2** | `SPI_SCK` / `PB13` | SPI1 Clock | `SCK` |
| **Pin 3** | `SPI_MISO` / `PB14`| SPI1 Host In / Slave Out | `MISO` |
| **Pin 4** | `SPI_MOSI` / `PB15`| SPI1 Host Out / Slave In | `MOSI` |
| **Pin 5** | `SPI_CS` / `PB12`  | SPI1 Chip Select | `NSS` |

---

## Building & Installation

Requirements: [uFBT (micro Flipper Build Tool)](https://github.com/flipperdevices/ufbt).

```bash
# Clone the repository
git clone https://github.com/MrBoogus/Proxmark5_FlipperZero_FAP.git
cd Proxmark5_FlipperZero_FAP

# Build application
ufbt

# Install directly to a USB-connected Flipper Zero
ufbt launch
```

The resulting binary [fmps_cxt.fap](dist/fmps_cxt.fap) will be installed under **Apps → RFID → Proxmark5 CXT**.

---

## Upstream Preparation & Patch Format

All patches have been structured as clean, atomic git commits ready for upstream PR submission:

1. `0001-driver-add-PM3-command-definitions-break-loop-contro.patch`: Driver layer opcodes, break-loop control, and `Dbprintf` ring buffer.
2. `0002-core-bump-to-v2.0-modernize-main-menu-navigation-and.patch`: Application manifest bump to v2.0, categorized navigation, and 4-view status dashboard.
3. `0003-hf-add-Magic-card-detection-MFC-auditor-air-sniffer-.patch`: HF 14a Magic card detection, MFC 1K key auditor, FPGA air sniffer, and ISO15693/iClass.
4. `0004-lf-add-T5577-master-suite-LF-cloner-lab-Hitag2-decod.patch`: LF T5577 suite, cloner lab, Hitag2 chip decoder, RF oscilloscope, antenna tuner, and hardware control.
