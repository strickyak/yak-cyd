#pragma once
#include <stdint.h>
#include "gep9_bus.h"

// 6809 Condition Code Flags
static constexpr uint8_t FlagC = 0x01;  // Carry
static constexpr uint8_t FlagV = 0x02;  // Overflow
static constexpr uint8_t FlagZ = 0x04;  // Zero
static constexpr uint8_t FlagN = 0x08;  // Negative
static constexpr uint8_t FlagI = 0x10;  // IRQ Mask
static constexpr uint8_t FlagH = 0x20;  // Half-Carry
static constexpr uint8_t FlagF = 0x40;  // FIRQ Mask
static constexpr uint8_t FlagE = 0x80;  // Entire state saved

class Cpu6809 {
 public:
  explicit Cpu6809(Deep65280Bus* bus);

  void reset();
  int step();
  void setIRQ(bool active) { irqLine_ = active; }
  void setFIRQ(bool active) { firqLine_ = active; }
  void triggerNMI() { nmiPending_ = true; }

  // Registers
  uint8_t A = 0;
  uint8_t B = 0;
  uint8_t DP = 0;
  uint8_t CC = 0;
  uint16_t X = 0;
  uint16_t Y = 0;
  uint16_t U = 0;
  uint16_t S = 0;
  uint16_t PC = 0;

  uint64_t Cycles = 0;
  bool Halted = false;

  uint16_t getD() const { return uint16_t((uint16_t(A) << 8) | B); }
  void setD(uint16_t v) {
    A = uint8_t(v >> 8);
    B = uint8_t(v & 0xFF);
  }

  void setFlag(uint8_t flag, bool cond) {
    if (cond) {
      CC |= flag;
    } else {
      CC &= uint8_t(~flag);
    }
  }
  bool getFlag(uint8_t flag) const { return (CC & flag) != 0; }

  void updateNZ8(uint8_t val) {
    setFlag(FlagZ, val == 0);
    setFlag(FlagN, (val & 0x80) != 0);
  }
  void updateNZ16(uint16_t val) {
    setFlag(FlagZ, val == 0);
    setFlag(FlagN, (val & 0x8000) != 0);
  }

 private:
  uint8_t fetch8() {
    uint8_t v = bus_->read(PC);
    PC++;
    return v;
  }
  uint16_t fetch16() {
    uint16_t v = bus_->read16(PC);
    PC += 2;
    return v;
  }

  void push8S(uint8_t val) {
    S--;
    bus_->write(S, val);
  }
  uint8_t pull8S() {
    uint8_t val = bus_->read(S);
    S++;
    return val;
  }
  void push16S(uint16_t val) {
    push8S(uint8_t(val & 0xFF));
    push8S(uint8_t(val >> 8));
  }
  uint16_t pull16S() {
    uint16_t hi = pull8S();
    uint16_t lo = pull8S();
    return uint16_t((hi << 8) | lo);
  }

  void push8U(uint8_t val) {
    U--;
    bus_->write(U, val);
  }
  uint8_t pull8U() {
    uint8_t val = bus_->read(U);
    U++;
    return val;
  }
  void push16U(uint16_t val) {
    push8U(uint8_t(val & 0xFF));
    push8U(uint8_t(val >> 8));
  }
  uint16_t pull16U() {
    uint16_t hi = pull8U();
    uint16_t lo = pull8U();
    return uint16_t((hi << 8) | lo);
  }

  // Addressing modes
  uint16_t addrDirect() {
    uint8_t offset = fetch8();
    return uint16_t((uint16_t(DP) << 8) | offset);
  }
  uint16_t addrExtended() { return fetch16(); }
  uint16_t addrIndexed(int& extraCycles);
  uint16_t* decodeIndexReg(uint8_t bits);

  // Stack / register ops
  int pushS(uint8_t mask);
  int pullS(uint8_t mask);
  int pushU(uint8_t mask);
  int pullU(uint8_t mask);
  uint16_t getReg16(uint8_t code) const;
  void setReg16(uint8_t code, uint16_t val);
  uint8_t getReg8(uint8_t code) const;
  void setReg8(uint8_t code, uint8_t val);
  void exg(uint8_t postbyte);
  void tfr(uint8_t postbyte);

  // ALU operations
  uint8_t opNeg8(uint8_t val);
  uint8_t opCom8(uint8_t val);
  uint8_t opLsr8(uint8_t val);
  uint8_t opRor8(uint8_t val);
  uint8_t opAsr8(uint8_t val);
  uint8_t opAsl8(uint8_t val);
  uint8_t opRol8(uint8_t val);
  uint8_t opDec8(uint8_t val);
  uint8_t opInc8(uint8_t val);
  void opTst8(uint8_t val);
  uint8_t opClr8();

  uint8_t opSub8(uint8_t reg, uint8_t val);
  uint8_t opSbc8(uint8_t reg, uint8_t val);
  void opCmp8(uint8_t reg, uint8_t val);
  uint8_t opAnd8(uint8_t reg, uint8_t val);
  uint8_t opEor8(uint8_t reg, uint8_t val);
  uint8_t opOr8(uint8_t reg, uint8_t val);
  uint8_t opAdd8(uint8_t reg, uint8_t val);
  uint8_t opAdc8(uint8_t reg, uint8_t val);
  void opDaa();

  uint16_t opSub16(uint16_t reg, uint16_t val);
  void opCmp16(uint16_t reg, uint16_t val);
  uint16_t opAdd16(uint16_t reg, uint16_t val);

  int stepPage2();
  int stepPage3();

  Deep65280Bus* bus_;
  bool nmiPending_ = false;
  bool firqLine_ = false;
  bool irqLine_ = false;
  bool sInitialized_ = false;
  bool waitingCWAI_ = false;
  bool waitingSYNC_ = false;
};
