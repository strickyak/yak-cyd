#include "gep9_bus.h"
#include "gep9_cpu.h"
#include <stdlib.h>
#include <string.h>

#ifdef ARDUINO
#include <Arduino.h>
#else
#define memcpy_P memcpy
#endif

Deep65280Bus::Deep65280Bus() {}

Deep65280Bus::~Deep65280Bus() {
  for (uint32_t i = 0; i < kNumBlocks; i++) {
    if (physBlocks_[i]) {
      free(physBlocks_[i]);
      physBlocks_[i] = nullptr;
    }
  }
}

bool Deep65280Bus::initRam() {
  for (uint32_t i = 0; i < kNumBlocks; i++) {
    if (!physBlocks_[i]) {
      physBlocks_[i] = static_cast<uint8_t*>(malloc(kBlockSize));
      if (!physBlocks_[i]) return false;
    }
    memset(physBlocks_[i], 0, kBlockSize);
  }
  return true;
}

void Deep65280Bus::loadBootImage64K(const uint8_t* img64k) {
  initRam();

  // Copy 64 KB image into top 64 KB of physical RAM (blocks 8..15).
  for (uint32_t slot = 0; slot < 8; slot++) {
    memcpy_P(physBlocks_[8 + slot], img64k + slot * kBlockSize, kBlockSize);
  }

  // Copy Slot 0 ($0000..$1FFF) to Physical Block 0 (0x00000..0x01FFF)
  memcpy_P(physBlocks_[0], img64k, kBlockSize);

  // In 128 KB mode, clear Block 8 so user RAM block 8 is clean
  memset(physBlocks_[8], 0, kBlockSize);

  mmuTask_ = 0;
  mmuRegs_[0][0] = 0x00;
  mmuRegs_[1][0] = 0x00;
  for (int i = 1; i < 8; i++) {
    mmuRegs_[0][i] = uint8_t(0x38 + i);
    mmuRegs_[1][i] = uint8_t(0x38 + i);
  }

  aciaCtrl_ = 0;
  clockAndStop_ = 0;
  emuDskLSN_ = 0;
  emuDskBuffer_ = 0;
  emuDskDrive_ = 0;
  emuDskStatus_ = 0;
  halted_ = false;
  rxHead_ = 0;
  rxTail_ = 0;
}

uint32_t Deep65280Bus::deepPhysAddr(uint16_t addr) const {
  if ((addr >= 0xFE00 && addr <= 0xFEFF) || addr >= 0xFFF0) {
    return (0x70000u + uint32_t(addr)) & kPhysRamMask;
  }
  uint8_t slot = uint8_t(addr >> 13);
  uint8_t page = mmuRegs_[mmuTask_ & 1][slot];
  return ((uint32_t(page) << 13) | uint32_t(addr & 0x1FFFu)) & kPhysRamMask;
}

uint8_t Deep65280Bus::read(uint16_t addr) {
  if (addr >= 0xFF00 && addr <= 0xFFEF) {
    return readIO(addr);
  }
  return readPhys(deepPhysAddr(addr));
}

void Deep65280Bus::write(uint16_t addr, uint8_t val) {
  if (addr >= 0xFF00 && addr <= 0xFFEF) {
    writeIO(addr, val);
    return;
  }
  writePhys(deepPhysAddr(addr), val);
}

uint8_t Deep65280Bus::readIO(uint16_t addr) {
  if (addr >= 0xFF00 && addr <= 0xFF03) {
    return 0x00;
  }
  if (addr >= 0xFF80 && addr <= 0xFF86) {
    switch (addr) {
      case 0xFF80:
        return uint8_t((emuDskLSN_ >> 16) & 0xFF);
      case 0xFF81:
        return uint8_t((emuDskLSN_ >> 8) & 0xFF);
      case 0xFF82:
        return uint8_t(emuDskLSN_ & 0xFF);
      case 0xFF83:
        return emuDskStatus_;
      case 0xFF84:
        return uint8_t((emuDskBuffer_ >> 8) & 0xFF);
      case 0xFF85:
        return uint8_t(emuDskBuffer_ & 0xFF);
      case 0xFF86:
        return emuDskDrive_;
    }
  }
  if (addr == 0xFF87) {
    return clockAndStop_;
  }
  if (addr == 0xFF88) {
    uint8_t status = 0x02;  // TDRE always ready
    if (rxHead_ != rxTail_) {
      status |= 0x01;  // RDRF
      if (aciaCtrl_ & 0x80) {
        status |= 0x80;  // IRQ
      }
    }
    return status;
  }
  if (addr == 0xFF89) {
    if (rxHead_ != rxTail_) {
      uint8_t ch = rxQueue_[rxTail_];
      rxTail_ = (rxTail_ + 1) % kRxQueueSize;
      evaluateIRQ();
      return ch;
    }
    return 0x00;
  }
  if (addr == 0xFF90) {
    return 0x00;
  }
  if (addr == 0xFF91) {
    return mmuTask_;
  }
  if (addr == 0xFF92 || addr == 0xFF93) {
    return 0x00;
  }
  if (addr >= 0xFFA0 && addr <= 0xFFA7) {
    return mmuRegs_[0][addr - 0xFFA0];
  }
  if (addr >= 0xFFA8 && addr <= 0xFFAF) {
    return mmuRegs_[1][addr - 0xFFA8];
  }
  if (addr == 0xFFD8 || addr == 0xFFDE) {
    return 0x00;
  }
  return readPhys(deepPhysAddr(addr));
}

void Deep65280Bus::writeIO(uint16_t addr, uint8_t val) {
  if (addr >= 0xFF00 && addr <= 0xFF03) {
    return;
  }
  if (addr >= 0xFF80 && addr <= 0xFF86) {
    switch (addr) {
      case 0xFF80:
        emuDskLSN_ = (emuDskLSN_ & 0x00FFFFu) | (uint32_t(val) << 16);
        break;
      case 0xFF81:
        emuDskLSN_ = (emuDskLSN_ & 0xFF00FFu) | (uint32_t(val) << 8);
        break;
      case 0xFF82:
        emuDskLSN_ = (emuDskLSN_ & 0xFFFF00u) | uint32_t(val);
        break;
      case 0xFF83:
        executeEmuDsk(val);
        break;
      case 0xFF84:
        emuDskBuffer_ = uint16_t((emuDskBuffer_ & 0x00FFu) | (uint16_t(val) << 8));
        break;
      case 0xFF85:
        emuDskBuffer_ = uint16_t((emuDskBuffer_ & 0xFF00u) | uint16_t(val));
        break;
      case 0xFF86:
        emuDskDrive_ = val;
        break;
    }
    return;
  }
  if (addr == 0xFF87) {
    if (val == 0xFC || val == 0xFD || val == 0xFE) {
      halted_ = true;
      if (cpu_) cpu_->Halted = true;
      return;
    }
    if (val & 0x01) {
      clockAndStop_ &= uint8_t(~0x01);
    }
    clockAndStop_ = uint8_t((clockAndStop_ & 0x01) | (val & 0xFE));
    evaluateIRQ();
    return;
  }
  if (addr == 0xFF88) {
    if ((val & 0x03) == 0x03) {
      aciaCtrl_ = 0;
    } else {
      aciaCtrl_ = val;
    }
    evaluateIRQ();
    return;
  }
  if (addr == 0xFF89) {
    if (consoleOutFn_) {
      consoleOutFn_(val, consoleCtx_);
    }
    return;
  }
  if (addr == 0xFF90) {
    return;
  }
  if (addr == 0xFF91) {
    mmuTask_ = val & 0x01;
    return;
  }
  if (addr == 0xFF92 || addr == 0xFF93) {
    return;
  }
  if (addr >= 0xFFA0 && addr <= 0xFFA7) {
    mmuRegs_[0][addr - 0xFFA0] = val;
    return;
  }
  if (addr >= 0xFFA8 && addr <= 0xFFAF) {
    mmuRegs_[1][addr - 0xFFA8] = val;
    return;
  }
  if (addr == 0xFFD8 || addr == 0xFFDE) {
    return;
  }
  writePhys(deepPhysAddr(addr), val);
}

void Deep65280Bus::executeEmuDsk(uint8_t cmd) {
  if (emuDskDrive_ >= 4) {
    emuDskStatus_ = 1;
    return;
  }
  if (cmd == 2) {
    emuDskStatus_ = 0;
    return;
  }
  if (!diskFn_) {
    emuDskStatus_ = 1;
    return;
  }
  uint8_t sectorBuf[256];
  if (cmd == 0) {
    // Read 256-byte sector
    memset(sectorBuf, 0, sizeof(sectorBuf));
    if (!diskFn_(emuDskDrive_, false, emuDskLSN_, sectorBuf, diskCtx_)) {
      emuDskStatus_ = 1;
      return;
    }
    for (uint16_t i = 0; i < 256; i++) {
      uint32_t phys = deepPhysAddr(uint16_t(emuDskBuffer_ + i));
      writePhys(phys, sectorBuf[i]);
    }
    emuDskStatus_ = 0;
  } else if (cmd == 1) {
    // Write 256-byte sector
    for (uint16_t i = 0; i < 256; i++) {
      uint32_t phys = deepPhysAddr(uint16_t(emuDskBuffer_ + i));
      sectorBuf[i] = readPhys(phys);
    }
    if (!diskFn_(emuDskDrive_, true, emuDskLSN_, sectorBuf, diskCtx_)) {
      emuDskStatus_ = 1;
      return;
    }
    emuDskStatus_ = 0;
  } else {
    emuDskStatus_ = 1;
  }
}

void Deep65280Bus::pushConsoleInput(uint8_t ch) {
  size_t next = (rxHead_ + 1) % kRxQueueSize;
  if (next != rxTail_) {
    rxQueue_[rxHead_] = ch;
    rxHead_ = next;
  }
  evaluateIRQ();
}

void Deep65280Bus::clearConsoleInput() {
  rxHead_ = 0;
  rxTail_ = 0;
  evaluateIRQ();
}

void Deep65280Bus::clockTick() {
  clockAndStop_ |= 0x01;
  evaluateIRQ();
}

void Deep65280Bus::evaluateIRQ() {
  if (!cpu_) return;
  bool timerIRQ = (clockAndStop_ & 0x03) == 0x03;
  bool aciaIRQ = (rxHead_ != rxTail_) && ((aciaCtrl_ & 0x80) != 0);
  cpu_->setIRQ(timerIRQ || aciaIRQ);
}
