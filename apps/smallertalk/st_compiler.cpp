#include "st_compiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum TokenType : uint8_t {
  TOK_EOF = 0,
  TOK_IDENT,        // foo, Object, self
  TOK_KEYWORD,      // at:, put:
  TOK_BLOCK_ARG,    // :x
  TOK_BINOP,        // +, -, <=, ~=, |, etc.
  TOK_ASSIGN,       // := or _
  TOK_RETURN,       // ^
  TOK_PERIOD,       // .
  TOK_CASCADE,      // ;
  TOK_LPAREN,       // (
  TOK_RPAREN,       // )
  TOK_LBRACK,       // [
  TOK_RBRACK,       // ]
  TOK_LBRACE,       // {
  TOK_RBRACE,       // }
  TOK_INT,          // 42, -7, 16rFF
  TOK_FLOAT,        // 3.14
  TOK_CHAR,         // $a
  TOK_STRING,       // 'hello'
  TOK_SYMBOL,       // #foo, #at:put:, #+, #'hello world'
  TOK_HASH_LPAREN,  // #(
  TOK_HASH_LBRACK,  // #[
  TOK_ERROR
};

static inline bool isAlpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static inline bool isDigit(char c) {
  return (c >= '0' && c <= '9');
}
static inline bool isAlnum(char c) {
  return isAlpha(c) || isDigit(c);
}
static inline bool isBinOpChar(char c) {
  switch (c) {
    case '+': case '-': case '*': case '/': case '\\':
    case '~': case '<': case '>': case '=': case '@':
    case '%': case '|': case '&': case '?': case ',':
    case '!':
      return true;
    default:
      return false;
  }
}

struct Token {
  TokenType type;
  char      text[128];
  int32_t   intVal;
  float     floatVal;
  uint16_t  line;
};

struct Compiler {
  VMState*    vm;
  const char* src;
  size_t      pos;
  uint16_t    line;
  bool        lastWasValue;

  Token       cur;
  bool        hasError;
  char        errMsg[128];

  Oop         clsOop;
  bool        isClassMethod;
  uint16_t    primId;

  // Lexical Temporary / Argument Table (shared across homeContext)
  // Each scope pushes entries and pops at end of block, while `maxTempSlots`
  // tracks total distinct slots allocated in `homeContext`.
  struct VarEntry {
    Oop      sym;
    uint8_t  slotIdx;
    uint8_t  scopeDepth;
  };
  VarEntry    vars[48];
  uint8_t     varCount;
  uint8_t     maxTempSlots;
  uint8_t     numMethodArgs;
  uint8_t     scopeDepth;

  // Literals Table
  Oop         literals[64];
  uint8_t     literalCount;

  // Bytecode Buffer
  uint8_t     code[512];
  uint16_t    codeLen;
};

static void setError(Compiler* c, const char* msg) {
  if (c->hasError) return;
  c->hasError = true;
  snprintf(c->errMsg, sizeof(c->errMsg), "Line %u: %s", (unsigned)c->line, msg);
  if (c->vm) {
    strncpy(c->vm->lastError, c->errMsg, sizeof(c->vm->lastError) - 1);
    c->vm->lastError[sizeof(c->vm->lastError) - 1] = '\0';
  }
}

static void skipWhitespaceAndComments(Compiler* c) {
  while (c->src[c->pos] != '\0') {
    char ch = c->src[c->pos];
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
      if (ch == '\n') c->line++;
      c->pos++;
    } else if (ch == '"') {
      c->pos++;
      while (c->src[c->pos] != '\0' && c->src[c->pos] != '"') {
        if (c->src[c->pos] == '\n') c->line++;
        c->pos++;
      }
      if (c->src[c->pos] == '"') {
        c->pos++;
      } else {
        setError(c, "Unterminated comment");
        return;
      }
    } else {
      break;
    }
  }
}

static void nextToken(Compiler* c) {
  skipWhitespaceAndComments(c);
  memset(&c->cur, 0, sizeof(c->cur));
  c->cur.line = c->line;

  char ch = c->src[c->pos];
  if (ch == '\0') {
    c->cur.type = TOK_EOF;
    c->lastWasValue = false;
    return;
  }

  // Single-character punctuation
  if (ch == '(') { c->pos++; c->cur.type = TOK_LPAREN;  c->lastWasValue = false; return; }
  if (ch == ')') { c->pos++; c->cur.type = TOK_RPAREN;  c->lastWasValue = true;  return; }
  if (ch == '[') { c->pos++; c->cur.type = TOK_LBRACK;  c->lastWasValue = false; return; }
  if (ch == ']') { c->pos++; c->cur.type = TOK_RBRACK;  c->lastWasValue = true;  return; }
  if (ch == '{') { c->pos++; c->cur.type = TOK_LBRACE;  c->lastWasValue = false; return; }
  if (ch == '}') { c->pos++; c->cur.type = TOK_RBRACE;  c->lastWasValue = true;  return; }
  if (ch == '^') { c->pos++; c->cur.type = TOK_RETURN;  c->lastWasValue = false; return; }
  if (ch == '.') { c->pos++; c->cur.type = TOK_PERIOD;  c->lastWasValue = false; return; }
  if (ch == ';') { c->pos++; c->cur.type = TOK_CASCADE; c->lastWasValue = false; return; }

  // Assignment `:=` or block arg `:var`
  if (ch == ':') {
    if (c->src[c->pos + 1] == '=') {
      c->pos += 2;
      c->cur.type = TOK_ASSIGN;
      strcpy(c->cur.text, ":=");
      c->lastWasValue = false;
      return;
    }
    c->pos++;
    while (c->src[c->pos] == ' ' || c->src[c->pos] == '\t') c->pos++;
    if (!isAlpha(c->src[c->pos]) && c->src[c->pos] != '_') {
      setError(c, "Expected identifier after ':'");
      c->cur.type = TOK_ERROR;
      return;
    }
    size_t len = 0;
    while (isAlnum(c->src[c->pos]) || c->src[c->pos] == '_') {
      if (len + 1 < sizeof(c->cur.text)) c->cur.text[len++] = c->src[c->pos];
      c->pos++;
    }
    c->cur.text[len] = '\0';
    c->cur.type = TOK_BLOCK_ARG;
    c->lastWasValue = false;
    return;
  }

  // Character literal `$x`
  if (ch == '$') {
    c->pos++;
    if (c->src[c->pos] == '\0') {
      setError(c, "Unexpected EOF after '$'");
      c->cur.type = TOK_ERROR;
      return;
    }
    c->cur.intVal = (uint8_t)c->src[c->pos++];
    c->cur.type   = TOK_CHAR;
    c->lastWasValue = true;
    return;
  }

  // String literal `'...'` (with `''` escaping)
  if (ch == '\'') {
    c->pos++;
    size_t len = 0;
    while (c->src[c->pos] != '\0') {
      if (c->src[c->pos] == '\'') {
        if (c->src[c->pos + 1] == '\'') {
          if (len + 1 < sizeof(c->cur.text)) c->cur.text[len++] = '\'';
          c->pos += 2;
          continue;
        }
        c->pos++;
        c->cur.text[len] = '\0';
        c->cur.type = TOK_STRING;
        c->lastWasValue = true;
        return;
      }
      if (c->src[c->pos] == '\n') c->line++;
      if (len + 1 < sizeof(c->cur.text)) c->cur.text[len++] = c->src[c->pos];
      c->pos++;
    }
    setError(c, "Unterminated string literal");
    c->cur.type = TOK_ERROR;
    return;
  }

  // Symbol or Literal Array `#...`
  if (ch == '#') {
    c->pos++;
    char next = c->src[c->pos];
    if (next == '(') {
      c->pos++;
      c->cur.type = TOK_HASH_LPAREN;
      c->lastWasValue = false;
      return;
    }
    if (next == '[') {
      c->pos++;
      c->cur.type = TOK_HASH_LBRACK;
      c->lastWasValue = false;
      return;
    }
    if (next == '\'') {
      c->pos++;
      size_t len = 0;
      while (c->src[c->pos] != '\0') {
        if (c->src[c->pos] == '\'') {
          if (c->src[c->pos + 1] == '\'') {
            if (len + 1 < sizeof(c->cur.text)) c->cur.text[len++] = '\'';
            c->pos += 2;
            continue;
          }
          c->pos++;
          break;
        }
        if (len + 1 < sizeof(c->cur.text)) c->cur.text[len++] = c->src[c->pos];
        c->pos++;
      }
      c->cur.text[len] = '\0';
      c->cur.type = TOK_SYMBOL;
      c->lastWasValue = true;
      return;
    }
    if (isBinOpChar(next)) {
      size_t len = 0;
      while (isBinOpChar(c->src[c->pos])) {
        if (len + 1 < sizeof(c->cur.text)) c->cur.text[len++] = c->src[c->pos];
        c->pos++;
      }
      c->cur.text[len] = '\0';
      c->cur.type = TOK_SYMBOL;
      c->lastWasValue = true;
      return;
    }
    // Identifier or multi-keyword symbol `#at:put:`
    size_t len = 0;
    while (isAlnum(c->src[c->pos]) || c->src[c->pos] == '_' || c->src[c->pos] == ':') {
      if (len + 1 < sizeof(c->cur.text)) c->cur.text[len++] = c->src[c->pos];
      c->pos++;
    }
    c->cur.text[len] = '\0';
    c->cur.type = TOK_SYMBOL;
    c->lastWasValue = true;
    return;
  }

  // Number literal (or negative number if `!c->lastWasValue && ch == '-' && isDigit(next)`)
  if (isDigit(ch) || (!c->lastWasValue && ch == '-' && isDigit(c->src[c->pos + 1]))) {
    bool neg = false;
    if (ch == '-') {
      neg = true;
      c->pos++;
    }
    int32_t intAccum = 0;
    while (isDigit(c->src[c->pos])) {
      intAccum = intAccum * 10 + (c->src[c->pos] - '0');
      c->pos++;
    }
    // Check for radix literal `16rFF`
    if (c->src[c->pos] == 'r' || c->src[c->pos] == 'R') {
      int32_t base = intAccum;
      if (base < 2 || base > 36) base = 10;
      c->pos++;
      int32_t radVal = 0;
      while (isAlnum(c->src[c->pos])) {
        char d = c->src[c->pos];
        int dv = -1;
        if (d >= '0' && d <= '9') dv = d - '0';
        else if (d >= 'A' && d <= 'Z') dv = 10 + (d - 'A');
        else if (d >= 'a' && d <= 'z') dv = 10 + (d - 'a');
        if (dv < 0 || dv >= base) break;
        radVal = radVal * base + dv;
        c->pos++;
      }
      c->cur.intVal = neg ? -radVal : radVal;
      c->cur.type   = TOK_INT;
      c->lastWasValue = true;
      return;
    }
    // Check for decimal point followed by digit (`3.14` vs `3.` at end of statement)
    if (c->src[c->pos] == '.' && isDigit(c->src[c->pos + 1])) {
      c->pos++;
      float fval = (float)intAccum;
      float scale = 0.1f;
      while (isDigit(c->src[c->pos])) {
        fval += (c->src[c->pos] - '0') * scale;
        scale *= 0.1f;
        c->pos++;
      }
      if (c->src[c->pos] == 'e' || c->src[c->pos] == 'E') {
        c->pos++;
        bool expNeg = false;
        if (c->src[c->pos] == '-') { expNeg = true; c->pos++; }
        else if (c->src[c->pos] == '+') { c->pos++; }
        int expVal = 0;
        while (isDigit(c->src[c->pos])) {
          expVal = expVal * 10 + (c->src[c->pos] - '0');
          c->pos++;
        }
        float mul = 1.0f;
        for (int i = 0; i < expVal; i++) mul *= 10.0f;
        fval = expNeg ? (fval / mul) : (fval * mul);
      }
      c->cur.floatVal = neg ? -fval : fval;
      c->cur.type     = TOK_FLOAT;
      c->lastWasValue = true;
      return;
    }
    c->cur.intVal = neg ? -intAccum : intAccum;
    c->cur.type   = TOK_INT;
    c->lastWasValue = true;
    return;
  }

  // Identifier or Keyword (`foo` vs `foo:`) or standalone `_` assignment
  if (isAlpha(ch) || ch == '_') {
    size_t len = 0;
    while (isAlnum(c->src[c->pos]) || c->src[c->pos] == '_') {
      if (len + 1 < sizeof(c->cur.text)) c->cur.text[len++] = c->src[c->pos];
      c->pos++;
    }
    c->cur.text[len] = '\0';
    if (len == 1 && c->cur.text[0] == '_') {
      c->cur.type = TOK_ASSIGN;
      c->lastWasValue = false;
      return;
    }
    if (c->src[c->pos] == ':' && c->src[c->pos + 1] != '=') {
      if (len + 1 < sizeof(c->cur.text)) c->cur.text[len++] = ':';
      c->cur.text[len] = '\0';
      c->pos++;
      c->cur.type = TOK_KEYWORD;
      c->lastWasValue = false;
      return;
    }
    c->cur.type = TOK_IDENT;
    c->lastWasValue = true;
    return;
  }

  // Binary operator (`+`, `-`, `<=`, `~=`, `|`, etc.)
  if (isBinOpChar(ch)) {
    size_t len = 0;
    while (isBinOpChar(c->src[c->pos])) {
      if (len + 1 < sizeof(c->cur.text)) c->cur.text[len++] = c->src[c->pos];
      c->pos++;
    }
    c->cur.text[len] = '\0';
    c->cur.type = TOK_BINOP;
    c->lastWasValue = false;
    return;
  }

  setError(c, "Unexpected character");
  c->pos++;
  c->cur.type = TOK_ERROR;
}

// ---------------------------------------------------------------------------
// Bytecode & Literal Emission Helpers
// ---------------------------------------------------------------------------
static void emitByte(Compiler* c, uint8_t b) {
  if (c->codeLen < sizeof(c->code)) {
    c->code[c->codeLen++] = b;
  } else {
    setError(c, "Method bytecode too large");
  }
}

static void emitBytes2(Compiler* c, uint8_t a, uint8_t b) {
  emitByte(c, a);
  emitByte(c, b);
}

static void emitBytes3(Compiler* c, uint8_t a, uint8_t b, uint8_t d) {
  emitByte(c, a);
  emitByte(c, b);
  emitByte(c, d);
}

static void insertByteAt(Compiler* c, uint16_t insertPos, uint8_t b) {
  if (c->codeLen + 1 >= sizeof(c->code) || insertPos > c->codeLen) {
    setError(c, "Method bytecode too large for cascade");
    return;
  }
  memmove(&c->code[insertPos + 1], &c->code[insertPos], c->codeLen - insertPos);
  c->code[insertPos] = b;
  c->codeLen++;
}

static uint8_t addLiteral(Compiler* c, Oop lit) {
  for (uint8_t i = 0; i < c->literalCount; i++) {
    if (c->literals[i] == lit) return i;
  }
  if (c->literalCount >= 64) {
    setError(c, "Too many literals in method");
    return 0;
  }
  uint8_t idx = c->literalCount++;
  c->literals[idx] = lit;
  return idx;
}

static void emitPushSmallInt(Compiler* c, int32_t val) {
  if (val >= -128 && val <= 127) {
    emitBytes2(c, OP_PUSH_SMALLINT_8, (uint8_t)(int8_t)val);
  } else if (val >= -32768 && val <= 32767) {
    uint16_t u = (uint16_t)(int16_t)val;
    emitBytes3(c, OP_PUSH_SMALLINT_16, (uint8_t)(u & 0xFF), (uint8_t)(u >> 8));
  } else {
    uint8_t litIdx = addLiteral(c, makeSmallInt(val));
    emitBytes2(c, OP_PUSH_LITERAL, litIdx);
  }
}

static uint8_t addLocalVar(Compiler* c, const char* name) {
  Oop sym = stInternSymbol(c->vm, name);
  if (c->varCount >= 48 || c->maxTempSlots >= 48) {
    setError(c, "Too many temporary variables");
    return 0;
  }
  uint8_t slotIdx = c->maxTempSlots++;
  Compiler::VarEntry& ve = c->vars[c->varCount++];
  ve.sym        = sym;
  ve.slotIdx    = slotIdx;
  ve.scopeDepth = c->scopeDepth;
  return slotIdx;
}

static int findLocalVar(Compiler* c, Oop sym) {
  for (int i = (int)c->varCount - 1; i >= 0; i--) {
    if (c->vars[i].sym == sym) {
      return c->vars[i].slotIdx;
    }
  }
  return -1;
}

static void emitPushVariable(Compiler* c, const char* name) {
  if (strcmp(name, "self") == 0 || strcmp(name, "super") == 0) {
    emitByte(c, OP_PUSH_SELF);
    return;
  }
  if (strcmp(name, "nil") == 0) {
    emitByte(c, OP_PUSH_NIL);
    return;
  }
  if (strcmp(name, "true") == 0) {
    emitByte(c, OP_PUSH_TRUE);
    return;
  }
  if (strcmp(name, "false") == 0) {
    emitByte(c, OP_PUSH_FALSE);
    return;
  }
  if (strcmp(name, "thisContext") == 0) {
    emitByte(c, OP_PUSH_CONTEXT);
    return;
  }

  Oop sym = stInternSymbol(c->vm, name);
  int tempSlot = findLocalVar(c, sym);
  if (tempSlot >= 0) {
    emitBytes2(c, OP_PUSH_TEMP, (uint8_t)tempSlot);
    return;
  }

  if (!c->isClassMethod && c->clsOop != kOopNil) {
    int ivarIdx = stFindInstVarIndex(c->vm, c->clsOop, sym);
    if (ivarIdx >= 0) {
      emitBytes2(c, OP_PUSH_INST, (uint8_t)ivarIdx);
      return;
    }
  }

  uint8_t litIdx = addLiteral(c, sym);
  emitBytes2(c, OP_PUSH_GLOBAL, litIdx);
}

static void emitStoreVariable(Compiler* c, Oop sym) {
  int tempSlot = findLocalVar(c, sym);
  if (tempSlot >= 0) {
    emitBytes2(c, OP_STORE_TEMP, (uint8_t)tempSlot);
    return;
  }
  if (!c->isClassMethod && c->clsOop != kOopNil) {
    int ivarIdx = stFindInstVarIndex(c->vm, c->clsOop, sym);
    if (ivarIdx >= 0) {
      emitBytes2(c, OP_STORE_INST, (uint8_t)ivarIdx);
      return;
    }
  }
  uint8_t litIdx = addLiteral(c, sym);
  emitBytes2(c, OP_STORE_GLOBAL, litIdx);
}

// Forward declarations for recursive-descent grammar
static void parseExpression(Compiler* c);
static void parseStatementList(Compiler* c, bool isBlock, bool isDoIt);

static Oop parseLiteralArrayElement(Compiler* c);

static Oop parseLiteralArrayBody(Compiler* c) {
  Oop elems[32];
  uint16_t count = 0;
  while (c->cur.type != TOK_RPAREN && c->cur.type != TOK_EOF && !c->hasError) {
    if (count < 32) {
      elems[count++] = parseLiteralArrayElement(c);
    } else {
      setError(c, "Literal array too long");
      break;
    }
  }
  if (c->cur.type == TOK_RPAREN) {
    nextToken(c);
  } else {
    setError(c, "Expected ')' at end of literal array");
  }
  Oop arr = stNewArray(c->vm, count);
  for (uint16_t i = 0; i < count; i++) {
    stSlotAtPut(c->vm, arr, i, elems[i]);
  }
  return arr;
}

static Oop parseLiteralArrayElement(Compiler* c) {
  switch (c->cur.type) {
    case TOK_INT: {
      Oop v = makeSmallInt(c->cur.intVal);
      nextToken(c);
      return v;
    }
    case TOK_FLOAT: {
      Oop v = stNewFloat(c->vm, c->cur.floatVal);
      nextToken(c);
      return v;
    }
    case TOK_CHAR: {
      Oop v = makeChar((uint8_t)c->cur.intVal);
      nextToken(c);
      return v;
    }
    case TOK_STRING: {
      Oop v = stNewString(c->vm, c->cur.text);
      nextToken(c);
      return v;
    }
    case TOK_SYMBOL:
    case TOK_IDENT:
    case TOK_KEYWORD:
    case TOK_BINOP: {
      if (c->cur.type == TOK_IDENT) {
        if (strcmp(c->cur.text, "nil") == 0)   { nextToken(c); return kOopNil; }
        if (strcmp(c->cur.text, "true") == 0)  { nextToken(c); return kOopTrue; }
        if (strcmp(c->cur.text, "false") == 0) { nextToken(c); return kOopFalse; }
      }
      Oop v = stInternSymbol(c->vm, c->cur.text);
      nextToken(c);
      return v;
    }
    case TOK_LPAREN:
    case TOK_HASH_LPAREN: {
      nextToken(c);
      return parseLiteralArrayBody(c);
    }
    default:
      setError(c, "Invalid element in literal array");
      nextToken(c);
      return kOopNil;
  }
}

static Oop parseLiteralByteArray(Compiler* c) {
  uint8_t bytes[128];
  uint16_t count = 0;
  while (c->cur.type != TOK_RBRACK && c->cur.type != TOK_EOF && !c->hasError) {
    if (c->cur.type == TOK_INT && c->cur.intVal >= 0 && c->cur.intVal <= 255) {
      if (count < sizeof(bytes)) bytes[count++] = (uint8_t)c->cur.intVal;
      nextToken(c);
    } else {
      setError(c, "ByteArray #[...] expects integers 0..255");
      break;
    }
  }
  if (c->cur.type == TOK_RBRACK) {
    nextToken(c);
  } else {
    setError(c, "Expected ']' after ByteArray literal");
  }
  return stNewBytes(c->vm, CLS_BYTE_ARRAY, FMT_BYTES, bytes, count);
}

static void parseBlock(Compiler* c) {
  // Current token is `[`
  nextToken(c);
  c->scopeDepth++;
  uint8_t savedVarCount = c->varCount;
  uint8_t tempBase = c->maxTempSlots;
  uint8_t numBlockArgs = 0;

  // Optional block arguments `:x :y |`
  if (c->cur.type == TOK_BLOCK_ARG) {
    while (c->cur.type == TOK_BLOCK_ARG && !c->hasError) {
      addLocalVar(c, c->cur.text);
      numBlockArgs++;
      nextToken(c);
    }
    if (c->cur.type == TOK_BINOP && strcmp(c->cur.text, "|") == 0) {
      c->lastWasValue = false;
      nextToken(c);
    } else if (c->cur.type == TOK_BINOP && c->cur.text[0] == '|' && c->cur.text[1] == '|') {
      // Handle `[:x || t | ...]` where `||` was lexed together as one binop token!
      // Consuming the first `|` leaves the second `|` as the start of block temps!
      memmove(c->cur.text, c->cur.text + 1, strlen(c->cur.text));
    } else {
      setError(c, "Expected '|' after block arguments");
    }
  }

  // Optional block-local temporaries `| t1 t2 |`
  if (c->cur.type == TOK_BINOP && strcmp(c->cur.text, "|") == 0) {
    c->lastWasValue = false;
    nextToken(c);
    while (c->cur.type == TOK_IDENT && !c->hasError) {
      addLocalVar(c, c->cur.text);
      nextToken(c);
    }
    if (c->cur.type == TOK_BINOP && strcmp(c->cur.text, "|") == 0) {
      c->lastWasValue = false;
      nextToken(c);
    } else {
      setError(c, "Expected closing '|' after block temporaries");
    }
  } else if (c->cur.type == TOK_BINOP && strcmp(c->cur.text, "||") == 0) {
    // Empty block temporaries `||`
    c->lastWasValue = false;
    nextToken(c);
  }

  // Emit OP_BLOCK_COPY: [OP_BLOCK_COPY, numArgs, tempBase, bodyLenLo, bodyLenHi]
  emitByte(c, OP_BLOCK_COPY);
  emitByte(c, numBlockArgs);
  emitByte(c, tempBase);
  uint16_t patchPos = c->codeLen;
  emitBytes2(c, 0, 0);
  uint16_t bodyStart = c->codeLen;

  if (c->cur.type == TOK_RBRACK) {
    // Empty block `[]` returns `nil`
    emitByte(c, OP_PUSH_NIL);
    emitByte(c, OP_BLOCK_RETURN);
    nextToken(c);
  } else {
    parseStatementList(c, /*isBlock=*/true, /*isDoIt=*/false);
    if (c->cur.type == TOK_RBRACK) {
      nextToken(c);
    } else {
      setError(c, "Expected ']' at end of block");
    }
  }

  uint16_t bodyLen = c->codeLen - bodyStart;
  c->code[patchPos]     = (uint8_t)(bodyLen & 0xFF);
  c->code[patchPos + 1] = (uint8_t)(bodyLen >> 8);

  c->varCount = savedVarCount;
  c->scopeDepth--;
}

static void parseDynamicArray(Compiler* c) {
  // Current token is `{`
  nextToken(c);
  uint8_t count = 0;
  while (c->cur.type != TOK_RBRACE && c->cur.type != TOK_EOF && !c->hasError) {
    parseExpression(c);
    count++;
    if (c->cur.type == TOK_PERIOD) {
      nextToken(c);
    } else {
      break;
    }
  }
  if (c->cur.type == TOK_RBRACE) {
    nextToken(c);
  } else {
    setError(c, "Expected '}' at end of dynamic array");
  }
  emitBytes2(c, OP_MAKE_ARRAY, count);
}

static void parsePrimary(Compiler* c, bool* outIsSuper) {
  *outIsSuper = false;
  switch (c->cur.type) {
    case TOK_INT:
      emitPushSmallInt(c, c->cur.intVal);
      nextToken(c);
      break;
    case TOK_FLOAT: {
      Oop flt = stNewFloat(c->vm, c->cur.floatVal);
      uint8_t idx = addLiteral(c, flt);
      emitBytes2(c, OP_PUSH_LITERAL, idx);
      nextToken(c);
      break;
    }
    case TOK_CHAR: {
      uint8_t idx = addLiteral(c, makeChar((uint8_t)c->cur.intVal));
      emitBytes2(c, OP_PUSH_LITERAL, idx);
      nextToken(c);
      break;
    }
    case TOK_STRING: {
      Oop str = stNewString(c->vm, c->cur.text);
      uint8_t idx = addLiteral(c, str);
      emitBytes2(c, OP_PUSH_LITERAL, idx);
      nextToken(c);
      break;
    }
    case TOK_SYMBOL: {
      Oop sym = stInternSymbol(c->vm, c->cur.text);
      uint8_t idx = addLiteral(c, sym);
      emitBytes2(c, OP_PUSH_LITERAL, idx);
      nextToken(c);
      break;
    }
    case TOK_HASH_LPAREN: {
      nextToken(c);
      Oop arr = parseLiteralArrayBody(c);
      uint8_t idx = addLiteral(c, arr);
      emitBytes2(c, OP_PUSH_LITERAL, idx);
      break;
    }
    case TOK_HASH_LBRACK: {
      nextToken(c);
      Oop ba = parseLiteralByteArray(c);
      uint8_t idx = addLiteral(c, ba);
      emitBytes2(c, OP_PUSH_LITERAL, idx);
      break;
    }
    case TOK_IDENT: {
      if (strcmp(c->cur.text, "super") == 0) {
        *outIsSuper = true;
      }
      emitPushVariable(c, c->cur.text);
      nextToken(c);
      break;
    }
    case TOK_LPAREN: {
      nextToken(c);
      parseExpression(c);
      if (c->cur.type == TOK_RPAREN) {
        nextToken(c);
      } else {
        setError(c, "Expected ')'");
      }
      break;
    }
    case TOK_LBRACK:
      parseBlock(c);
      break;
    case TOK_LBRACE:
      parseDynamicArray(c);
      break;
    default:
      setError(c, "Expected primary expression");
      nextToken(c);
      break;
  }
}

static void parseUnaryExpr(Compiler* c, bool* ioIsSuper, uint16_t* outLastRcvrEndPos) {
  parsePrimary(c, ioIsSuper);
  while (c->cur.type == TOK_IDENT && !c->hasError) {
    if (outLastRcvrEndPos) *outLastRcvrEndPos = c->codeLen;
    Oop sel = stInternSymbol(c->vm, c->cur.text);
    uint8_t litIdx = addLiteral(c, sel);
    emitBytes3(c, *ioIsSuper ? OP_SUPER_SEND : OP_SEND, litIdx, 0);
    *ioIsSuper = false;
    nextToken(c);
  }
}

static void parseBinaryExpr(Compiler* c, bool* ioIsSuper, uint16_t* outLastRcvrEndPos) {
  parseUnaryExpr(c, ioIsSuper, outLastRcvrEndPos);
  while (c->cur.type == TOK_BINOP && !c->hasError) {
    if (outLastRcvrEndPos) *outLastRcvrEndPos = c->codeLen;
    Oop sel = stInternSymbol(c->vm, c->cur.text);
    bool sendSuper = *ioIsSuper;
    *ioIsSuper = false;
    nextToken(c);

    bool rhsSuper = false;
    parseUnaryExpr(c, &rhsSuper, nullptr);
    uint8_t litIdx = addLiteral(c, sel);
    emitBytes3(c, sendSuper ? OP_SUPER_SEND : OP_SEND, litIdx, 1);
  }
}

static void parseKeywordExpr(Compiler* c, bool* ioIsSuper, uint16_t* outLastRcvrEndPos) {
  parseBinaryExpr(c, ioIsSuper, outLastRcvrEndPos);
  if (c->cur.type == TOK_KEYWORD && !c->hasError) {
    if (outLastRcvrEndPos) *outLastRcvrEndPos = c->codeLen;
    bool sendSuper = *ioIsSuper;
    *ioIsSuper = false;

    char selectorBuf[128] = "";
    uint8_t numArgs = 0;
    while (c->cur.type == TOK_KEYWORD && !c->hasError) {
      if (strlen(selectorBuf) + strlen(c->cur.text) + 1 < sizeof(selectorBuf)) {
        strcat(selectorBuf, c->cur.text);
      }
      nextToken(c);
      bool argSuper = false;
      parseBinaryExpr(c, &argSuper, nullptr);
      numArgs++;
    }
    Oop sel = stInternSymbol(c->vm, selectorBuf);
    uint8_t litIdx = addLiteral(c, sel);
    emitBytes3(c, sendSuper ? OP_SUPER_SEND : OP_SEND, litIdx, numArgs);
  }
}

static void parseCascadeSideMessage(Compiler* c) {
  if (c->cur.type == TOK_IDENT) {
    Oop sel = stInternSymbol(c->vm, c->cur.text);
    uint8_t litIdx = addLiteral(c, sel);
    nextToken(c);
    emitBytes3(c, OP_SEND, litIdx, 0);
  } else if (c->cur.type == TOK_BINOP) {
    Oop sel = stInternSymbol(c->vm, c->cur.text);
    nextToken(c);
    bool rhsSuper = false;
    parseUnaryExpr(c, &rhsSuper, nullptr);
    uint8_t litIdx = addLiteral(c, sel);
    emitBytes3(c, OP_SEND, litIdx, 1);
  } else if (c->cur.type == TOK_KEYWORD) {
    char selectorBuf[128] = "";
    uint8_t numArgs = 0;
    while (c->cur.type == TOK_KEYWORD && !c->hasError) {
      if (strlen(selectorBuf) + strlen(c->cur.text) + 1 < sizeof(selectorBuf)) {
        strcat(selectorBuf, c->cur.text);
      }
      nextToken(c);
      bool argSuper = false;
      parseBinaryExpr(c, &argSuper, nullptr);
      numArgs++;
    }
    Oop sel = stInternSymbol(c->vm, selectorBuf);
    uint8_t litIdx = addLiteral(c, sel);
    emitBytes3(c, OP_SEND, litIdx, numArgs);
  } else {
    setError(c, "Expected message after ';'");
  }
}

static void parseExpression(Compiler* c) {
  // Handle chained assignments `a := b := expr`
  Oop assignTargets[8];
  int numAssigns = 0;

  while (c->cur.type == TOK_IDENT && numAssigns < 8) {
    // Peek ahead to see if `:=` or `_` follows
    size_t savedPos   = c->pos;
    uint16_t savedLn  = c->line;
    bool savedLastVal = c->lastWasValue;
    Token savedTok    = c->cur;

    nextToken(c);
    if (c->cur.type == TOK_ASSIGN) {
      assignTargets[numAssigns++] = stInternSymbol(c->vm, savedTok.text);
      nextToken(c);
    } else {
      // Rewind
      c->pos          = savedPos;
      c->line         = savedLn;
      c->lastWasValue = savedLastVal;
      c->cur          = savedTok;
      break;
    }
  }

  bool isSuper = false;
  uint16_t lastRcvrEndPos = 0xFFFFu;
  parseKeywordExpr(c, &isSuper, &lastRcvrEndPos);

  // Handle message cascades `;`
  if (c->cur.type == TOK_CASCADE && !c->hasError) {
    if (lastRcvrEndPos == 0xFFFFu) {
      setError(c, "Cascade ';' requires a preceding message send");
    } else {
      insertByteAt(c, lastRcvrEndPos, OP_DUP);
      while (c->cur.type == TOK_CASCADE && !c->hasError) {
        nextToken(c);
        emitByte(c, OP_POP);  // discard previous message result
        // Check if another `;` will follow this cascaded message:
        // We can record the receiver top position right here (`c->codeLen`)
        // and if another `;` follows, we insert `OP_DUP` at `cascadeRcvrPos`!
        uint16_t cascadeRcvrPos = c->codeLen;
        parseCascadeSideMessage(c);
        if (c->cur.type == TOK_CASCADE) {
          insertByteAt(c, cascadeRcvrPos, OP_DUP);
        }
      }
    }
  }

  for (int i = numAssigns - 1; i >= 0; i--) {
    emitStoreVariable(c, assignTargets[i]);
  }
}

static void parseOptionalTemporaries(Compiler* c);

static void parseStatementList(Compiler* c, bool isBlock, bool isDoIt) {
  bool lastWasReturn = false;
  int stmtCount = 0;

  while (c->cur.type != TOK_EOF &&
         c->cur.type != TOK_RBRACK &&
         !c->hasError) {
    parseOptionalTemporaries(c);
    if (c->cur.type == TOK_EOF || c->cur.type == TOK_RBRACK || c->hasError) {
      break;
    }
    if (stmtCount > 0) {
      // Pop result of previous non-final statement
      emitByte(c, OP_POP);
    }
    if (c->cur.type == TOK_RETURN) {
      nextToken(c);
      parseExpression(c);
      emitByte(c, OP_RETURN_TOP);
      lastWasReturn = true;
      stmtCount++;
      if (c->cur.type == TOK_PERIOD) {
        nextToken(c);
      }
      break;
    }

    parseExpression(c);
    stmtCount++;
    lastWasReturn = false;

    if (c->cur.type == TOK_PERIOD) {
      nextToken(c);
      // Allow trailing period before `]` or EOF
      if (c->cur.type == TOK_EOF || c->cur.type == TOK_RBRACK) {
        break;
      }
    } else {
      break;
    }
  }

  if (stmtCount == 0) {
    if (isBlock || isDoIt) {
      emitByte(c, OP_PUSH_NIL);
      emitByte(c, isBlock ? OP_BLOCK_RETURN : OP_RETURN_TOP);
    } else {
      emitByte(c, OP_RETURN_SELF);
    }
    return;
  }

  if (!lastWasReturn) {
    if (isBlock) {
      emitByte(c, OP_BLOCK_RETURN);
    } else if (isDoIt) {
      emitByte(c, OP_RETURN_TOP);
    } else {
      emitByte(c, OP_POP);
      emitByte(c, OP_RETURN_SELF);
    }
  }
}

static void parseOptionalTemporaries(Compiler* c) {
  if (c->cur.type == TOK_BINOP && strcmp(c->cur.text, "|") == 0) {
    c->lastWasValue = false;
    nextToken(c);
    while (c->cur.type == TOK_IDENT && !c->hasError) {
      addLocalVar(c, c->cur.text);
      nextToken(c);
    }
    if (c->cur.type == TOK_BINOP && strcmp(c->cur.text, "|") == 0) {
      c->lastWasValue = false;
      nextToken(c);
    } else {
      setError(c, "Expected closing '|' after temporaries");
    }
  } else if (c->cur.type == TOK_BINOP && strcmp(c->cur.text, "||") == 0) {
    c->lastWasValue = false;
    nextToken(c);
  }
}

static void parseOptionalPrimitivePragma(Compiler* c) {
  if (c->cur.type == TOK_BINOP && strcmp(c->cur.text, "<") == 0) {
    nextToken(c);
    if (c->cur.type == TOK_KEYWORD && strcmp(c->cur.text, "primitive:") == 0) {
      nextToken(c);
      if (c->cur.type == TOK_INT && c->cur.intVal > 0 && c->cur.intVal < 1024) {
        c->primId = (uint16_t)c->cur.intVal;
        nextToken(c);
        if (c->cur.type == TOK_BINOP && strcmp(c->cur.text, ">") == 0) {
          c->lastWasValue = false;
          nextToken(c);
          return;
        }
      }
    }
    setError(c, "Malformed <primitive: N> pragma");
  }
}

static Oop buildCompiledMethod(Compiler* c, Oop selectorSym, const char* src, int bootstrapSrcIdx) {
  if (c->hasError) return kOopNil;

  uint16_t saved = c->vm->tempRootCount;
  stPushRoot(c->vm, selectorSym);

  Oop litsArr = stNewArray(c->vm, c->literalCount);
  stPushRoot(c->vm, litsArr);
  for (uint8_t i = 0; i < c->literalCount; i++) {
    stSlotAtPut(c->vm, litsArr, i, c->literals[i]);
  }

  Oop bcArr = stNewBytes(c->vm, CLS_BYTE_ARRAY, FMT_BYTES, c->code, c->codeLen);
  stPushRoot(c->vm, bcArr);

  Oop srcOop = (bootstrapSrcIdx >= 0)
                   ? makeSmallInt(bootstrapSrcIdx)
                   : stNewString(c->vm, src ? src : "");
  stPushRoot(c->vm, srcOop);

  Oop meth = stNewOopSlots(c->vm, CLS_COMPILED_METHOD, SLOT_METH_COUNT, 0);
  if (meth != kOopNil) {
    int32_t hdr = packMethodHeader(c->numMethodArgs, c->maxTempSlots, c->primId, c->isClassMethod);
    stSlotAtPut(c->vm, meth, SLOT_METH_SELECTOR,    selectorSym);
    stSlotAtPut(c->vm, meth, SLOT_METH_OWNER_CLASS, c->clsOop);
    stSlotAtPut(c->vm, meth, SLOT_METH_LITERALS,    litsArr);
    stSlotAtPut(c->vm, meth, SLOT_METH_BYTECODES,   bcArr);
    stSlotAtPut(c->vm, meth, SLOT_METH_HEADER,      makeSmallInt(hdr));
    stSlotAtPut(c->vm, meth, SLOT_METH_SOURCE,      srcOop);
  }

  stPopRoots(c->vm, saved);
  return meth;
}

static Compiler gCompilerScratch;

Oop stCompileMethod(VMState* vm, Oop clsOop, bool isClassMethod,
                    const char* src, int bootstrapSrcIdx) {
  if (!vm || !src) return kOopNil;

  bool prevPin = vm->pinNewObjects;
  vm->pinNewObjects = true;
  vm->lastError[0] = '\0';

  Compiler& comp = gCompilerScratch;
  memset(&comp, 0, sizeof(comp));
  comp.vm            = vm;
  comp.src           = src;
  comp.line          = 1;
  comp.clsOop        = clsOop;
  comp.isClassMethod = isClassMethod;

  nextToken(&comp);

  // Parse method header (unary, binary, or keyword)
  char selectorBuf[128] = "";
  if (comp.cur.type == TOK_IDENT) {
    strncpy(selectorBuf, comp.cur.text, sizeof(selectorBuf) - 1);
    comp.numMethodArgs = 0;
    nextToken(&comp);
  } else if (comp.cur.type == TOK_BINOP) {
    strncpy(selectorBuf, comp.cur.text, sizeof(selectorBuf) - 1);
    comp.numMethodArgs = 1;
    nextToken(&comp);
    if (comp.cur.type == TOK_IDENT) {
      addLocalVar(&comp, comp.cur.text);
      nextToken(&comp);
    } else {
      setError(&comp, "Expected parameter name after binary selector");
    }
  } else if (comp.cur.type == TOK_KEYWORD) {
    while (comp.cur.type == TOK_KEYWORD && !comp.hasError) {
      if (strlen(selectorBuf) + strlen(comp.cur.text) + 1 < sizeof(selectorBuf)) {
        strcat(selectorBuf, comp.cur.text);
      }
      nextToken(&comp);
      if (comp.cur.type == TOK_IDENT) {
        addLocalVar(&comp, comp.cur.text);
        comp.numMethodArgs++;
        nextToken(&comp);
      } else {
        setError(&comp, "Expected parameter name after keyword");
        break;
      }
    }
  } else {
    setError(&comp, "Expected method selector header");
  }

  parseOptionalPrimitivePragma(&comp);
  parseOptionalTemporaries(&comp);
  parseOptionalsAgain:
  if (comp.cur.type == TOK_BINOP && strcmp(comp.cur.text, "<") == 0 && comp.primId == 0) {
    parseOptionalPrimitivePragma(&comp);
    goto parseOptionalsAgain;
  }

  parseStatementList(&comp, /*isBlock=*/false, /*isDoIt=*/false);

  Oop meth = kOopNil;
  if (!comp.hasError) {
    Oop selSym = stInternSymbol(vm, selectorBuf);
    meth = buildCompiledMethod(&comp, selSym, src, bootstrapSrcIdx);
    if (meth != kOopNil && isHeapOop(clsOop) && clsOop != kOopNil) {
      Oop mdict = stSlotAt(vm, clsOop, isClassMethod ? SLOT_CLS_CLASS_METHODS : SLOT_CLS_INST_METHODS);
      stDictAtPut(vm, mdict, selSym, meth);
    }
  }

  vm->pinNewObjects = prevPin;
  return meth;
}

Oop stCompileExpression(VMState* vm, const char* src, Oop receiverClsOop) {
  if (!vm || !src) return kOopNil;

  bool prevPin = vm->pinNewObjects;
  vm->pinNewObjects = true;
  vm->lastError[0] = '\0';

  Compiler& comp = gCompilerScratch;
  memset(&comp, 0, sizeof(comp));
  comp.vm            = vm;
  comp.src           = src;
  comp.line          = 1;
  comp.clsOop        = (receiverClsOop != kOopNull) ? receiverClsOop : vm->classes[CLS_UNDEFINED_OBJECT];
  comp.isClassMethod = false;

  nextToken(&comp);
  parseOptionalTemporaries(&comp);
  parseStatementList(&comp, /*isBlock=*/false, /*isDoIt=*/true);

  Oop meth = kOopNil;
  if (!comp.hasError) {
    Oop doItSym = stInternSymbol(vm, "doIt");
    meth = buildCompiledMethod(&comp, doItSym, src, -1);
  }

  vm->pinNewObjects = prevPin;
  return meth;
}
