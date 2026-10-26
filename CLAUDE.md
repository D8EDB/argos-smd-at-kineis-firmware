# Handover: own Argos A2 / A2-legacy / VLDA4 transmit stack for STM32WL55

## Goal

Replace the closed Kinéis libraries in `arribada/argos-smd-at-kineis-firmware` with our own C source for three uplink formats only: **LDA2**, **LDA2L** (A2 legacy, 28-bit ID) and **VLDA4**. Work in VS Code with arm-none-eabi-gcc. Keep the result bit-exact and RF-equivalent to the stock libraries, proven by tests, not by inspection.

Two closed archives are involved, in `Kineis/Lib/` (stack v10.0.1):

- `libknsrf_wl.a` is the RF layer: `kns_rf.o`, `aks_l1.o`, `sx126x_stm32wl.o`, `sx126x_stm32wl_board.o`
- `libkineis.a` is the MAC and frame formatting: `kns_ff.o` (encoders), `kns_mac_*.o`, `kns_tx*.o`, `kns_cfg.o`, `kns_utils.o`

Neither carries a licence (`SPDX-License-Identifier: no SPDX license`). Check the Kinéis/CLS terms before distributing a replacement. Do all RF testing into a dummy load or shielded enclosure unless an authorised ID and radio configuration are in use.

## Start here

1. `git clone https://github.com/arribada/argos-smd-at-kineis-firmware`
2. Apply `kns_a2_vld_only.patch` (`git am`). It adds `KNS_MODS = A2_VLD` to the Makefile, wraps out the LDK encoder and compiles out LDK paths in the app. It has **not been built yet**, because the previous session had no cross-compiler.
3. Build twice, with `KNS_MODS=ALL` and with the default, and diff the `.text` totals in `build/*.map`. Record both sizes as the baseline for everything below.

## What is already known

### Physical layer (from `stm32wl55_modulation_discovery.md` in the project)

- The SUBGHZ radio has an undocumented 4-point I/Q constellation mapper.
  - Enable with bit 0 of reg `0x0680` (read-modify-write, `(v & 0xFC) | 0x01`).
  - Load filter reg `0x029F` with `{0x01, 0x06, 0x80}`.
  - Load the table at reg `0x0681` with 8 signed bytes, magnitude 64:
    - point 0 = phase −Φ
    - point 1 = phase +Φ
    - point 2 = (+64, 0), pure carrier
    - point 3 = (0, 0)
- For Φ = 1.100 rad the table is `{0x1D,0xC7,0x1D,0x39,0x40,0x00,0x00,0x00}`.
- Each chip is 2 bits in the FIFO, so 800 chip/s needs a 1600 bps setting. The divider is `BR_DIV = 32·32 MHz / 1600 = 0x09C400`.
- `SetModulationParams` pulse shaping must be `0x00` (off), **not** `0x16`. Otherwise the envelope collapses at transitions.

### Frame encoders (`kns_ff.o`, built with `-ffunction-sections`)

| Function | .text (B) | Depends on |
|---|---|---|
| `KNS_FF_encodeA2legacy` | 340 | `KNS_calcCrcBch32`, `offset` |
| `KNS_FF_encodeA2` | 580 | `getAuthent` (318), `getEncryptedData.constprop.0` (432), `nvm_management` (44), `KNS_calcCrcBch32` (126) |
| `KNS_FF_encodeVLDA4` | 888 | same as A2, plus `VLDA4_encodedPacket` (384) and `vlda4_interleaving_table` (45 B rodata) |
| `KNS_FF_encodeLDK` | 576 | not needed |
| `KNS_FF_encodeHDA4` | 1096 | not needed, and already unreferenced |

`kns_cfg.o` holds `radioConfKey` (16 B), `radioConfIv` (16 B) and `radioConfVersion` (2 B). The 16-byte radio configuration blob in `mcu_nvm.c` is decrypted with these in `KNS_CFG_getRadioInfo` (120 B), which also uses `KNS_calcCrcBch32`. AES source is already open in `Kineis/Extdep/Mcu/Src/aes.c` and `mcu_aes.c`.

### Encoder calling contract

This comes from the disassembly of `KNS_MAC_TXcmn_buildFrm` in `kns_mac_ll_tx_common.o`. The function dispatches with a `tbb` on `modulation − 2`.

- `r0` = user data pointer
- `r1` = user data bit length (u16)
- `r2` = output frame buffer
- `r3` = output bit-length pointer
- `[sp]` = service-flags byte
- `[sp+4]` = 0. The A2-legacy call passes no stack arguments.
- Return 0 means OK. Any non-zero return makes the MAC free the FIFO slot and return −1.

### RF layer API to preserve (`Kineis/Lib/kns_rf.h`)

`KNS_RFTX_powerOn`, `KNS_RFTX_powerOff`, `KNS_RFTX_setCfg`, `KNS_RFTX_tcxoWarmup`, `KNS_RFTX_pushBitstream`, `KNS_RFTX_startImmediate` and `KNS_RFTX_abortRf`, with completion through the `eop_isr_cb(struct KNS_RF_evt_t *)` callback.

- The RX functions (`KNS_RFRX_*`) exist in the header, but this is a TX-only build.
- Internally:
  - `kns_rf.o` keeps a 128-byte bitstream buffer.
  - `aks_l1.o` does the work: `Aks_L1_Set_Rf_Config` (512 B, handles every modulation in one function), `Aks_L1_Send_Bitsream` (760 B) and `Aks_L1_Test_Start_Tx_Cw`.
  - Its private rodata is `aksL1DataPattern` (16 B) and `aksL1NoBitPattern` (4 B).

## Open questions, to answer from the binaries and captures

1. The exact SUBGHZ command and register sequence per modulation: packet type, packet params, PA config, ramp time, TCXO voltage and delay, preamble/carrier duration, and IRQ setup.
2. How `Aks_L1_Send_Bitsream` maps frame bits to 2-bit chips (Manchester → constellation index), and how it streams more than 255 bytes, if it ever does.
3. VLDA4 physical layer: chip rate, Φ, and whether it uses the same mapper. Do not assume it matches A2.
4. The A2 / VLDA4 codec internals: the cipher mode in `getEncryptedData`, what `getAuthent` computes, the message-counter handling in `nvm_management`, the BCH32 polynomial, and the VLDA4 convolutional code plus interleaver.
5. What `offset` is, a global used by most encoders.

## Plan

### Phase 1: instrument the stock libraries (cheapest route to ground truth)

- **Radio command log.** Link the stock firmware with `-Wl,--wrap=` on `SX126xStm32wl_LL_WriteCommand`, `SX126xStm32wl_LL_WriteRegisters`, `SX126xStm32wl_LL_WriteRegister` and `SX126xStm32wl_LL_WriteBuffer`.
  - Log opcode, address and bytes to UART, then call `__real_*`.
  - Do this for one transmission per modulation, using `AT+CW=2/3/4` and a real `AT+TX`.
  - This answers questions 1 and 2 without reverse-engineering `aks_l1.o` line by line.
- **Golden encoder vectors.**
  - Build a test image, on target or under `qemu-arm` with semihosting, that links `kns_ff.o` and `kns_utils.o` from the archive.
  - Call each encoder over a fixed set of payloads, bit lengths, IDs and message counters, and dump the outputs to files.
  - These become the regression vectors for the replacement.
- **RF golden captures.** Use the Airspy Mini at 3.0 MSPS, centred on the burst. Capture a stock-firmware burst for each format and keep the IQ files.

### Phase 2: own RF layer (`kns_rf_own.c`)

- Implement the `KNS_RFTX_*` API directly on the STM32 HAL SUBGHZ driver, replaying the sequence logged in Phase 1.
- Remove `libknsrf_wl.a` from the link. The stock MAC in `libkineis.a` should keep working unchanged.
- Verify with three checks:
  - the wrapped command log matches byte for byte
  - the SDR capture overlays the golden one (constellation, chip timing, envelope)
  - a stock-firmware frame still gets through the Kinéis or CLS back end, if an authorised ID is available

### Phase 3: own frame encoders (`kns_ff_own.c`)

- Implement A2-legacy first; it is the simplest, with no crypto. Then A2, then VLDA4.
- Use the same function names and calling contract, so the stock MAC links against them.
- Host unit tests (gcc + CMake + CTest on Linux) must reproduce the Phase 1 golden vectors exactly.

### Phase 4: drop `libkineis.a`

- Replace the MAC with a minimal scheduler: one shot, plus a repeat-period ("blind") mode.
- Keep only what the A2 / VLDA4 uplink needs.
- Compare the final map-file size against the Phase 0 baseline.

## Tooling notes

- Disassemble with `arm-none-eabi-objdump -drw --no-show-raw-insn <obj>`; `-r` is essential, because the relocations name every callee and global.
  - The previous session used a small capstone + pyelftools script that annotates relocations per function section. Recreate it if objdump's output is awkward.
- The repo builds with its Makefile (`-O1`, `-ffunction-sections -fdata-sections`, `--gc-sections`, nano.specs).
  - Move to CMake once the source tree is ours, so VS Code (CMake Tools plus Cortex-Debug) can drive build, flash and debug.
- Reference transmitter: Argos-2 unit on COM4, 115200 baud, triggered with `x`.

## Repo layout

- Fork of `arribada/argos-smd-at-kineis-firmware`; work happens on branch `own-stack`. Remotes: `origin` = fork, `upstream` = Arribada.
- Step 2 of "Start here" is done: the first commit on `own-stack` is `kns_a2_vld_only.patch`, applied with `git am`.
- Own replacement code goes in `Own/` (`rf/`, `ff/`, `test/`), never mixed into `Kineis/`, so stock vs own layers stay a build switch.
- SDR IQ captures go in `C:\dev\argos-captures`, outside git.
