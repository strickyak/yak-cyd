#include "gep9_cpu.h"

Cpu6809::Cpu6809(Deep65280Bus* bus) : bus_(bus) {
  if (bus_) {
    bus_->attachCpu(this);
  }
}

void Cpu6809::reset() {
  A = 0;
  B = 0;
  DP = 0;
  X = 0;
  Y = 0;
  U = 0;
  S = 0;
  CC = FlagI | FlagF;
  nmiPending_ = false;
  firqLine_ = false;
  irqLine_ = false;
  sInitialized_ = false;
  waitingCWAI_ = false;
  waitingSYNC_ = false;
  Halted = false;
  PC = bus_->read16(0xFFFE);
}

uint16_t* Cpu6809::decodeIndexReg(uint8_t bits) {
  switch (bits & 0x03) {
    case 0:
      return &X;
    case 1:
      return &Y;
    case 2:
      return &U;
    default:
      return &S;
  }
}

uint16_t Cpu6809::addrIndexed(int& extraCycles) {
  uint8_t postbyte = fetch8();
  uint16_t* reg = decodeIndexReg((postbyte >> 5) & 0x03);

  // 5-bit constant offset (bit 7 == 0)
  if ((postbyte & 0x80) == 0) {
    int8_t offset = int8_t(postbyte & 0x1F);
    if (offset & 0x10) {
      offset |= int8_t(0xE0);  // sign-extend 5-bit to 8-bit
    }
    extraCycles = 1;
    return uint16_t(int32_t(*reg) + int32_t(offset));
  }

  bool indirect = (postbyte & 0x10) != 0;
  uint8_t mode = postbyte & 0x0F;
  uint16_t ea = 0;
  int cycles = 0;

  switch (mode) {
    case 0x00:  // ,R+
      ea = *reg;
      (*reg)++;
      cycles = 2;
      break;
    case 0x01:  // ,R++
      ea = *reg;
      (*reg) += 2;
      cycles = 3;
      break;
    case 0x02:  // ,-R
      (*reg)--;
      ea = *reg;
      cycles = 2;
      break;
    case 0x03:  // ,--R
      (*reg) -= 2;
      ea = *reg;
      cycles = 3;
      break;
    case 0x04:  // ,R (zero offset)
      ea = *reg;
      cycles = 0;
      break;
    case 0x05:  // B,R
      ea = uint16_t(int32_t(*reg) + int32_t(int8_t(B)));
      cycles = 1;
      break;
    case 0x06:  // A,R
      ea = uint16_t(int32_t(*reg) + int32_t(int8_t(A)));
      cycles = 1;
      break;
    case 0x08: {  // 8-bit offset, R
      int8_t off8 = int8_t(fetch8());
      ea = uint16_t(int32_t(*reg) + int32_t(off8));
      cycles = 1;
      break;
    }
    case 0x09: {  // 16-bit offset, R
      int16_t off16 = int16_t(fetch16());
      ea = uint16_t(int32_t(*reg) + int32_t(off16));
      cycles = 4;
      break;
    }
    case 0x0B:  // D,R
      ea = uint16_t(int32_t(*reg) + int32_t(int16_t(getD())));
      cycles = 4;
      break;
    case 0x0C: {  // 8-bit offset, PCR
      int8_t off8 = int8_t(fetch8());
      ea = uint16_t(int32_t(PC) + int32_t(off8));
      cycles = 1;
      break;
    }
    case 0x0D: {  // 16-bit offset, PCR
      int16_t off16 = int16_t(fetch16());
      ea = uint16_t(int32_t(PC) + int32_t(off16));
      cycles = 5;
      break;
    }
    case 0x0F:  // [Extended Indirect]
      ea = fetch16();
      cycles = 5;
      break;
    default:
      ea = *reg;
      cycles = 0;
      break;
  }

  if (indirect) {
    ea = bus_->read16(ea);
    if (mode != 0x0F) {
      cycles += 3;
    }
  }
  extraCycles = cycles;
  return ea;
}

int Cpu6809::pushS(uint8_t mask) {
  int cycles = 0;
  if (mask & 0x80) {
    push16S(PC);
    cycles += 2;
  }
  if (mask & 0x40) {
    push16S(U);
    cycles += 2;
  }
  if (mask & 0x20) {
    push16S(Y);
    cycles += 2;
  }
  if (mask & 0x10) {
    push16S(X);
    cycles += 2;
  }
  if (mask & 0x08) {
    push8S(DP);
    cycles += 1;
  }
  if (mask & 0x04) {
    push8S(B);
    cycles += 1;
  }
  if (mask & 0x02) {
    push8S(A);
    cycles += 1;
  }
  if (mask & 0x01) {
    push8S(CC);
    cycles += 1;
  }
  return cycles;
}

int Cpu6809::pullS(uint8_t mask) {
  int cycles = 0;
  if (mask & 0x01) {
    CC = pull8S();
    cycles += 1;
  }
  if (mask & 0x02) {
    A = pull8S();
    cycles += 1;
  }
  if (mask & 0x04) {
    B = pull8S();
    cycles += 1;
  }
  if (mask & 0x08) {
    DP = pull8S();
    cycles += 1;
  }
  if (mask & 0x10) {
    X = pull16S();
    cycles += 2;
  }
  if (mask & 0x20) {
    Y = pull16S();
    cycles += 2;
  }
  if (mask & 0x40) {
    U = pull16S();
    cycles += 2;
  }
  if (mask & 0x80) {
    PC = pull16S();
    cycles += 2;
  }
  return cycles;
}

int Cpu6809::pushU(uint8_t mask) {
  int cycles = 0;
  if (mask & 0x80) {
    push16U(PC);
    cycles += 2;
  }
  if (mask & 0x40) {
    push16U(S);
    cycles += 2;
  }
  if (mask & 0x20) {
    push16U(Y);
    cycles += 2;
  }
  if (mask & 0x10) {
    push16U(X);
    cycles += 2;
  }
  if (mask & 0x08) {
    push8U(DP);
    cycles += 1;
  }
  if (mask & 0x04) {
    push8U(B);
    cycles += 1;
  }
  if (mask & 0x02) {
    push8U(A);
    cycles += 1;
  }
  if (mask & 0x01) {
    push8U(CC);
    cycles += 1;
  }
  return cycles;
}

int Cpu6809::pullU(uint8_t mask) {
  int cycles = 0;
  if (mask & 0x01) {
    CC = pull8U();
    cycles += 1;
  }
  if (mask & 0x02) {
    A = pull8U();
    cycles += 1;
  }
  if (mask & 0x04) {
    B = pull8U();
    cycles += 1;
  }
  if (mask & 0x08) {
    DP = pull8U();
    cycles += 1;
  }
  if (mask & 0x10) {
    X = pull16U();
    cycles += 2;
  }
  if (mask & 0x20) {
    Y = pull16U();
    cycles += 2;
  }
  if (mask & 0x40) {
    S = pull16U();
    sInitialized_ = true;
    cycles += 2;
  }
  if (mask & 0x80) {
    PC = pull16U();
    cycles += 2;
  }
  return cycles;
}

uint16_t Cpu6809::getReg16(uint8_t code) const {
  switch (code) {
    case 0x0:
      return getD();
    case 0x1:
      return X;
    case 0x2:
      return Y;
    case 0x3:
      return U;
    case 0x4:
      return S;
    case 0x5:
      return PC;
    default:
      return 0xFFFF;
  }
}

void Cpu6809::setReg16(uint8_t code, uint16_t val) {
  switch (code) {
    case 0x0:
      setD(val);
      break;
    case 0x1:
      X = val;
      break;
    case 0x2:
      Y = val;
      break;
    case 0x3:
      U = val;
      break;
    case 0x4:
      S = val;
      sInitialized_ = true;
      break;
    case 0x5:
      PC = val;
      break;
  }
}

uint8_t Cpu6809::getReg8(uint8_t code) const {
  switch (code) {
    case 0x8:
      return A;
    case 0x9:
      return B;
    case 0xA:
      return CC;
    case 0xB:
      return DP;
    default:
      return 0xFF;
  }
}

void Cpu6809::setReg8(uint8_t code, uint8_t val) {
  switch (code) {
    case 0x8:
      A = val;
      break;
    case 0x9:
      B = val;
      break;
    case 0xA:
      CC = val;
      break;
    case 0xB:
      DP = val;
      break;
  }
}

void Cpu6809::exg(uint8_t postbyte) {
  uint8_t r1 = (postbyte >> 4) & 0x0F;
  uint8_t r2 = postbyte & 0x0F;
  if (r1 < 8 && r2 < 8) {
    uint16_t v1 = getReg16(r1);
    uint16_t v2 = getReg16(r2);
    setReg16(r1, v2);
    setReg16(r2, v1);
  } else if (r1 >= 8 && r2 >= 8) {
    uint8_t v1 = getReg8(r1);
    uint8_t v2 = getReg8(r2);
    setReg8(r1, v2);
    setReg8(r2, v1);
  }
}

void Cpu6809::tfr(uint8_t postbyte) {
  uint8_t src = (postbyte >> 4) & 0x0F;
  uint8_t dst = postbyte & 0x0F;
  if (src < 8 && dst < 8) {
    setReg16(dst, getReg16(src));
  } else if (src >= 8 && dst >= 8) {
    setReg8(dst, getReg8(src));
  }
}

uint8_t Cpu6809::opNeg8(uint8_t val) {
  uint8_t res = uint8_t(0 - val);
  updateNZ8(res);
  setFlag(FlagV, val == 0x80);
  setFlag(FlagC, val != 0x00);
  return res;
}

uint8_t Cpu6809::opCom8(uint8_t val) {
  uint8_t res = uint8_t(~val);
  updateNZ8(res);
  setFlag(FlagV, false);
  setFlag(FlagC, true);
  return res;
}

uint8_t Cpu6809::opLsr8(uint8_t val) {
  setFlag(FlagC, (val & 0x01) != 0);
  uint8_t res = val >> 1;
  setFlag(FlagZ, res == 0);
  setFlag(FlagN, false);
  return res;
}

uint8_t Cpu6809::opRor8(uint8_t val) {
  uint8_t oldC = getFlag(FlagC) ? 0x80 : 0x00;
  setFlag(FlagC, (val & 0x01) != 0);
  uint8_t res = (val >> 1) | oldC;
  updateNZ8(res);
  return res;
}

uint8_t Cpu6809::opAsr8(uint8_t val) {
  setFlag(FlagC, (val & 0x01) != 0);
  uint8_t res = (val >> 1) | (val & 0x80);
  updateNZ8(res);
  return res;
}

uint8_t Cpu6809::opAsl8(uint8_t val) {
  setFlag(FlagC, (val & 0x80) != 0);
  setFlag(FlagV, (((val >> 7) ^ (val >> 6)) & 0x01) != 0);
  uint8_t res = uint8_t(val << 1);
  updateNZ8(res);
  return res;
}

uint8_t Cpu6809::opRol8(uint8_t val) {
  uint8_t oldC = getFlag(FlagC) ? 0x01 : 0x00;
  setFlag(FlagC, (val & 0x80) != 0);
  setFlag(FlagV, (((val >> 7) ^ (val >> 6)) & 0x01) != 0);
  uint8_t res = uint8_t((val << 1) | oldC);
  updateNZ8(res);
  return res;
}

uint8_t Cpu6809::opDec8(uint8_t val) {
  setFlag(FlagV, val == 0x80);
  uint8_t res = uint8_t(val - 1);
  updateNZ8(res);
  return res;
}

uint8_t Cpu6809::opInc8(uint8_t val) {
  setFlag(FlagV, val == 0x7F);
  uint8_t res = uint8_t(val + 1);
  updateNZ8(res);
  return res;
}

void Cpu6809::opTst8(uint8_t val) {
  setFlag(FlagV, false);
  updateNZ8(val);
}

uint8_t Cpu6809::opClr8() {
  setFlag(FlagN, false);
  setFlag(FlagZ, true);
  setFlag(FlagV, false);
  setFlag(FlagC, false);
  return 0;
}

uint8_t Cpu6809::opSub8(uint8_t reg, uint8_t val) {
  uint16_t res16 = uint16_t(reg) - uint16_t(val);
  uint8_t res = uint8_t(res16);
  updateNZ8(res);
  setFlag(FlagC, (res16 & 0x100) != 0);
  setFlag(FlagV, (((reg ^ val) & (reg ^ res)) & 0x80) != 0);
  return res;
}

uint8_t Cpu6809::opSbc8(uint8_t reg, uint8_t val) {
  uint16_t c = getFlag(FlagC) ? 1 : 0;
  uint16_t res16 = uint16_t(reg) - uint16_t(val) - c;
  uint8_t res = uint8_t(res16);
  updateNZ8(res);
  setFlag(FlagC, (res16 & 0x100) != 0);
  setFlag(FlagV, (((reg ^ val) & (reg ^ res)) & 0x80) != 0);
  return res;
}

void Cpu6809::opCmp8(uint8_t reg, uint8_t val) { (void)opSub8(reg, val); }

uint8_t Cpu6809::opAnd8(uint8_t reg, uint8_t val) {
  uint8_t res = reg & val;
  updateNZ8(res);
  setFlag(FlagV, false);
  return res;
}

uint8_t Cpu6809::opEor8(uint8_t reg, uint8_t val) {
  uint8_t res = reg ^ val;
  updateNZ8(res);
  setFlag(FlagV, false);
  return res;
}

uint8_t Cpu6809::opOr8(uint8_t reg, uint8_t val) {
  uint8_t res = reg | val;
  updateNZ8(res);
  setFlag(FlagV, false);
  return res;
}

uint8_t Cpu6809::opAdd8(uint8_t reg, uint8_t val) {
  uint16_t res16 = uint16_t(reg) + uint16_t(val);
  uint8_t res = uint8_t(res16);
  updateNZ8(res);
  setFlag(FlagC, (res16 & 0x100) != 0);
  setFlag(FlagV, ((~(reg ^ val) & (reg ^ res)) & 0x80) != 0);
  setFlag(FlagH, (((reg & 0x0F) + (val & 0x0F)) & 0x10) != 0);
  return res;
}

uint8_t Cpu6809::opAdc8(uint8_t reg, uint8_t val) {
  uint16_t c = getFlag(FlagC) ? 1 : 0;
  uint16_t res16 = uint16_t(reg) + uint16_t(val) + c;
  uint8_t res = uint8_t(res16);
  updateNZ8(res);
  setFlag(FlagC, (res16 & 0x100) != 0);
  setFlag(FlagV, ((~(reg ^ val) & (reg ^ res)) & 0x80) != 0);
  setFlag(FlagH, (((reg & 0x0F) + (val & 0x0F) + uint8_t(c)) & 0x10) != 0);
  return res;
}

void Cpu6809::opDaa() {
  uint16_t cf = 0;
  uint8_t msn = A & 0xF0;
  uint8_t lsn = A & 0x0F;
  if (lsn > 0x09 || getFlag(FlagH)) {
    cf |= 0x06;
  }
  if (msn > 0x80 && lsn > 0x09) {
    cf |= 0x60;
  }
  if (msn > 0x90 || getFlag(FlagC)) {
    cf |= 0x60;
  }
  uint16_t res16 = uint16_t(A) + cf;
  setFlag(FlagC, getFlag(FlagC) || (res16 & 0x100) != 0);
  A = uint8_t(res16);
  updateNZ8(A);
}

uint16_t Cpu6809::opSub16(uint16_t reg, uint16_t val) {
  uint32_t res32 = uint32_t(reg) - uint32_t(val);
  uint16_t res = uint16_t(res32);
  updateNZ16(res);
  setFlag(FlagC, (res32 & 0x10000) != 0);
  setFlag(FlagV, (((reg ^ val) & (reg ^ res)) & 0x8000) != 0);
  return res;
}

void Cpu6809::opCmp16(uint16_t reg, uint16_t val) { (void)opSub16(reg, val); }

uint16_t Cpu6809::opAdd16(uint16_t reg, uint16_t val) {
  uint32_t res32 = uint32_t(reg) + uint32_t(val);
  uint16_t res = uint16_t(res32);
  updateNZ16(res);
  setFlag(FlagC, (res32 & 0x10000) != 0);
  setFlag(FlagV, ((~(reg ^ val) & (reg ^ res)) & 0x8000) != 0);
  return res;
}

int Cpu6809::step() {
  if (Halted) return 0;

  // Check interrupts
  if (nmiPending_ && sInitialized_) {
    nmiPending_ = false;
    waitingSYNC_ = false;
    if (waitingCWAI_) {
      waitingCWAI_ = false;
    } else {
      setFlag(FlagE, true);
      pushS(0xFF);
    }
    setFlag(FlagI, true);
    setFlag(FlagF, true);
    PC = bus_->read16(0xFFFC);
    Cycles += 19;
    return 19;
  }

  if (firqLine_) {
    waitingSYNC_ = false;
    if (!getFlag(FlagF)) {
      if (waitingCWAI_) {
        waitingCWAI_ = false;
      } else {
        setFlag(FlagE, false);
        pushS(0x81);  // PC and CC only
      }
      setFlag(FlagI, true);
      setFlag(FlagF, true);
      PC = bus_->read16(0xFFF6);
      Cycles += 10;
      return 10;
    }
  }

  if (irqLine_) {
    waitingSYNC_ = false;
    if (!getFlag(FlagI)) {
      if (waitingCWAI_) {
        waitingCWAI_ = false;
      } else {
        setFlag(FlagE, true);
        pushS(0xFF);  // Entire state
      }
      setFlag(FlagI, true);
      PC = bus_->read16(0xFFF8);
      Cycles += 19;
      return 19;
    }
  }

  if (waitingCWAI_ || waitingSYNC_) {
    Cycles += 1;
    return 1;
  }

  uint8_t op = fetch8();
  int cycles = 0;

  switch (op) {
    // --- Direct Page unary ops ($00 - $0F) ---
    case 0x00: {
      uint16_t ea = addrDirect();
      bus_->write(ea, opNeg8(bus_->read(ea)));
      cycles = 6;
      break;
    }
    case 0x03: {
      uint16_t ea = addrDirect();
      bus_->write(ea, opCom8(bus_->read(ea)));
      cycles = 6;
      break;
    }
    case 0x04: {
      uint16_t ea = addrDirect();
      bus_->write(ea, opLsr8(bus_->read(ea)));
      cycles = 6;
      break;
    }
    case 0x06: {
      uint16_t ea = addrDirect();
      bus_->write(ea, opRor8(bus_->read(ea)));
      cycles = 6;
      break;
    }
    case 0x07: {
      uint16_t ea = addrDirect();
      bus_->write(ea, opAsr8(bus_->read(ea)));
      cycles = 6;
      break;
    }
    case 0x08: {
      uint16_t ea = addrDirect();
      bus_->write(ea, opAsl8(bus_->read(ea)));
      cycles = 6;
      break;
    }
    case 0x09: {
      uint16_t ea = addrDirect();
      bus_->write(ea, opRol8(bus_->read(ea)));
      cycles = 6;
      break;
    }
    case 0x0A: {
      uint16_t ea = addrDirect();
      bus_->write(ea, opDec8(bus_->read(ea)));
      cycles = 6;
      break;
    }
    case 0x0C: {
      uint16_t ea = addrDirect();
      bus_->write(ea, opInc8(bus_->read(ea)));
      cycles = 6;
      break;
    }
    case 0x0D: {
      uint16_t ea = addrDirect();
      opTst8(bus_->read(ea));
      cycles = 6;
      break;
    }
    case 0x0E: {
      PC = addrDirect();
      cycles = 3;
      break;
    }
    case 0x0F: {
      uint16_t ea = addrDirect();
      bus_->write(ea, opClr8());
      cycles = 6;
      break;
    }

    // --- Page 2 / Page 3 prefixes & Misc ($10 - $1F) ---
    case 0x10:
      cycles = stepPage2();
      break;
    case 0x11:
      cycles = stepPage3();
      break;
    case 0x12:  // NOP
      cycles = 2;
      break;
    case 0x13:  // SYNC
      waitingSYNC_ = true;
      cycles = 4;
      break;
    case 0x16: {  // LBRA
      int16_t off = int16_t(fetch16());
      PC = uint16_t(int32_t(PC) + int32_t(off));
      cycles = 5;
      break;
    }
    case 0x17: {  // LBSR
      int16_t off = int16_t(fetch16());
      push16S(PC);
      PC = uint16_t(int32_t(PC) + int32_t(off));
      cycles = 9;
      break;
    }
    case 0x19:  // DAA
      opDaa();
      cycles = 2;
      break;
    case 0x1A:  // ORCC
      CC |= fetch8();
      cycles = 3;
      break;
    case 0x1C:  // ANDCC
      CC &= fetch8();
      cycles = 3;
      break;
    case 0x1D:  // SEX
      A = (B & 0x80) ? 0xFF : 0x00;
      updateNZ16(getD());
      cycles = 2;
      break;
    case 0x1E:  // EXG
      exg(fetch8());
      cycles = 8;
      break;
    case 0x1F:  // TFR
      tfr(fetch8());
      cycles = 6;
      break;

    // --- Short Branches ($20 - $2F) ---
    case 0x20: {  // BRA
      int8_t off = int8_t(fetch8());
      PC = uint16_t(int32_t(PC) + int32_t(off));
      cycles = 3;
      break;
    }
    case 0x21:  // BRN
      (void)fetch8();
      cycles = 3;
      break;
    case 0x22: {  // BHI
      int8_t off = int8_t(fetch8());
      if (!getFlag(FlagC) && !getFlag(FlagZ)) PC = uint16_t(int32_t(PC) + int32_t(off));
      cycles = 3;
      break;
    }
    case 0x23: {  // BLS
      int8_t off = int8_t(fetch8());
      if (getFlag(FlagC) || getFlag(FlagZ)) PC = uint16_t(int32_t(PC) + int32_t(off));
      cycles = 3;
      break;
    }
    case 0x24: {  // BCC / BHS
      int8_t off = int8_t(fetch8());
      if (!getFlag(FlagC)) PC = uint16_t(int32_t(PC) + int32_t(off));
      cycles = 3;
      break;
    }
    case 0x25: {  // BCS / BLO
      int8_t off = int8_t(fetch8());
      if (getFlag(FlagC)) PC = uint16_t(int32_t(PC) + int32_t(off));
      cycles = 3;
      break;
    }
    case 0x26: {  // BNE
      int8_t off = int8_t(fetch8());
      if (!getFlag(FlagZ)) PC = uint16_t(int32_t(PC) + int32_t(off));
      cycles = 3;
      break;
    }
    case 0x27: {  // BEQ
      int8_t off = int8_t(fetch8());
      if (getFlag(FlagZ)) PC = uint16_t(int32_t(PC) + int32_t(off));
      cycles = 3;
      break;
    }
    case 0x28: {  // BVC
      int8_t off = int8_t(fetch8());
      if (!getFlag(FlagV)) PC = uint16_t(int32_t(PC) + int32_t(off));
      cycles = 3;
      break;
    }
    case 0x29: {  // BVS
      int8_t off = int8_t(fetch8());
      if (getFlag(FlagV)) PC = uint16_t(int32_t(PC) + int32_t(off));
      cycles = 3;
      break;
    }
    case 0x2A: {  // BPL
      int8_t off = int8_t(fetch8());
      if (!getFlag(FlagN)) PC = uint16_t(int32_t(PC) + int32_t(off));
      cycles = 3;
      break;
    }
    case 0x2B: {  // BMI
      int8_t off = int8_t(fetch8());
      if (getFlag(FlagN)) PC = uint16_t(int32_t(PC) + int32_t(off));
      cycles = 3;
      break;
    }
    case 0x2C: {  // BGE
      int8_t off = int8_t(fetch8());
      if (getFlag(FlagN) == getFlag(FlagV)) PC = uint16_t(int32_t(PC) + int32_t(off));
      cycles = 3;
      break;
    }
    case 0x2D: {  // BLT
      int8_t off = int8_t(fetch8());
      if (getFlag(FlagN) != getFlag(FlagV)) PC = uint16_t(int32_t(PC) + int32_t(off));
      cycles = 3;
      break;
    }
    case 0x2E: {  // BGT
      int8_t off = int8_t(fetch8());
      if (!getFlag(FlagZ) && (getFlag(FlagN) == getFlag(FlagV)))
        PC = uint16_t(int32_t(PC) + int32_t(off));
      cycles = 3;
      break;
    }
    case 0x2F: {  // BLE
      int8_t off = int8_t(fetch8());
      if (getFlag(FlagZ) || (getFlag(FlagN) != getFlag(FlagV)))
        PC = uint16_t(int32_t(PC) + int32_t(off));
      cycles = 3;
      break;
    }

    // --- LEA & Stack & Interrupt Return ($30 - $3F) ---
    case 0x30: {  // LEAX
      int ex = 0;
      X = addrIndexed(ex);
      setFlag(FlagZ, X == 0);
      cycles = 4 + ex;
      break;
    }
    case 0x31: {  // LEAY
      int ex = 0;
      Y = addrIndexed(ex);
      setFlag(FlagZ, Y == 0);
      cycles = 4 + ex;
      break;
    }
    case 0x32: {  // LEAS
      int ex = 0;
      S = addrIndexed(ex);
      sInitialized_ = true;
      cycles = 4 + ex;
      break;
    }
    case 0x33: {  // LEAU
      int ex = 0;
      U = addrIndexed(ex);
      cycles = 4 + ex;
      break;
    }
    case 0x34:  // PSHS
      cycles = 5 + pushS(fetch8());
      break;
    case 0x35:  // PULS
      cycles = 5 + pullS(fetch8());
      break;
    case 0x36:  // PSHU
      cycles = 5 + pushU(fetch8());
      break;
    case 0x37:  // PULU
      cycles = 5 + pullU(fetch8());
      break;
    case 0x39:  // RTS
      PC = pull16S();
      cycles = 5;
      break;
    case 0x3A:  // ABX
      X += uint16_t(B);
      cycles = 3;
      break;
    case 0x3B:  // RTI
      CC = pull8S();
      if (getFlag(FlagE)) {
        pullS(0xFE);
        cycles = 15;
      } else {
        PC = pull16S();
        cycles = 6;
      }
      break;
    case 0x3C: {  // CWAI
      uint8_t imm = fetch8();
      CC &= imm;
      setFlag(FlagE, true);
      pushS(0xFF);
      waitingCWAI_ = true;
      cycles = 20;
      break;
    }
    case 0x3D: {  // MUL
      uint16_t res = uint16_t(A) * uint16_t(B);
      setD(res);
      setFlag(FlagZ, res == 0);
      setFlag(FlagC, (B & 0x80) != 0);
      cycles = 11;
      break;
    }
    case 0x3F:  // SWI
      setFlag(FlagE, true);
      pushS(0xFF);
      setFlag(FlagI, true);
      setFlag(FlagF, true);
      PC = bus_->read16(0xFFFA);
      cycles = 19;
      break;

    // --- Accumulator A unary ops ($40 - $4F) ---
    case 0x40:
      A = opNeg8(A);
      cycles = 2;
      break;
    case 0x43:
      A = opCom8(A);
      cycles = 2;
      break;
    case 0x44:
      A = opLsr8(A);
      cycles = 2;
      break;
    case 0x46:
      A = opRor8(A);
      cycles = 2;
      break;
    case 0x47:
      A = opAsr8(A);
      cycles = 2;
      break;
    case 0x48:
      A = opAsl8(A);
      cycles = 2;
      break;
    case 0x49:
      A = opRol8(A);
      cycles = 2;
      break;
    case 0x4A:
      A = opDec8(A);
      cycles = 2;
      break;
    case 0x4C:
      A = opInc8(A);
      cycles = 2;
      break;
    case 0x4D:
      opTst8(A);
      cycles = 2;
      break;
    case 0x4F:
      A = opClr8();
      cycles = 2;
      break;

    // --- Accumulator B unary ops ($50 - $5F) ---
    case 0x50:
      B = opNeg8(B);
      cycles = 2;
      break;
    case 0x53:
      B = opCom8(B);
      cycles = 2;
      break;
    case 0x54:
      B = opLsr8(B);
      cycles = 2;
      break;
    case 0x56:
      B = opRor8(B);
      cycles = 2;
      break;
    case 0x57:
      B = opAsr8(B);
      cycles = 2;
      break;
    case 0x58:
      B = opAsl8(B);
      cycles = 2;
      break;
    case 0x59:
      B = opRol8(B);
      cycles = 2;
      break;
    case 0x5A:
      B = opDec8(B);
      cycles = 2;
      break;
    case 0x5C:
      B = opInc8(B);
      cycles = 2;
      break;
    case 0x5D:
      opTst8(B);
      cycles = 2;
      break;
    case 0x5F:
      B = opClr8();
      cycles = 2;
      break;

    // --- Indexed unary ops ($60 - $6F) ---
    case 0x60: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      bus_->write(ea, opNeg8(bus_->read(ea)));
      cycles = 6 + ex;
      break;
    }
    case 0x63: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      bus_->write(ea, opCom8(bus_->read(ea)));
      cycles = 6 + ex;
      break;
    }
    case 0x64: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      bus_->write(ea, opLsr8(bus_->read(ea)));
      cycles = 6 + ex;
      break;
    }
    case 0x66: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      bus_->write(ea, opRor8(bus_->read(ea)));
      cycles = 6 + ex;
      break;
    }
    case 0x67: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      bus_->write(ea, opAsr8(bus_->read(ea)));
      cycles = 6 + ex;
      break;
    }
    case 0x68: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      bus_->write(ea, opAsl8(bus_->read(ea)));
      cycles = 6 + ex;
      break;
    }
    case 0x69: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      bus_->write(ea, opRol8(bus_->read(ea)));
      cycles = 6 + ex;
      break;
    }
    case 0x6A: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      bus_->write(ea, opDec8(bus_->read(ea)));
      cycles = 6 + ex;
      break;
    }
    case 0x6C: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      bus_->write(ea, opInc8(bus_->read(ea)));
      cycles = 6 + ex;
      break;
    }
    case 0x6D: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      opTst8(bus_->read(ea));
      cycles = 6 + ex;
      break;
    }
    case 0x6E: {
      int ex = 0;
      PC = addrIndexed(ex);
      cycles = 3 + ex;
      break;
    }
    case 0x6F: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      bus_->write(ea, opClr8());
      cycles = 6 + ex;
      break;
    }

    // --- Extended unary ops ($70 - $7F) ---
    case 0x70: {
      uint16_t ea = addrExtended();
      bus_->write(ea, opNeg8(bus_->read(ea)));
      cycles = 7;
      break;
    }
    case 0x73: {
      uint16_t ea = addrExtended();
      bus_->write(ea, opCom8(bus_->read(ea)));
      cycles = 7;
      break;
    }
    case 0x74: {
      uint16_t ea = addrExtended();
      bus_->write(ea, opLsr8(bus_->read(ea)));
      cycles = 7;
      break;
    }
    case 0x76: {
      uint16_t ea = addrExtended();
      bus_->write(ea, opRor8(bus_->read(ea)));
      cycles = 7;
      break;
    }
    case 0x77: {
      uint16_t ea = addrExtended();
      bus_->write(ea, opAsr8(bus_->read(ea)));
      cycles = 7;
      break;
    }
    case 0x78: {
      uint16_t ea = addrExtended();
      bus_->write(ea, opAsl8(bus_->read(ea)));
      cycles = 7;
      break;
    }
    case 0x79: {
      uint16_t ea = addrExtended();
      bus_->write(ea, opRol8(bus_->read(ea)));
      cycles = 7;
      break;
    }
    case 0x7A: {
      uint16_t ea = addrExtended();
      bus_->write(ea, opDec8(bus_->read(ea)));
      cycles = 7;
      break;
    }
    case 0x7C: {
      uint16_t ea = addrExtended();
      bus_->write(ea, opInc8(bus_->read(ea)));
      cycles = 7;
      break;
    }
    case 0x7D: {
      uint16_t ea = addrExtended();
      opTst8(bus_->read(ea));
      cycles = 7;
      break;
    }
    case 0x7E:
      PC = addrExtended();
      cycles = 4;
      break;
    case 0x7F: {
      uint16_t ea = addrExtended();
      bus_->write(ea, opClr8());
      cycles = 7;
      break;
    }

    // --- Accumulator A / 16-bit Immediate ($80 - $8F) ---
    case 0x80:
      A = opSub8(A, fetch8());
      cycles = 2;
      break;
    case 0x81:
      opCmp8(A, fetch8());
      cycles = 2;
      break;
    case 0x82:
      A = opSbc8(A, fetch8());
      cycles = 2;
      break;
    case 0x83:
      setD(opSub16(getD(), fetch16()));
      cycles = 4;
      break;
    case 0x84:
      A = opAnd8(A, fetch8());
      cycles = 2;
      break;
    case 0x85:
      (void)opAnd8(A, fetch8());
      cycles = 2;
      break;
    case 0x86:
      A = fetch8();
      updateNZ8(A);
      setFlag(FlagV, false);
      cycles = 2;
      break;
    case 0x88:
      A = opEor8(A, fetch8());
      cycles = 2;
      break;
    case 0x89:
      A = opAdc8(A, fetch8());
      cycles = 2;
      break;
    case 0x8A:
      A = opOr8(A, fetch8());
      cycles = 2;
      break;
    case 0x8B:
      A = opAdd8(A, fetch8());
      cycles = 2;
      break;
    case 0x8C:
      opCmp16(X, fetch16());
      cycles = 4;
      break;
    case 0x8D: {  // BSR
      int8_t off = int8_t(fetch8());
      push16S(PC);
      PC = uint16_t(int32_t(PC) + int32_t(off));
      cycles = 7;
      break;
    }
    case 0x8E:
      X = fetch16();
      updateNZ16(X);
      setFlag(FlagV, false);
      cycles = 3;
      break;

    // --- Accumulator A / 16-bit Direct ($90 - $9F) ---
    case 0x90:
      A = opSub8(A, bus_->read(addrDirect()));
      cycles = 4;
      break;
    case 0x91:
      opCmp8(A, bus_->read(addrDirect()));
      cycles = 4;
      break;
    case 0x92:
      A = opSbc8(A, bus_->read(addrDirect()));
      cycles = 4;
      break;
    case 0x93:
      setD(opSub16(getD(), bus_->read16(addrDirect())));
      cycles = 6;
      break;
    case 0x94:
      A = opAnd8(A, bus_->read(addrDirect()));
      cycles = 4;
      break;
    case 0x95:
      (void)opAnd8(A, bus_->read(addrDirect()));
      cycles = 4;
      break;
    case 0x96:
      A = bus_->read(addrDirect());
      updateNZ8(A);
      setFlag(FlagV, false);
      cycles = 4;
      break;
    case 0x97:
      bus_->write(addrDirect(), A);
      updateNZ8(A);
      setFlag(FlagV, false);
      cycles = 4;
      break;
    case 0x98:
      A = opEor8(A, bus_->read(addrDirect()));
      cycles = 4;
      break;
    case 0x99:
      A = opAdc8(A, bus_->read(addrDirect()));
      cycles = 4;
      break;
    case 0x9A:
      A = opOr8(A, bus_->read(addrDirect()));
      cycles = 4;
      break;
    case 0x9B:
      A = opAdd8(A, bus_->read(addrDirect()));
      cycles = 4;
      break;
    case 0x9C:
      opCmp16(X, bus_->read16(addrDirect()));
      cycles = 6;
      break;
    case 0x9D: {  // JSR Direct
      uint16_t ea = addrDirect();
      push16S(PC);
      PC = ea;
      cycles = 7;
      break;
    }
    case 0x9E:
      X = bus_->read16(addrDirect());
      updateNZ16(X);
      setFlag(FlagV, false);
      cycles = 5;
      break;
    case 0x9F:
      bus_->write16(addrDirect(), X);
      updateNZ16(X);
      setFlag(FlagV, false);
      cycles = 5;
      break;

    // --- Accumulator A / 16-bit Indexed ($A0 - $AF) ---
    case 0xA0: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      A = opSub8(A, bus_->read(ea));
      cycles = 4 + ex;
      break;
    }
    case 0xA1: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      opCmp8(A, bus_->read(ea));
      cycles = 4 + ex;
      break;
    }
    case 0xA2: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      A = opSbc8(A, bus_->read(ea));
      cycles = 4 + ex;
      break;
    }
    case 0xA3: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      setD(opSub16(getD(), bus_->read16(ea)));
      cycles = 6 + ex;
      break;
    }
    case 0xA4: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      A = opAnd8(A, bus_->read(ea));
      cycles = 4 + ex;
      break;
    }
    case 0xA5: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      (void)opAnd8(A, bus_->read(ea));
      cycles = 4 + ex;
      break;
    }
    case 0xA6: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      A = bus_->read(ea);
      updateNZ8(A);
      setFlag(FlagV, false);
      cycles = 4 + ex;
      break;
    }
    case 0xA7: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      bus_->write(ea, A);
      updateNZ8(A);
      setFlag(FlagV, false);
      cycles = 4 + ex;
      break;
    }
    case 0xA8: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      A = opEor8(A, bus_->read(ea));
      cycles = 4 + ex;
      break;
    }
    case 0xA9: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      A = opAdc8(A, bus_->read(ea));
      cycles = 4 + ex;
      break;
    }
    case 0xAA: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      A = opOr8(A, bus_->read(ea));
      cycles = 4 + ex;
      break;
    }
    case 0xAB: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      A = opAdd8(A, bus_->read(ea));
      cycles = 4 + ex;
      break;
    }
    case 0xAC: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      opCmp16(X, bus_->read16(ea));
      cycles = 6 + ex;
      break;
    }
    case 0xAD: {  // JSR Indexed
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      push16S(PC);
      PC = ea;
      cycles = 7 + ex;
      break;
    }
    case 0xAE: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      X = bus_->read16(ea);
      updateNZ16(X);
      setFlag(FlagV, false);
      cycles = 5 + ex;
      break;
    }
    case 0xAF: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      bus_->write16(ea, X);
      updateNZ16(X);
      setFlag(FlagV, false);
      cycles = 5 + ex;
      break;
    }

    // --- Accumulator A / 16-bit Extended ($B0 - $BF) ---
    case 0xB0:
      A = opSub8(A, bus_->read(addrExtended()));
      cycles = 5;
      break;
    case 0xB1:
      opCmp8(A, bus_->read(addrExtended()));
      cycles = 5;
      break;
    case 0xB2:
      A = opSbc8(A, bus_->read(addrExtended()));
      cycles = 5;
      break;
    case 0xB3:
      setD(opSub16(getD(), bus_->read16(addrExtended())));
      cycles = 7;
      break;
    case 0xB4:
      A = opAnd8(A, bus_->read(addrExtended()));
      cycles = 5;
      break;
    case 0xB5:
      (void)opAnd8(A, bus_->read(addrExtended()));
      cycles = 5;
      break;
    case 0xB6:
      A = bus_->read(addrExtended());
      updateNZ8(A);
      setFlag(FlagV, false);
      cycles = 5;
      break;
    case 0xB7:
      bus_->write(addrExtended(), A);
      updateNZ8(A);
      setFlag(FlagV, false);
      cycles = 5;
      break;
    case 0xB8:
      A = opEor8(A, bus_->read(addrExtended()));
      cycles = 5;
      break;
    case 0xB9:
      A = opAdc8(A, bus_->read(addrExtended()));
      cycles = 5;
      break;
    case 0xBA:
      A = opOr8(A, bus_->read(addrExtended()));
      cycles = 5;
      break;
    case 0xBB:
      A = opAdd8(A, bus_->read(addrExtended()));
      cycles = 5;
      break;
    case 0xBC:
      opCmp16(X, bus_->read16(addrExtended()));
      cycles = 7;
      break;
    case 0xBD: {  // JSR Extended
      uint16_t ea = addrExtended();
      push16S(PC);
      PC = ea;
      cycles = 8;
      break;
    }
    case 0xBE:
      X = bus_->read16(addrExtended());
      updateNZ16(X);
      setFlag(FlagV, false);
      cycles = 6;
      break;
    case 0xBF:
      bus_->write16(addrExtended(), X);
      updateNZ16(X);
      setFlag(FlagV, false);
      cycles = 6;
      break;

    // --- Accumulator B / 16-bit Immediate ($C0 - $CF) ---
    case 0xC0:
      B = opSub8(B, fetch8());
      cycles = 2;
      break;
    case 0xC1:
      opCmp8(B, fetch8());
      cycles = 2;
      break;
    case 0xC2:
      B = opSbc8(B, fetch8());
      cycles = 2;
      break;
    case 0xC3:
      setD(opAdd16(getD(), fetch16()));
      cycles = 4;
      break;
    case 0xC4:
      B = opAnd8(B, fetch8());
      cycles = 2;
      break;
    case 0xC5:
      (void)opAnd8(B, fetch8());
      cycles = 2;
      break;
    case 0xC6:
      B = fetch8();
      updateNZ8(B);
      setFlag(FlagV, false);
      cycles = 2;
      break;
    case 0xC8:
      B = opEor8(B, fetch8());
      cycles = 2;
      break;
    case 0xC9:
      B = opAdc8(B, fetch8());
      cycles = 2;
      break;
    case 0xCA:
      B = opOr8(B, fetch8());
      cycles = 2;
      break;
    case 0xCB:
      B = opAdd8(B, fetch8());
      cycles = 2;
      break;
    case 0xCC: {
      uint16_t v = fetch16();
      setD(v);
      updateNZ16(v);
      setFlag(FlagV, false);
      cycles = 3;
      break;
    }
    case 0xCE:
      U = fetch16();
      updateNZ16(U);
      setFlag(FlagV, false);
      cycles = 3;
      break;

    // --- Accumulator B / 16-bit Direct ($D0 - $DF) ---
    case 0xD0:
      B = opSub8(B, bus_->read(addrDirect()));
      cycles = 4;
      break;
    case 0xD1:
      opCmp8(B, bus_->read(addrDirect()));
      cycles = 4;
      break;
    case 0xD2:
      B = opSbc8(B, bus_->read(addrDirect()));
      cycles = 4;
      break;
    case 0xD3:
      setD(opAdd16(getD(), bus_->read16(addrDirect())));
      cycles = 6;
      break;
    case 0xD4:
      B = opAnd8(B, bus_->read(addrDirect()));
      cycles = 4;
      break;
    case 0xD5:
      (void)opAnd8(B, bus_->read(addrDirect()));
      cycles = 4;
      break;
    case 0xD6:
      B = bus_->read(addrDirect());
      updateNZ8(B);
      setFlag(FlagV, false);
      cycles = 4;
      break;
    case 0xD7:
      bus_->write(addrDirect(), B);
      updateNZ8(B);
      setFlag(FlagV, false);
      cycles = 4;
      break;
    case 0xD8:
      B = opEor8(B, bus_->read(addrDirect()));
      cycles = 4;
      break;
    case 0xD9:
      B = opAdc8(B, bus_->read(addrDirect()));
      cycles = 4;
      break;
    case 0xDA:
      B = opOr8(B, bus_->read(addrDirect()));
      cycles = 4;
      break;
    case 0xDB:
      B = opAdd8(B, bus_->read(addrDirect()));
      cycles = 4;
      break;
    case 0xDC: {
      uint16_t v = bus_->read16(addrDirect());
      setD(v);
      updateNZ16(v);
      setFlag(FlagV, false);
      cycles = 5;
      break;
    }
    case 0xDD: {
      uint16_t v = getD();
      bus_->write16(addrDirect(), v);
      updateNZ16(v);
      setFlag(FlagV, false);
      cycles = 5;
      break;
    }
    case 0xDE:
      U = bus_->read16(addrDirect());
      updateNZ16(U);
      setFlag(FlagV, false);
      cycles = 5;
      break;
    case 0xDF:
      bus_->write16(addrDirect(), U);
      updateNZ16(U);
      setFlag(FlagV, false);
      cycles = 5;
      break;

    // --- Accumulator B / 16-bit Indexed ($E0 - $EF) ---
    case 0xE0: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      B = opSub8(B, bus_->read(ea));
      cycles = 4 + ex;
      break;
    }
    case 0xE1: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      opCmp8(B, bus_->read(ea));
      cycles = 4 + ex;
      break;
    }
    case 0xE2: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      B = opSbc8(B, bus_->read(ea));
      cycles = 4 + ex;
      break;
    }
    case 0xE3: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      setD(opAdd16(getD(), bus_->read16(ea)));
      cycles = 6 + ex;
      break;
    }
    case 0xE4: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      B = opAnd8(B, bus_->read(ea));
      cycles = 4 + ex;
      break;
    }
    case 0xE5: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      (void)opAnd8(B, bus_->read(ea));
      cycles = 4 + ex;
      break;
    }
    case 0xE6: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      B = bus_->read(ea);
      updateNZ8(B);
      setFlag(FlagV, false);
      cycles = 4 + ex;
      break;
    }
    case 0xE7: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      bus_->write(ea, B);
      updateNZ8(B);
      setFlag(FlagV, false);
      cycles = 4 + ex;
      break;
    }
    case 0xE8: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      B = opEor8(B, bus_->read(ea));
      cycles = 4 + ex;
      break;
    }
    case 0xE9: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      B = opAdc8(B, bus_->read(ea));
      cycles = 4 + ex;
      break;
    }
    case 0xEA: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      B = opOr8(B, bus_->read(ea));
      cycles = 4 + ex;
      break;
    }
    case 0xEB: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      B = opAdd8(B, bus_->read(ea));
      cycles = 4 + ex;
      break;
    }
    case 0xEC: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      uint16_t v = bus_->read16(ea);
      setD(v);
      updateNZ16(v);
      setFlag(FlagV, false);
      cycles = 5 + ex;
      break;
    }
    case 0xED: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      uint16_t v = getD();
      bus_->write16(ea, v);
      updateNZ16(v);
      setFlag(FlagV, false);
      cycles = 5 + ex;
      break;
    }
    case 0xEE: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      U = bus_->read16(ea);
      updateNZ16(U);
      setFlag(FlagV, false);
      cycles = 5 + ex;
      break;
    }
    case 0xEF: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      bus_->write16(ea, U);
      updateNZ16(U);
      setFlag(FlagV, false);
      cycles = 5 + ex;
      break;
    }

    // --- Accumulator B / 16-bit Extended ($F0 - $FF) ---
    case 0xF0:
      B = opSub8(B, bus_->read(addrExtended()));
      cycles = 5;
      break;
    case 0xF1:
      opCmp8(B, bus_->read(addrExtended()));
      cycles = 5;
      break;
    case 0xF2:
      B = opSbc8(B, bus_->read(addrExtended()));
      cycles = 5;
      break;
    case 0xF3:
      setD(opAdd16(getD(), bus_->read16(addrExtended())));
      cycles = 7;
      break;
    case 0xF4:
      B = opAnd8(B, bus_->read(addrExtended()));
      cycles = 5;
      break;
    case 0xF5:
      (void)opAnd8(B, bus_->read(addrExtended()));
      cycles = 5;
      break;
    case 0xF6:
      B = bus_->read(addrExtended());
      updateNZ8(B);
      setFlag(FlagV, false);
      cycles = 5;
      break;
    case 0xF7:
      bus_->write(addrExtended(), B);
      updateNZ8(B);
      setFlag(FlagV, false);
      cycles = 5;
      break;
    case 0xF8:
      B = opEor8(B, bus_->read(addrExtended()));
      cycles = 5;
      break;
    case 0xF9:
      B = opAdc8(B, bus_->read(addrExtended()));
      cycles = 5;
      break;
    case 0xFA:
      B = opOr8(B, bus_->read(addrExtended()));
      cycles = 5;
      break;
    case 0xFB:
      B = opAdd8(B, bus_->read(addrExtended()));
      cycles = 5;
      break;
    case 0xFC: {
      uint16_t v = bus_->read16(addrExtended());
      setD(v);
      updateNZ16(v);
      setFlag(FlagV, false);
      cycles = 6;
      break;
    }
    case 0xFD: {
      uint16_t v = getD();
      bus_->write16(addrExtended(), v);
      updateNZ16(v);
      setFlag(FlagV, false);
      cycles = 6;
      break;
    }
    case 0xFE:
      U = bus_->read16(addrExtended());
      updateNZ16(U);
      setFlag(FlagV, false);
      cycles = 6;
      break;
    case 0xFF:
      bus_->write16(addrExtended(), U);
      updateNZ16(U);
      setFlag(FlagV, false);
      cycles = 6;
      break;

    default:
      cycles = 2;
      break;
  }

  Cycles += uint64_t(cycles);
  return cycles;
}

int Cpu6809::stepPage2() {
  uint8_t op = fetch8();
  switch (op) {
    case 0x21:  // LBRN
      (void)fetch16();
      return 5;
    case 0x22: {  // LBHI
      int16_t off = int16_t(fetch16());
      if (!getFlag(FlagC) && !getFlag(FlagZ)) {
        PC = uint16_t(int32_t(PC) + int32_t(off));
        return 6;
      }
      return 5;
    }
    case 0x23: {  // LBLS
      int16_t off = int16_t(fetch16());
      if (getFlag(FlagC) || getFlag(FlagZ)) {
        PC = uint16_t(int32_t(PC) + int32_t(off));
        return 6;
      }
      return 5;
    }
    case 0x24: {  // LBCC / LBHS
      int16_t off = int16_t(fetch16());
      if (!getFlag(FlagC)) {
        PC = uint16_t(int32_t(PC) + int32_t(off));
        return 6;
      }
      return 5;
    }
    case 0x25: {  // LBCS / LBLO
      int16_t off = int16_t(fetch16());
      if (getFlag(FlagC)) {
        PC = uint16_t(int32_t(PC) + int32_t(off));
        return 6;
      }
      return 5;
    }
    case 0x26: {  // LBNE
      int16_t off = int16_t(fetch16());
      if (!getFlag(FlagZ)) {
        PC = uint16_t(int32_t(PC) + int32_t(off));
        return 6;
      }
      return 5;
    }
    case 0x27: {  // LBEQ
      int16_t off = int16_t(fetch16());
      if (getFlag(FlagZ)) {
        PC = uint16_t(int32_t(PC) + int32_t(off));
        return 6;
      }
      return 5;
    }
    case 0x28: {  // LBVC
      int16_t off = int16_t(fetch16());
      if (!getFlag(FlagV)) {
        PC = uint16_t(int32_t(PC) + int32_t(off));
        return 6;
      }
      return 5;
    }
    case 0x29: {  // LBVS
      int16_t off = int16_t(fetch16());
      if (getFlag(FlagV)) {
        PC = uint16_t(int32_t(PC) + int32_t(off));
        return 6;
      }
      return 5;
    }
    case 0x2A: {  // LBPL
      int16_t off = int16_t(fetch16());
      if (!getFlag(FlagN)) {
        PC = uint16_t(int32_t(PC) + int32_t(off));
        return 6;
      }
      return 5;
    }
    case 0x2B: {  // LBMI
      int16_t off = int16_t(fetch16());
      if (getFlag(FlagN)) {
        PC = uint16_t(int32_t(PC) + int32_t(off));
        return 6;
      }
      return 5;
    }
    case 0x2C: {  // LBGE
      int16_t off = int16_t(fetch16());
      if (getFlag(FlagN) == getFlag(FlagV)) {
        PC = uint16_t(int32_t(PC) + int32_t(off));
        return 6;
      }
      return 5;
    }
    case 0x2D: {  // LBLT
      int16_t off = int16_t(fetch16());
      if (getFlag(FlagN) != getFlag(FlagV)) {
        PC = uint16_t(int32_t(PC) + int32_t(off));
        return 6;
      }
      return 5;
    }
    case 0x2E: {  // LBGT
      int16_t off = int16_t(fetch16());
      if (!getFlag(FlagZ) && (getFlag(FlagN) == getFlag(FlagV))) {
        PC = uint16_t(int32_t(PC) + int32_t(off));
        return 6;
      }
      return 5;
    }
    case 0x2F: {  // LBLE
      int16_t off = int16_t(fetch16());
      if (getFlag(FlagZ) || (getFlag(FlagN) != getFlag(FlagV))) {
        PC = uint16_t(int32_t(PC) + int32_t(off));
        return 6;
      }
      return 5;
    }
    case 0x3F:  // SWI2 (OS-9 System Call!)
      setFlag(FlagE, true);
      pushS(0xFF);
      PC = bus_->read16(0xFFF4);
      return 20;
    case 0x83:
      opCmp16(getD(), fetch16());
      return 5;
    case 0x8C:
      opCmp16(Y, fetch16());
      return 5;
    case 0x8E:
      Y = fetch16();
      updateNZ16(Y);
      setFlag(FlagV, false);
      return 4;
    case 0x93:
      opCmp16(getD(), bus_->read16(addrDirect()));
      return 7;
    case 0x9C:
      opCmp16(Y, bus_->read16(addrDirect()));
      return 7;
    case 0x9E:
      Y = bus_->read16(addrDirect());
      updateNZ16(Y);
      setFlag(FlagV, false);
      return 6;
    case 0x9F:
      bus_->write16(addrDirect(), Y);
      updateNZ16(Y);
      setFlag(FlagV, false);
      return 6;
    case 0xA3: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      opCmp16(getD(), bus_->read16(ea));
      return 7 + ex;
    }
    case 0xAC: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      opCmp16(Y, bus_->read16(ea));
      return 7 + ex;
    }
    case 0xAE: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      Y = bus_->read16(ea);
      updateNZ16(Y);
      setFlag(FlagV, false);
      return 6 + ex;
    }
    case 0xAF: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      bus_->write16(ea, Y);
      updateNZ16(Y);
      setFlag(FlagV, false);
      return 6 + ex;
    }
    case 0xB3:
      opCmp16(getD(), bus_->read16(addrExtended()));
      return 8;
    case 0xBC:
      opCmp16(Y, bus_->read16(addrExtended()));
      return 8;
    case 0xBE:
      Y = bus_->read16(addrExtended());
      updateNZ16(Y);
      setFlag(FlagV, false);
      return 7;
    case 0xBF:
      bus_->write16(addrExtended(), Y);
      updateNZ16(Y);
      setFlag(FlagV, false);
      return 7;
    case 0xCE:
      S = fetch16();
      sInitialized_ = true;
      updateNZ16(S);
      setFlag(FlagV, false);
      return 4;
    case 0xDE:
      S = bus_->read16(addrDirect());
      sInitialized_ = true;
      updateNZ16(S);
      setFlag(FlagV, false);
      return 6;
    case 0xDF:
      bus_->write16(addrDirect(), S);
      updateNZ16(S);
      setFlag(FlagV, false);
      return 6;
    case 0xEE: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      S = bus_->read16(ea);
      sInitialized_ = true;
      updateNZ16(S);
      setFlag(FlagV, false);
      return 6 + ex;
    }
    case 0xEF: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      bus_->write16(ea, S);
      updateNZ16(S);
      setFlag(FlagV, false);
      return 6 + ex;
    }
    case 0xFE:
      S = bus_->read16(addrExtended());
      sInitialized_ = true;
      updateNZ16(S);
      setFlag(FlagV, false);
      return 7;
    case 0xFF:
      bus_->write16(addrExtended(), S);
      updateNZ16(S);
      setFlag(FlagV, false);
      return 7;
    default:
      return 2;
  }
}

int Cpu6809::stepPage3() {
  uint8_t op = fetch8();
  switch (op) {
    case 0x3F:  // SWI3
      setFlag(FlagE, true);
      pushS(0xFF);
      PC = bus_->read16(0xFFF2);
      return 20;
    case 0x83:
      opCmp16(U, fetch16());
      return 5;
    case 0x8C:
      opCmp16(S, fetch16());
      return 5;
    case 0x93:
      opCmp16(U, bus_->read16(addrDirect()));
      return 7;
    case 0x9C:
      opCmp16(S, bus_->read16(addrDirect()));
      return 7;
    case 0xA3: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      opCmp16(U, bus_->read16(ea));
      return 7 + ex;
    }
    case 0xAC: {
      int ex = 0;
      uint16_t ea = addrIndexed(ex);
      opCmp16(S, bus_->read16(ea));
      return 7 + ex;
    }
    case 0xB3:
      opCmp16(U, bus_->read16(addrExtended()));
      return 8;
    case 0xBC:
      opCmp16(S, bus_->read16(addrExtended()));
      return 8;
    default:
      return 2;
  }
}
