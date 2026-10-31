#pragma once
#include <stddef.h>
#include <stdint.h>

class Cpu6809;

using ConsoleOutFn = void (*)(uint8_t ch, void* ctx);
using DiskSectorFn = bool (*)(uint8_t drive, bool write, uint32_t lsn, uint8_t* buf256, void* ctx);

class Deep65280Bus {
 public:
  static constexpr uint32_t kNumBlocks = 16;
  static constexpr uint32_t kBlockSize = 8192;
  static constexpr uint32_t kPhysRamSize = kNumBlocks * kBlockSize;  // 128 KB (16 x 8 KB MMU blocks)
  static constexpr uint32_t kPhysRamMask = kPhysRamSize - 1;         // 0x1FFFF

  Deep65280Bus();
  ~Deep65280Bus();

  bool initRam();
  void attachCpu(Cpu6809* cpu) { cpu_ = cpu; }
  void setConsoleCallback(ConsoleOutFn fn, void* ctx) {
    consoleOutFn_ = fn;
    consoleCtx_ = ctx;
  }
  void setDiskCallback(DiskSectorFn fn, void* ctx) {
    diskFn_ = fn;
    diskCtx_ = ctx;
  }

  // Loads the 64 KB deep65280.img into top of physical RAM and initializes MMU.
  void loadBootImage64K(const uint8_t* img64k);

  uint8_t read(uint16_t addr);
  void write(uint16_t addr, uint8_t val);

  uint16_t read16(uint16_t addr) {
    uint16_t hi = read(addr);
    uint16_t lo = read(uint16_t(addr + 1));
    return uint16_t((hi << 8) | lo);
  }

  void write16(uint16_t addr, uint16_t val) {
    write(addr, uint8_t(val >> 8));
    write(uint16_t(addr + 1), uint8_t(val & 0xFF));
  }

  // Push input byte(s) into ACIA receive FIFO
  void pushConsoleInput(uint8_t ch);
  void clearConsoleInput();
  bool hasConsoleInput() const { return rxHead_ != rxTail_; }

  // Called at 60 Hz (every ~16.67 ms)
  void clockTick();

  bool isHalted() const { return halted_; }
  uint32_t deepPhysAddr(uint16_t addr) const;

 private:
  uint8_t readIO(uint16_t addr);
  void writeIO(uint16_t addr, uint8_t val);
  void executeEmuDsk(uint8_t cmd);
  void evaluateIRQ();

  uint8_t readPhys(uint32_t phys) const {
    return physBlocks_[(phys >> 13) & 0x0F][phys & 0x1FFF];
  }
  void writePhys(uint32_t phys, uint8_t val) {
    physBlocks_[(phys >> 13) & 0x0F][phys & 0x1FFF] = val;
  }

  uint8_t* physBlocks_[kNumBlocks] = {};
  Cpu6809* cpu_ = nullptr;

  uint8_t mmuTask_ = 0;
  uint8_t mmuRegs_[2][8] = {};

  uint8_t aciaCtrl_ = 0;
  uint8_t clockAndStop_ = 0;

  uint32_t emuDskLSN_ = 0;
  uint16_t emuDskBuffer_ = 0;
  uint8_t emuDskDrive_ = 0;
  uint8_t emuDskStatus_ = 0;

  bool halted_ = false;

  static constexpr size_t kRxQueueSize = 512;
  uint8_t rxQueue_[kRxQueueSize] = {};
  size_t rxHead_ = 0;
  size_t rxTail_ = 0;

  ConsoleOutFn consoleOutFn_ = nullptr;
  void* consoleCtx_ = nullptr;
  DiskSectorFn diskFn_ = nullptr;
  void* diskCtx_ = nullptr;
};
