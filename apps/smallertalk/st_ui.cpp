#include "st_ui.h"
#include "st_compiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Built-In Workspace Demo Scripts (stored in DROM .rodata)
// ---------------------------------------------------------------------------
struct DemoScript {
  const char* title;
  const char* code;
};

static const DemoScript kDemoScripts[] = {
  {
    "1: Primes & Factorial",
    "| primes |\n"
    "primes := #(2 3 4 5 6 7 8 9 10 11 12 13) select: [ :n | n isPrime ].\n"
    "Transcript show: 'Primes: '; print: primes; cr.\n"
    "Transcript show: '7! = '; print: 7 factorial; cr.\n"
    "7 factorial"
  },
  {
    "2: Closures & Non-Local ^",
    "| makeAdder findGt10 |\n"
    "makeAdder := [ :k | [ :x | x + k ] ].\n"
    "findGt10 := [ :arr |\n"
    "  arr do: [ :n | n > 10 ifTrue: [ ^ (makeAdder value: 100) value: n ] ].\n"
    "  -1\n"
    "].\n"
    "findGt10 value: #(3 7 15 22)"
  },
  {
    "3: Subclass & Compile",
    "Object subclass: #Counter instanceVariableNames: 'val'.\n"
    "Counter compile: 'init val := 0'.\n"
    "Counter compile: 'add: d val := val + d. ^ val'.\n"
    "| c |\n"
    "c := Counter new init.\n"
    "c add: 10; add: 32"
  },
  {
    "4: Turtle Pen Star",
    "| p |\n"
    "p := Pen new.\n"
    "p place: 210 y: 125; color: 65504.\n"
    "1 to: 15 do: [ :i |\n"
    "  p go: (i * 3); turn: 144\n"
    "].\n"
    "'Turtle Star Drawn!'"
  },
  {
    "5: VFS /flash File I/O",
    "File write: '/flash/demo.st' contents: '10 factorial'.\n"
    "Transcript show: 'Wrote /flash/demo.st: ';\n"
    "  show: (File read: '/flash/demo.st'); cr.\n"
    "File read: '/flash/demo.st'"
  },
  {
    "6: Open ClassBrowser",
    "ClassBrowser openOn: Array"
  }
};
static constexpr int kNumDemos = (int)(sizeof(kDemoScripts) / sizeof(kDemoScripts[0]));

// ---------------------------------------------------------------------------
// TextBuffer Helpers
// ---------------------------------------------------------------------------
void stTextBufferSet(TextBuffer* tb, const char* text) {
  memset(tb, 0, sizeof(*tb));
  if (!text || !text[0]) {
    tb->lineCount = 1;
    tb->lines[0][0] = '\0';
    return;
  }
  const char* p = text;
  while (*p && tb->lineCount < kMaxEditorLines) {
    const char* nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len > 0 && p[len - 1] == '\r') len--;
    if (len >= kMaxLineChars) len = kMaxLineChars - 1;
    memcpy(tb->lines[tb->lineCount], p, len);
    tb->lines[tb->lineCount][len] = '\0';
    tb->lineCount++;
    if (!nl) break;
    p = nl + 1;
  }
  if (tb->lineCount == 0) {
    tb->lineCount = 1;
  }
}

void stTextBufferJoin(const TextBuffer* tb, char* outBuf, size_t maxLen) {
  if (!outBuf || maxLen == 0) return;
  outBuf[0] = '\0';
  size_t pos = 0;
  for (int i = 0; i < tb->lineCount; i++) {
    size_t l = strlen(tb->lines[i]);
    if (pos + l + 2 >= maxLen) break;
    if (i > 0) outBuf[pos++] = '\n';
    memcpy(outBuf + pos, tb->lines[i], l);
    pos += l;
  }
  outBuf[pos] = '\0';
}

// ---------------------------------------------------------------------------
// Low-Level UI Drawing Primitives
// ---------------------------------------------------------------------------
static void drawButton(const LakatosApi* os, int x, int y, int w, int h,
                       uint16_t bg, uint16_t border, uint16_t fg,
                       const char* label, uint8_t font = 1) {
  os->tft_fill_round_rect(x, y, w, h, 3, bg);
  os->tft_draw_round_rect(x, y, w, h, 3, border);
  os->tft_set_text_datum(LAKATOS_MC_DATUM);
  os->tft_set_text_color(fg, bg);
  os->tft_draw_string(label, x + w / 2, y + h / 2, font);
  os->tft_set_text_datum(LAKATOS_TL_DATUM);
}

static void drawTruncatedText(const LakatosApi* os, const char* str, int x, int y,
                              int maxChars, uint16_t fg, uint16_t bg, uint8_t font = 1) {
  char buf[96];
  if (!str) str = "";
  if (maxChars <= 0) return;
  if (maxChars >= (int)sizeof(buf)) maxChars = (int)sizeof(buf) - 1;
  strncpy(buf, str, (size_t)maxChars);
  buf[maxChars] = '\0';
  os->tft_set_text_datum(LAKATOS_TL_DATUM);
  os->tft_set_text_color(fg, bg);
  os->tft_draw_string(buf, x, y, font);
}

// ---------------------------------------------------------------------------
// Quick-Token + Stylus Keyboard Line Editor Modal
// ---------------------------------------------------------------------------
static const char* const kQuickTokens[16] = {
  ":= ", "^ ", "[:x | ", "] ",
  "self ", "super ", "true", "false",
  "ifTrue: [", "whileTrue: [", "to: 10 do: [", "; ",
  "Transcript show: ", "'hello'", "#sym", "."
};

static void drawLineEditorModal(const LakatosApi* os, const char* title, const char* lineBuf) {
  os->tft_fill_screen(0x0841);
  // Top bar
  os->tft_fill_rect(0, 0, 320, 24, LAKATOS_NAVY);
  drawButton(os, 2, 2, 56, 20, 0x4800, LAKATOS_RED, LAKATOS_WHITE, "CANCEL", 1);
  os->tft_set_text_datum(LAKATOS_MC_DATUM);
  os->tft_set_text_color(LAKATOS_YELLOW, LAKATOS_NAVY);
  os->tft_draw_string(title ? title : "EDIT SMALLTALK LINE", 150, 12, 2);
  os->tft_set_text_datum(LAKATOS_TL_DATUM);
  drawButton(os, 250, 2, 68, 20, LAKATOS_DARKGREEN, LAKATOS_GREEN, LAKATOS_WHITE, "SAVE/OK", 1);

  // Current line box (y = 27..59)
  os->tft_fill_round_rect(4, 27, 232, 32, 4, LAKATOS_BLACK);
  os->tft_draw_round_rect(4, 27, 232, 32, 4, LAKATOS_CYAN);
  size_t len = strlen(lineBuf);
  const char* disp = (len > 36) ? (lineBuf + len - 36) : lineBuf;
  char shown[48];
  snprintf(shown, sizeof(shown), "%s_", disp);
  drawTruncatedText(os, shown, 10, 39, 38, LAKATOS_WHITE, LAKATOS_BLACK, 1);

  drawButton(os, 240, 27, 38, 32, 0x4800, LAKATOS_RED, LAKATOS_WHITE, "DEL", 1);
  drawButton(os, 280, 27, 36, 32, 0x3186, LAKATOS_LIGHTGREY, LAKATOS_YELLOW, "CLR", 1);

  // Quick Smalltalk Token Grid (4 rows x 4 cols, y = 64..192)
  for (int r = 0; r < 4; r++) {
    int by = 64 + r * 32;
    for (int c = 0; c < 4; c++) {
      int bx = 4 + c * 78;
      drawButton(os, bx, by, 75, 29, 0x18E3, LAKATOS_CYAN, LAKATOS_WHITE,
                 kQuickTokens[r * 4 + c], 1);
    }
  }

  // Bottom bar (y = 198..236)
  drawButton(os, 4, 198, 210, 36, 0x2945, LAKATOS_YELLOW, LAKATOS_YELLOW,
             "OPEN FULL KEYBOARD (ABC/123)", 1);
  drawButton(os, 218, 198, 98, 36, LAKATOS_DARKGREEN, LAKATOS_GREEN, LAKATOS_WHITE,
             "SAVE LINE", 2);
}

static bool runLineEditorModal(StUiManager* ui, const char* title, char* ioLine, size_t maxLen) {
  const LakatosApi* os = ui->os;
  if (!os) return false;
  char work[kMaxLineChars];
  strncpy(work, ioLine, sizeof(work) - 1);
  work[sizeof(work) - 1] = '\0';

  drawLineEditorModal(os, title, work);
  touchFilterResetAfterModal(&ui->touch, os);

  for (;;) {
    FilteredTouchEvent ev = touchFilterPoll(&ui->touch, os);
    if (ev.justClicked) {
      int tx = ev.x;
      int ty = ev.y;
      if (ty < 25) {
        if (tx <= 60) {
          touchFilterResetAfterModal(&ui->touch, os);
          return false;  // Cancelled
        }
        if (tx >= 248) {
          strncpy(ioLine, work, maxLen - 1);
          ioLine[maxLen - 1] = '\0';
          touchFilterResetAfterModal(&ui->touch, os);
          return true;
        }
      } else if (ty >= 27 && ty <= 60) {
        if (tx >= 238 && tx < 279) {
          size_t l = strlen(work);
          if (l > 0) work[l - 1] = '\0';
          drawLineEditorModal(os, title, work);
        } else if (tx >= 279) {
          work[0] = '\0';
          drawLineEditorModal(os, title, work);
        } else if (tx < 236 && os->prompt_keyboard) {
          if (os->prompt_keyboard(title, work, sizeof(work), false) == 1) {
            strncpy(ioLine, work, maxLen - 1);
            ioLine[maxLen - 1] = '\0';
            touchFilterResetAfterModal(&ui->touch, os);
            return true;
          }
          drawLineEditorModal(os, title, work);
          touchFilterResetAfterModal(&ui->touch, os);
        }
      } else if (ty >= 64 && ty < 192) {
        int r = (ty - 64) / 32;
        int c = (tx - 4) / 78;
        if (r >= 0 && r < 4 && c >= 0 && c < 4) {
          const char* tok = kQuickTokens[r * 4 + c];
          size_t l = strlen(work);
          size_t tl = strlen(tok);
          if (l + tl + 1 < sizeof(work)) {
            strcat(work, tok);
            drawLineEditorModal(os, title, work);
          }
        }
      } else if (ty >= 196) {
        if (tx < 216) {
          if (os->prompt_keyboard) {
            int kr = os->prompt_keyboard(title, work, sizeof(work), false);
            touchFilterResetAfterModal(&ui->touch, os);
            if (kr == 1) {
              strncpy(ioLine, work, maxLen - 1);
              ioLine[maxLen - 1] = '\0';
              return true;
            }
            drawLineEditorModal(os, title, work);
          }
        } else {
          strncpy(ioLine, work, maxLen - 1);
          ioLine[maxLen - 1] = '\0';
          touchFilterResetAfterModal(&ui->touch, os);
          return true;
        }
      }
    }
    if (os->delay_ms) os->delay_ms(10);
  }
}

// ---------------------------------------------------------------------------
// ClassBrowser State Helpers
// ---------------------------------------------------------------------------
static int collectClassList(VMState* vm, Oop* outClasses, int maxCount) {
  int n = 0;
  for (uint16_t i = 1; i < vm->classCount && i < kMaxClasses; i++) {
    Oop c = vm->classes[i];
    if (isHeapOop(c) && c != kOopNil && n < maxCount) {
      outClasses[n++] = c;
    }
  }
  return n;
}

static int collectMethodList(VMState* vm, Oop clsOop, bool isClassSide, Oop* outSels, int maxCount) {
  if (!isHeapOop(clsOop) || clsOop == kOopNil) return 0;
  Oop dict = stSlotAt(vm, clsOop, isClassSide ? SLOT_CLS_CLASS_METHODS : SLOT_CLS_INST_METHODS);
  Oop keys = stSlotAt(vm, dict, SLOT_DICT_KEYS);
  Oop tallyOop = stSlotAt(vm, dict, SLOT_DICT_TALLY);
  int tally = isSmallInt(tallyOop) ? (int)asSmallInt(tallyOop) : 0;
  if (tally > maxCount) tally = maxCount;
  for (int i = 0; i < tally; i++) {
    outSels[i] = stSlotAt(vm, keys, (uint16_t)i);
  }
  return tally;
}

static void loadSelectedMethodIntoEditor(VMState* vm, ClassBrowserWinState* cb) {
  if (!isHeapOop(cb->selectedClass) || cb->selectedSelector == kOopNil) {
    stTextBufferSet(&cb->methodEditor, "newMethod\n  ^ self");
    return;
  }
  Oop dict = stSlotAt(vm, cb->selectedClass,
                      cb->isClassSide ? SLOT_CLS_CLASS_METHODS : SLOT_CLS_INST_METHODS);
  Oop meth = stDictAt(vm, dict, cb->selectedSelector, kOopNull);
  if (meth == kOopNull || meth == kOopNil) {
    stTextBufferSet(&cb->methodEditor, "newMethod\n  ^ self");
    return;
  }
  char scratch[512];
  const char* src = stGetMethodSourceCStr(vm, meth, scratch, sizeof(scratch));
  stTextBufferSet(&cb->methodEditor, src);
}

// ---------------------------------------------------------------------------
// FileBrowser State Helpers
// ---------------------------------------------------------------------------
static void refreshFileBrowserDir(const LakatosApi* os, FileBrowserWinState* fb) {
  fb->inFileView = false;
  fb->confirmDeleteArmed = false;
  fb->pageIdx = 0;
  if (!os || !os->fs_ls) {
    fb->entryCount = 0;
    snprintf(fb->statusMsg, sizeof(fb->statusMsg), "VFS unavailable");
    return;
  }
  int n = os->fs_ls(fb->currentDir, fb->entries, kMaxFileEntries);
  if (n < 0) {
    fb->entryCount = 0;
    snprintf(fb->statusMsg, sizeof(fb->statusMsg), "Cannot list %s", fb->currentDir);
  } else {
    fb->entryCount = n;
    snprintf(fb->statusMsg, sizeof(fb->statusMsg), "%s: %d item(s)", fb->currentDir, n);
  }
}

static void openFileInBrowser(const LakatosApi* os, FileBrowserWinState* fb, int idx) {
  if (idx < 0 || idx >= fb->entryCount) return;
  const LakatosFsEntry& e = fb->entries[idx];
  char fullPath[160];
  size_t dlen = strlen(fb->currentDir);
  if (dlen > 0 && fb->currentDir[dlen - 1] == '/') {
    snprintf(fullPath, sizeof(fullPath), "%s%s", fb->currentDir, e.name);
  } else {
    snprintf(fullPath, sizeof(fullPath), "%s/%s", fb->currentDir, e.name);
  }

  if (e.isDir) {
    strncpy(fb->currentDir, fullPath, sizeof(fb->currentDir) - 1);
    fb->currentDir[sizeof(fb->currentDir) - 1] = '\0';
    refreshFileBrowserDir(os, fb);
    return;
  }

  strncpy(fb->selectedFile, e.name, sizeof(fb->selectedFile) - 1);
  fb->selectedFile[sizeof(fb->selectedFile) - 1] = '\0';
  fb->selectedFileSize = e.size;
  fb->previewScrollLine = 0;
  fb->confirmDeleteArmed = false;
  memset(fb->previewBuf, 0, sizeof(fb->previewBuf));
  if (os && os->fs_read) {
    int32_t got = os->fs_read(fullPath, 0, fb->previewBuf, sizeof(fb->previewBuf) - 1);
    if (got > 0) {
      fb->previewBuf[got] = '\0';
    } else {
      strcpy(fb->previewBuf, "(Empty or binary file)");
    }
  }
  fb->inFileView = true;
  snprintf(fb->statusMsg, sizeof(fb->statusMsg), "%s (%lld B)", e.name, (long long)e.size);
}

// ---------------------------------------------------------------------------
// Window Creation & Z-Order Management
// ---------------------------------------------------------------------------
static void bringWindowToFront(StUiManager* ui, int winIdx) {
  int pos = -1;
  for (int i = 0; i < ui->winCount; i++) {
    if (ui->zOrder[i] == (uint8_t)winIdx) {
      pos = i;
      break;
    }
  }
  if (pos < 0) return;
  for (int i = pos; i + 1 < ui->winCount; i++) {
    ui->zOrder[i] = ui->zOrder[i + 1];
  }
  ui->zOrder[ui->winCount - 1] = (uint8_t)winIdx;
  ui->needsRedraw = true;
}

static void closeWindowAt(StUiManager* ui, int winIdx) {
  if (ui->winCount <= 1) {
    // Keep at least one window open
    ui->windows[winIdx].closeConfirmArmed = false;
    ui->needsRedraw = true;
    return;
  }
  ui->windows[winIdx].active = false;
  int pos = -1;
  for (int i = 0; i < ui->winCount; i++) {
    if (ui->zOrder[i] == (uint8_t)winIdx) {
      pos = i;
      break;
    }
  }
  if (pos >= 0) {
    for (int i = pos; i + 1 < ui->winCount; i++) {
      ui->zOrder[i] = ui->zOrder[i + 1];
    }
    ui->winCount--;
  }
  ui->needsRedraw = true;
}

static void recomputeOverlappingBounds(StUiManager* ui) {
  // Assign staggered cascade coordinates so up to 3 windows show distinct
  // clickable title bars in Overlapping Mode (`maximizedMode == false`).
  for (int z = 0; z < ui->winCount; z++) {
    StWindow& w = ui->windows[ui->zOrder[z]];
    if (ui->maximizedMode) {
      w.x = 0;
      w.y = 22;
      w.w = 320;
      w.h = 218;
    } else {
      int depthFromFront = (ui->winCount - 1) - z;
      if (depthFromFront >= 2) {
        w.x = 2;
        w.y = 23;
        w.w = 304;
        w.h = 180;
      } else if (depthFromFront == 1) {
        w.x = 6;
        w.y = 39;
        w.w = 308;
        w.h = 182;
      } else {
        // Frontmost focused window
        int topY = (ui->winCount >= 3) ? 55 : ((ui->winCount == 2) ? 39 : 23);
        w.x = 2;
        w.y = (int16_t)topY;
        w.w = 316;
        w.h = (int16_t)(239 - topY);
      }
    }
  }
}

int stUiSpawnWindow(StUiManager* ui, int winType, Oop argOop) {
  int slot = -1;
  for (int i = 0; i < kMaxWindows; i++) {
    if (!ui->windows[i].active) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    // Reuse oldest backmost window slot if all 8 slots are full
    slot = ui->zOrder[0];
    closeWindowAt(ui, slot);
  }

  StWindow& w = ui->windows[slot];
  memset(&w, 0, sizeof(w));
  w.active     = true;
  w.type       = (uint8_t)winType;
  w.instanceId = ++ui->nextInstanceSeq;

  if (winType == ST_WIN_WORKSPACE) {
    snprintf(w.title, sizeof(w.title), "Workspace & Transcript #%u", (unsigned)w.instanceId);
    snprintf(w.tabLabel, sizeof(w.tabLabel), "Work#%u", (unsigned)w.instanceId);
    w.ws.demoIdx = 0;
    if (isHeapOop(argOop) && argOop != kOopNil) {
      char customCode[384];
      stCopyCString(ui->vm, argOop, customCode, sizeof(customCode));
      stTextBufferSet(&w.ws.editor, customCode);
    } else {
      stTextBufferSet(&w.ws.editor, kDemoScripts[0].code);
    }
    strcpy(w.ws.lastEvalResult, "Ready -- tap [Do It] or [Print It]");
  } else if (winType == ST_WIN_CLASS_BROWSER) {
    snprintf(w.title, sizeof(w.title), "Class Browser #%u", (unsigned)w.instanceId);
    snprintf(w.tabLabel, sizeof(w.tabLabel), "Brow#%u", (unsigned)w.instanceId);
    Oop targetCls = ui->vm->classes[CLS_SMALL_INTEGER];
    if (isHeapOop(argOop) && argOop != kOopNil) {
      ObjHeader* ah = stGetHeader(ui->vm, argOop);
      if (ah && (ah->flags & OBJ_FLAG_CLASS_OBJ)) {
        targetCls = argOop;
      }
    }
    w.cb.selectedClass = targetCls;
    // Find page of selectedClass
    Oop clsList[kMaxClasses];
    int nCls = collectClassList(ui->vm, clsList, kMaxClasses);
    for (int i = 0; i < nCls; i++) {
      if (clsList[i] == targetCls) {
        w.cb.classPage = i / 6;
        break;
      }
    }
    Oop mList[64];
    int nMeth = collectMethodList(ui->vm, targetCls, false, mList, 64);
    w.cb.selectedSelector = (nMeth > 0) ? mList[0] : kOopNil;
    loadSelectedMethodIntoEditor(ui->vm, &w.cb);
    strcpy(w.cb.statusMsg, "Tap class/method, or [Src>] to edit & compile");
  } else if (winType == ST_WIN_FILE_BROWSER) {
    snprintf(w.title, sizeof(w.title), "File Browser #%u", (unsigned)w.instanceId);
    snprintf(w.tabLabel, sizeof(w.tabLabel), "File#%u", (unsigned)w.instanceId);
    strcpy(w.fb.currentDir, "/flash");
    if (isHeapOop(argOop) && argOop != kOopNil) {
      stCopyCString(ui->vm, argOop, w.fb.currentDir, sizeof(w.fb.currentDir));
    }
    refreshFileBrowserDir(ui->os, &w.fb);
  }

  ui->zOrder[ui->winCount++] = (uint8_t)slot;
  recomputeOverlappingBounds(ui);
  ui->needsRedraw = true;
  return slot;
}

static void vmOpenWindowHook(int winType, Oop argOop, void* user) {
  StUiManager* ui = (StUiManager*)user;
  if (ui) {
    stUiSpawnWindow(ui, winType, argOop);
  }
}

void stUiInit(StUiManager* ui, VMState* vm, const LakatosApi* os) {
  memset(ui, 0, sizeof(*ui));
  ui->vm            = vm;
  ui->os            = os;
  ui->maximizedMode = false;  // Start in Overlapping Hybrid mode showing all 3 tools!
  ui->needsRedraw   = true;
  touchFilterInit(&ui->touch);

  vm->onOpenWindow = vmOpenWindowHook;
  vm->uiUser       = ui;

  stTranscriptAppendCStr(vm, "SmallerTalk-80 on Lakatos OS\n");
  stTranscriptAppendCStr(vm, "Tap [Do It] or [Print It] above!\n");

  // Instantiate the 3 core tools in initial desktop state:
  // 1. File Browser (back)
  // 2. Class Browser (middle)
  // 3. Workspace / Transcript (frontmost focused)
  stUiSpawnWindow(ui, ST_WIN_FILE_BROWSER,  kOopNil);
  stUiSpawnWindow(ui, ST_WIN_CLASS_BROWSER, vm->classes[CLS_SMALL_INTEGER]);
  stUiSpawnWindow(ui, ST_WIN_WORKSPACE,     kOopNil);
}

// ---------------------------------------------------------------------------
// Rendering Individual Tool Windows
// ---------------------------------------------------------------------------
static void renderWorkspaceClient(StUiManager* ui, StWindow& w, int cx, int cy, int cw, int ch) {
  const LakatosApi* os = ui->os;
  WorkspaceWinState& ws = w.ws;

  // Layout inside client rect `(cx, cy, cw, ch)`:
  // 1. Code Editor Box (`editorH` pixels)
  // 2. Action Toolbar (`20 px` tall)
  // 3. Transcript Output Box + Status strip (remaining height)
  int toolbarH = 20;
  int statusH  = 14;
  int availH   = ch - toolbarH - statusH - 4;
  int editorH  = (availH * 52) / 100;
  if (editorH < 44) editorH = 44;
  int transH   = availH - editorH;

  int edY      = cy + 1;
  int tbY      = edY + editorH + 1;
  int trY      = tbY + toolbarH + 1;
  int stY      = trY + transH;

  // 1. Code Editor Pane
  os->tft_fill_rect(cx + 2, edY, cw - 4, editorH, 0x0841);
  os->tft_draw_rect(cx + 2, edY, cw - 4, editorH, 0x3186);

  int lineH = 12;
  int visLines = (editorH - 4) / lineH;
  if (visLines < 1) visLines = 1;
  if (ws.editor.selectedLine < ws.editor.scrollTop) {
    ws.editor.scrollTop = ws.editor.selectedLine;
  } else if (ws.editor.selectedLine >= ws.editor.scrollTop + visLines) {
    ws.editor.scrollTop = ws.editor.selectedLine - visLines + 1;
  }

  int maxChars = (cw - 30) / 6;
  for (int row = 0; row < visLines; row++) {
    int lIdx = ws.editor.scrollTop + row;
    if (lIdx >= ws.editor.lineCount) break;
    int ly = edY + 2 + row * lineH;
    bool sel = (lIdx == ws.editor.selectedLine);
    uint16_t rowBg = sel ? 0x194A : 0x0841;
    if (sel) {
      os->tft_fill_rect(cx + 3, ly, cw - 6, lineH, rowBg);
    }
    char numBuf[8];
    snprintf(numBuf, sizeof(numBuf), "%d:", lIdx + 1);
    drawTruncatedText(os, numBuf, cx + 5, ly + 2, 3, LAKATOS_YELLOW, rowBg, 1);
    drawTruncatedText(os, ws.editor.lines[lIdx], cx + 24, ly + 2, maxChars,
                      sel ? LAKATOS_WHITE : 0xDEFB, rowBg, 1);
  }

  // 2. Action Toolbar (`tbY`, height 20)
  int bw = (cw - 8) / 6;
  drawButton(os, cx + 2 + 0 * bw, tbY, bw - 2, 19, LAKATOS_DARKGREEN, LAKATOS_GREEN,     LAKATOS_WHITE,  "Do It",    1);
  drawButton(os, cx + 2 + 1 * bw, tbY, bw - 2, 19, 0x0290,            LAKATOS_CYAN,      LAKATOS_CYAN,   "Print It", 1);
  drawButton(os, cx + 2 + 2 * bw, tbY, bw - 2, 19, 0x2945,            LAKATOS_YELLOW,    LAKATOS_YELLOW, "Edit Ln",  1);
  drawButton(os, cx + 2 + 3 * bw, tbY, bw - 2, 19, 0x18E3,            LAKATOS_LIGHTGREY, LAKATOS_WHITE,  "+Line",    1);
  drawButton(os, cx + 2 + 4 * bw, tbY, bw - 2, 19, 0x3800,            LAKATOS_RED,       LAKATOS_WHITE,  "-Line",    1);
  drawButton(os, cx + 2 + 5 * bw, tbY, bw - 2, 19, 0x3010,            LAKATOS_MAGENTA,   LAKATOS_WHITE,  "Demos",    1);

  // 3. Transcript Pane (`trY`, height `transH`)
  os->tft_fill_rect(cx + 2, trY, cw - 4, transH, LAKATOS_BLACK);
  os->tft_draw_rect(cx + 2, trY, cw - 4, transH, 0x2945);

  // Split tail of `vm->transcript` into visible lines
  int trVisLines = (transH - 4) / 10;
  if (trVisLines < 1) trVisLines = 1;
  const char* trText = ui->vm->transcript;
  const char* lineStarts[64];
  int lineLens[64];
  int nTrLines = 0;
  const char* p = trText;
  while (*p && nTrLines < 64) {
    const char* nl = strchr(p, '\n');
    int len = nl ? (int)(nl - p) : (int)strlen(p);
    lineStarts[nTrLines] = p;
    lineLens[nTrLines]   = len;
    nTrLines++;
    if (!nl) break;
    p = nl + 1;
  }
  int startTr = (nTrLines > trVisLines) ? (nTrLines - trVisLines) : 0;
  for (int r = 0; r < trVisLines && (startTr + r) < nTrLines; r++) {
    int idx = startTr + r;
    char lbuf[64];
    int l = lineLens[idx];
    if (l >= (int)sizeof(lbuf)) l = (int)sizeof(lbuf) - 1;
    memcpy(lbuf, lineStarts[idx], (size_t)l);
    lbuf[l] = '\0';
    drawTruncatedText(os, lbuf, cx + 6, trY + 2 + r * 10, maxChars + 2,
                      LAKATOS_GREEN, LAKATOS_BLACK, 1);
  }
  // Small [Clr] button in top-right of Transcript pane
  drawButton(os, cx + cw - 34, trY + 2, 30, 13, 0x2104, LAKATOS_DARKGREY, LAKATOS_LIGHTGREY, "Clr", 1);

  // 4. Bottom Status Strip (`stY`)
  uint16_t stBg = ws.lastEvalError ? 0x4800 : 0x10A2;
  uint16_t stFg = ws.lastEvalError ? LAKATOS_YELLOW : LAKATOS_CYAN;
  os->tft_fill_rect(cx + 2, stY, cw - 4, statusH, stBg);
  drawTruncatedText(os, ws.lastEvalResult, cx + 6, stY + 3, (cw - 12) / 6, stFg, stBg, 1);
}

static void renderClassBrowserClient(StUiManager* ui, StWindow& w, int cx, int cy, int cw, int ch) {
  const LakatosApi* os = ui->os;
  ClassBrowserWinState& cb = w.cb;

  int subBarH = 19;
  int statusH = 14;
  int bodyY   = cy + subBarH + 2;
  int bodyH   = ch - subBarH - statusH - 3;
  int stY     = bodyY + bodyH;

  if (!cb.inSourceView) {
    // -----------------------------------------------------------------------
    // View A: Dual-Column Class & Method Selector
    // -----------------------------------------------------------------------
    os->tft_fill_rect(cx + 2, cy + 1, cw - 4, subBarH, 0x1082);
    drawButton(os, cx + 4,   cy + 2, 46, 16, 0x18E3, LAKATOS_CYAN,   LAKATOS_WHITE, "+Class", 1);
    drawButton(os, cx + 52,  cy + 2, 46, 16, cb.isClassSide ? 0x3186 : LAKATOS_DARKCYAN,
               LAKATOS_CYAN, LAKATOS_WHITE, "Inst", 1);
    drawButton(os, cx + 100, cy + 2, 46, 16, cb.isClassSide ? LAKATOS_DARKCYAN : 0x3186,
               LAKATOS_CYAN, LAKATOS_WHITE, "Class", 1);
    drawButton(os, cx + 150, cy + 2, 48, 16, 0x2945, LAKATOS_YELLOW, LAKATOS_YELLOW, "+Meth", 1);
    drawButton(os, cx + 202, cy + 2, cw - 206, 16, LAKATOS_DARKGREEN, LAKATOS_GREEN,
               LAKATOS_WHITE, "Edit Src >", 1);

    int leftW  = (cw * 45) / 100;
    int rightX = cx + leftW + 2;
    int rightW = cw - leftW - 4;

    os->tft_fill_rect(cx + 2, bodyY, leftW - 2, bodyH, 0x0841);
    os->tft_draw_rect(cx + 2, bodyY, leftW - 2, bodyH, 0x3186);
    os->tft_fill_rect(rightX, bodyY, rightW, bodyH, 0x0841);
    os->tft_draw_rect(rightX, bodyY, rightW, bodyH, 0x3186);

    int pagerH = 18;
    int listH  = bodyH - pagerH - 2;
    int rowsPerPage = listH / 18;
    if (rowsPerPage < 3) rowsPerPage = 3;
    if (rowsPerPage > 6) rowsPerPage = 6;
    int rowH = listH / rowsPerPage;

    // Left column: Classes
    Oop clsList[kMaxClasses];
    int nCls = collectClassList(ui->vm, clsList, kMaxClasses);
    int totalClsPages = (nCls + rowsPerPage - 1) / rowsPerPage;
    if (totalClsPages < 1) totalClsPages = 1;
    if (cb.classPage >= totalClsPages) cb.classPage = 0;

    for (int r = 0; r < rowsPerPage; r++) {
      int idx = cb.classPage * rowsPerPage + r;
      if (idx >= nCls) break;
      Oop cOop = clsList[idx];
      bool sel = (cOop == cb.selectedClass);
      int ry = bodyY + 2 + r * rowH;
      uint16_t bg = sel ? 0x194A : 0x0841;
      if (sel) os->tft_fill_rect(cx + 3, ry, leftW - 4, rowH - 1, bg);
      char cname[32];
      stCopyCString(ui->vm, stSlotAt(ui->vm, cOop, SLOT_CLS_NAME), cname, sizeof(cname));
      drawTruncatedText(os, cname, cx + 6, ry + (rowH - 8) / 2, (leftW - 12) / 6,
                        sel ? LAKATOS_YELLOW : LAKATOS_WHITE, bg, 1);
    }
    int pgY = bodyY + bodyH - pagerH;
    drawButton(os, cx + 4, pgY, 34, 16, 0x2104, LAKATOS_LIGHTGREY, LAKATOS_WHITE, "<", 1);
    char cpgBuf[16];
    snprintf(cpgBuf, sizeof(cpgBuf), "%d/%d", cb.classPage + 1, totalClsPages);
    drawTruncatedText(os, cpgBuf, cx + 44, pgY + 4, 7, LAKATOS_LIGHTGREY, 0x0841, 1);
    drawButton(os, cx + leftW - 38, pgY, 34, 16, 0x2104, LAKATOS_LIGHTGREY, LAKATOS_WHITE, ">", 1);

    // Right column: Methods of `selectedClass`
    Oop mList[64];
    int nMeth = collectMethodList(ui->vm, cb.selectedClass, cb.isClassSide, mList, 64);
    int totalMethPages = (nMeth + rowsPerPage - 1) / rowsPerPage;
    if (totalMethPages < 1) totalMethPages = 1;
    if (cb.methodPage >= totalMethPages) cb.methodPage = 0;

    for (int r = 0; r < rowsPerPage; r++) {
      int idx = cb.methodPage * rowsPerPage + r;
      if (idx >= nMeth) break;
      Oop sOop = mList[idx];
      bool sel = (sOop == cb.selectedSelector);
      int ry = bodyY + 2 + r * rowH;
      uint16_t bg = sel ? 0x194A : 0x0841;
      if (sel) os->tft_fill_rect(rightX + 1, ry, rightW - 2, rowH - 1, bg);
      char sname[40];
      stCopyCString(ui->vm, sOop, sname, sizeof(sname));
      drawTruncatedText(os, sname, rightX + 5, ry + (rowH - 8) / 2, (rightW - 10) / 6,
                        sel ? LAKATOS_CYAN : 0xDEFB, bg, 1);
    }
    drawButton(os, rightX + 4, pgY, 36, 16, 0x2104, LAKATOS_LIGHTGREY, LAKATOS_WHITE, "<", 1);
    char mpgBuf[16];
    snprintf(mpgBuf, sizeof(mpgBuf), "%d/%d", cb.methodPage + 1, totalMethPages);
    drawTruncatedText(os, mpgBuf, rightX + 48, pgY + 4, 8, LAKATOS_LIGHTGREY, 0x0841, 1);
    drawButton(os, rightX + rightW - 40, pgY, 36, 16, 0x2104, LAKATOS_LIGHTGREY, LAKATOS_WHITE, ">", 1);
  } else {
    // -----------------------------------------------------------------------
    // View B: Method Source Editor & Live Smalltalk-80 Compiler
    // -----------------------------------------------------------------------
    os->tft_fill_rect(cx + 2, cy + 1, cw - 4, subBarH, 0x1082);
    drawButton(os, cx + 4, cy + 2, 48, 16, 0x2104, LAKATOS_CYAN, LAKATOS_CYAN, "< List", 1);

    char clsName[24], selName[28], hdrLabel[64];
    stCopyCString(ui->vm, stSlotAt(ui->vm, cb.selectedClass, SLOT_CLS_NAME), clsName, sizeof(clsName));
    stCopyCString(ui->vm, cb.selectedSelector, selName, sizeof(selName));
    snprintf(hdrLabel, sizeof(hdrLabel), "%s%s>>%s",
             clsName, cb.isClassSide ? " class" : "", selName);
    drawTruncatedText(os, hdrLabel, cx + 56, cy + 5, 25, LAKATOS_YELLOW, 0x1082, 1);

    drawButton(os, cx + cw - 92, cy + 2, 88, 16, LAKATOS_DARKGREEN, LAKATOS_GREEN,
               LAKATOS_WHITE, "Accept/Compile", 1);

    int tbH = 19;
    int edH = bodyH - tbH - 2;
    os->tft_fill_rect(cx + 2, bodyY, cw - 4, edH, 0x0841);
    os->tft_draw_rect(cx + 2, bodyY, cw - 4, edH, 0x3186);

    int lineH = 12;
    int visLines = (edH - 4) / lineH;
    if (visLines < 1) visLines = 1;
    if (cb.methodEditor.selectedLine < cb.methodEditor.scrollTop) {
      cb.methodEditor.scrollTop = cb.methodEditor.selectedLine;
    } else if (cb.methodEditor.selectedLine >= cb.methodEditor.scrollTop + visLines) {
      cb.methodEditor.scrollTop = cb.methodEditor.selectedLine - visLines + 1;
    }

    int maxChars = (cw - 30) / 6;
    for (int r = 0; r < visLines; r++) {
      int lIdx = cb.methodEditor.scrollTop + r;
      if (lIdx >= cb.methodEditor.lineCount) break;
      int ly = bodyY + 2 + r * lineH;
      bool sel = (lIdx == cb.methodEditor.selectedLine);
      uint16_t rowBg = sel ? 0x194A : 0x0841;
      if (sel) os->tft_fill_rect(cx + 3, ly, cw - 6, lineH, rowBg);
      char nbuf[8];
      snprintf(nbuf, sizeof(nbuf), "%d:", lIdx + 1);
      drawTruncatedText(os, nbuf, cx + 5, ly + 2, 3, LAKATOS_YELLOW, rowBg, 1);
      drawTruncatedText(os, cb.methodEditor.lines[lIdx], cx + 24, ly + 2, maxChars,
                        (lIdx == 0) ? LAKATOS_CYAN : LAKATOS_WHITE, rowBg, 1);
    }

    int tbY = bodyY + edH + 1;
    int bw  = (cw - 8) / 4;
    drawButton(os, cx + 2 + 0 * bw, tbY, bw - 2, 18, 0x2945, LAKATOS_YELLOW,    LAKATOS_YELLOW, "Edit Ln", 1);
    drawButton(os, cx + 2 + 1 * bw, tbY, bw - 2, 18, 0x18E3, LAKATOS_LIGHTGREY, LAKATOS_WHITE,  "+Line",   1);
    drawButton(os, cx + 2 + 2 * bw, tbY, bw - 2, 18, 0x3800, LAKATOS_RED,       LAKATOS_WHITE,  "-Line",   1);
    drawButton(os, cx + 2 + 3 * bw, tbY, bw - 2, 18, 0x2104, LAKATOS_CYAN,      LAKATOS_CYAN,   "Revert",  1);
  }

  // Bottom Status Strip
  uint16_t stBg = cb.statusError ? 0x4800 : 0x10A2;
  uint16_t stFg = cb.statusError ? LAKATOS_YELLOW : LAKATOS_CYAN;
  os->tft_fill_rect(cx + 2, stY, cw - 4, statusH, stBg);
  drawTruncatedText(os, cb.statusMsg, cx + 6, stY + 3, (cw - 12) / 6, stFg, stBg, 1);
}

static void renderFileBrowserClient(StUiManager* ui, StWindow& w, int cx, int cy, int cw, int ch) {
  const LakatosApi* os = ui->os;
  FileBrowserWinState& fb = w.fb;

  int topBarH = 19;
  int statusH = 14;
  int bodyY   = cy + topBarH + 2;
  int bodyH   = ch - topBarH - statusH - 3;
  int stY     = bodyY + bodyH;

  // VFS Tree Navigation Bar
  os->tft_fill_rect(cx + 2, cy + 1, cw - 4, topBarH, 0x1082);
  bool onFlash = strncmp(fb.currentDir, "/flash", 6) == 0;
  bool onSd    = strncmp(fb.currentDir, "/sd", 3) == 0;
  bool onNet   = strncmp(fb.currentDir, "/net", 4) == 0;
  drawButton(os, cx + 4,   cy + 2, 50, 16, onFlash ? LAKATOS_DARKCYAN : 0x2104, LAKATOS_CYAN, LAKATOS_WHITE, "/flash", 1);
  drawButton(os, cx + 56,  cy + 2, 42, 16, onSd    ? LAKATOS_DARKCYAN : 0x2104, LAKATOS_CYAN, LAKATOS_WHITE, "/sd",    1);
  drawButton(os, cx + 100, cy + 2, 44, 16, onNet   ? LAKATOS_DARKCYAN : 0x2104, LAKATOS_CYAN, LAKATOS_WHITE, "/net",   1);
  drawButton(os, cx + 146, cy + 2, 46, 16, 0x2104, LAKATOS_YELLOW, LAKATOS_YELLOW, ".. UP", 1);
  drawButton(os, cx + 194, cy + 2, cw - 198, 16, LAKATOS_DARKGREEN, LAKATOS_GREEN, LAKATOS_WHITE, "+Save WS", 1);

  if (!fb.inFileView) {
    os->tft_fill_rect(cx + 2, bodyY, cw - 4, bodyH, 0x0841);
    os->tft_draw_rect(cx + 2, bodyY, cw - 4, bodyH, 0x3186);

    int pagerH = 18;
    int listH  = bodyH - pagerH - 2;
    int rowsPerPage = listH / 19;
    if (rowsPerPage < 3) rowsPerPage = 3;
    if (rowsPerPage > 6) rowsPerPage = 6;
    int rowH = listH / rowsPerPage;

    int totalPages = (fb.entryCount + rowsPerPage - 1) / rowsPerPage;
    if (totalPages < 1) totalPages = 1;
    if (fb.pageIdx >= totalPages) fb.pageIdx = 0;

    if (fb.entryCount == 0) {
      drawTruncatedText(os, "(Directory is empty)", cx + 80, bodyY + listH / 2 - 4, 24,
                        LAKATOS_LIGHTGREY, 0x0841, 1);
    } else {
      for (int r = 0; r < rowsPerPage; r++) {
        int idx = fb.pageIdx * rowsPerPage + r;
        if (idx >= fb.entryCount) break;
        const LakatosFsEntry& e = fb.entries[idx];
        int ry = bodyY + 2 + r * rowH;
        uint16_t rbg = (r % 2 == 0) ? 0x1082 : 0x0841;
        os->tft_fill_rect(cx + 4, ry, cw - 8, rowH - 1, rbg);

        const char* badge = e.isDir ? "DIR" : "FILE";
        uint16_t bCol = e.isDir ? LAKATOS_YELLOW : LAKATOS_CYAN;
        drawTruncatedText(os, badge, cx + 8, ry + (rowH - 8) / 2, 4, bCol, rbg, 1);
        drawTruncatedText(os, e.name, cx + 38, ry + (rowH - 8) / 2, 28, LAKATOS_WHITE, rbg, 1);

        if (!e.isDir) {
          char szBuf[16];
          snprintf(szBuf, sizeof(szBuf), "%lldB", (long long)e.size);
          drawTruncatedText(os, szBuf, cx + cw - 52, ry + (rowH - 8) / 2, 8, LAKATOS_GREEN, rbg, 1);
        }
      }
    }

    int pgY = bodyY + bodyH - pagerH;
    drawButton(os, cx + 4, pgY, 44, 16, 0x2104, LAKATOS_LIGHTGREY, LAKATOS_WHITE, "< Prev", 1);
    drawButton(os, cx + 52, pgY, 54, 16, 0x2104, LAKATOS_CYAN, LAKATOS_CYAN, "Refresh", 1);
    char pstr[24];
    snprintf(pstr, sizeof(pstr), "Page %d/%d", fb.pageIdx + 1, totalPages);
    drawTruncatedText(os, pstr, cx + 120, pgY + 4, 12, LAKATOS_LIGHTGREY, 0x0841, 1);
    drawButton(os, cx + cw - 50, pgY, 46, 16, 0x2104, LAKATOS_LIGHTGREY, LAKATOS_WHITE, "Next >", 1);
  } else {
    // File Preview & Action View
    int actH = 19;
    int prvH = bodyH - actH - 2;
    os->tft_fill_rect(cx + 2, bodyY, cw - 4, prvH, LAKATOS_BLACK);
    os->tft_draw_rect(cx + 2, bodyY, cw - 4, prvH, LAKATOS_CYAN);

    int visLines = (prvH - 4) / 11;
    if (visLines < 1) visLines = 1;
    const char* p = fb.previewBuf;
    int lineIdx = 0;
    int drawn = 0;
    while (*p && drawn < visLines) {
      const char* nl = strchr(p, '\n');
      int len = nl ? (int)(nl - p) : (int)strlen(p);
      if (lineIdx >= fb.previewScrollLine) {
        char lbuf[64];
        if (len >= (int)sizeof(lbuf)) len = (int)sizeof(lbuf) - 1;
        memcpy(lbuf, p, (size_t)len);
        lbuf[len] = '\0';
        drawTruncatedText(os, lbuf, cx + 6, bodyY + 2 + drawn * 11, (cw - 12) / 6,
                          LAKATOS_GREENYELLOW, LAKATOS_BLACK, 1);
        drawn++;
      }
      lineIdx++;
      if (!nl) break;
      p = nl + 1;
    }

    int actY = bodyY + prvH + 1;
    int bw   = (cw - 8) / 4;
    drawButton(os, cx + 2 + 0 * bw, actY, bw - 2, 18, 0x2104, LAKATOS_CYAN, LAKATOS_CYAN, "< Dir", 1);
    drawButton(os, cx + 2 + 1 * bw, actY, bw - 2, 18, LAKATOS_DARKGREEN, LAKATOS_GREEN, LAKATOS_WHITE, "File-In", 1);
    drawButton(os, cx + 2 + 2 * bw, actY, bw - 2, 18, 0x2945, LAKATOS_YELLOW, LAKATOS_YELLOW, "-> Work", 1);
    drawButton(os, cx + 2 + 3 * bw, actY, bw - 2, 18,
               fb.confirmDeleteArmed ? LAKATOS_RED : 0x4800, LAKATOS_RED, LAKATOS_WHITE,
               fb.confirmDeleteArmed ? "Confirm?" : "Delete", 1);
  }

  // Bottom Status Strip
  os->tft_fill_rect(cx + 2, stY, cw - 4, statusH, 0x10A2);
  drawTruncatedText(os, fb.statusMsg, cx + 6, stY + 3, (cw - 12) / 6, LAKATOS_CYAN, 0x10A2, 1);
}

static void renderWindowFrameAndClient(StUiManager* ui, StWindow& w, bool isFrontmost) {
  const LakatosApi* os = ui->os;
  recomputeOverlappingBounds(ui);

  int x = w.x;
  int y = w.y;
  int ww = w.w;
  int wh = w.h;

  uint16_t borderCol = isFrontmost ? LAKATOS_CYAN : 0x4A69;
  uint16_t titleBg   = isFrontmost ? 0x194A : 0x18E3;
  uint16_t titleFg   = isFrontmost ? LAKATOS_WHITE : LAKATOS_LIGHTGREY;

  os->tft_fill_rect(x, y, ww, wh, 0x0841);
  os->tft_draw_rect(x, y, ww, wh, borderCol);

  // Window Title Bar (`16 px` high)
  int titleH = 16;
  os->tft_fill_rect(x + 1, y + 1, ww - 2, titleH, titleBg);
  os->tft_draw_fast_h_line(x + 1, y + titleH, ww - 2, borderCol);
  drawTruncatedText(os, w.title, x + 6, y + 4, (ww - 84) / 6, titleFg, titleBg, 1);

  // Title bar `[Max/Ovl]` and 2-tap `[X]` (`[Close?]`) buttons
  drawButton(os, x + ww - 68, y + 2, 30, 13, 0x2104, borderCol, LAKATOS_CYAN,
             ui->maximizedMode ? "Ovl" : "Max", 1);
  drawButton(os, x + ww - 36, y + 2, 34, 13,
             w.closeConfirmArmed ? LAKATOS_RED : 0x4800, LAKATOS_RED, LAKATOS_WHITE,
             w.closeConfirmArmed ? "Cls?" : "X", 1);

  // If this is a background window in Overlapping Mode and another window covers its body,
  // we only needed its exposed frame and title bar drawn before the front window paints over!
  int cx = x + 1;
  int cy = y + titleH + 1;
  int cw = ww - 2;
  int ch = wh - titleH - 2;
  if (ch <= 20) return;

  if (w.type == ST_WIN_WORKSPACE) {
    renderWorkspaceClient(ui, w, cx, cy, cw, ch);
  } else if (w.type == ST_WIN_CLASS_BROWSER) {
    renderClassBrowserClient(ui, w, cx, cy, cw, ch);
  } else if (w.type == ST_WIN_FILE_BROWSER) {
    renderFileBrowserClient(ui, w, cx, cy, cw, ch);
  }
}

static void renderTopSystemBar(StUiManager* ui) {
  const LakatosApi* os = ui->os;
  os->tft_fill_rect(0, 0, 320, 22, 0x10A2);
  os->tft_draw_fast_h_line(0, 21, 320, LAKATOS_CYAN);

  // Standard NW Exit X mark (sized via Lakatos corner size)
  int cs = lakatos_corner_size(os);
  lakatos_draw_exit_wedge(os);

  // Window Switcher Tabs (`x = tabX0..238`)
  int tabX0   = cs + 4;
  int maxTabs = (ui->winCount > 4) ? 4 : ui->winCount;
  int startZ  = ui->winCount - maxTabs;
  int tabW    = (238 - tabX0) / 4 - 1;
  for (int i = 0; i < maxTabs; i++) {
    uint8_t wIdx = ui->zOrder[startZ + i];
    StWindow& w = ui->windows[wIdx];
    bool isFront = (startZ + i == ui->winCount - 1);
    int tx = tabX0 + i * (tabW + 1);
    drawButton(os, tx, 2, tabW, 18,
               isFront ? LAKATOS_DARKCYAN : 0x18E3,
               isFront ? LAKATOS_YELLOW   : 0x4A69,
               isFront ? LAKATOS_WHITE    : LAKATOS_LIGHTGREY,
               w.tabLabel, 1);
  }

  // [+NEW] instantiate dropdown button (`x = 240..280`)
  drawButton(os, 240, 2, 40, 18,
             ui->newMenuOpen ? LAKATOS_YELLOW : LAKATOS_DARKGREEN,
             LAKATOS_GREEN,
             ui->newMenuOpen ? LAKATOS_BLACK : LAKATOS_WHITE,
             "+NEW", 1);

  // [MAX/OVL] layout toggle button (`x = 282..318`)
  drawButton(os, 282, 2, 36, 18, 0x2945, LAKATOS_CYAN, LAKATOS_CYAN,
             ui->maximizedMode ? "OVL" : "MAX", 1);
}

static void renderNewWindowMenu(StUiManager* ui) {
  if (!ui->newMenuOpen) return;
  const LakatosApi* os = ui->os;
  int mx = 148, my = 23, mw = 170, mh = 122;
  os->tft_fill_round_rect(mx, my, mw, mh, 5, 0x10A2);
  os->tft_draw_round_rect(mx, my, mw, mh, 5, LAKATOS_YELLOW);

  const char* labels[4] = {
    "+ Workspace / Eval",
    "+ Class Browser",
    "+ File Browser",
    "Run GC & Heap Info"
  };
  for (int i = 0; i < 4; i++) {
    drawButton(os, mx + 6, my + 5 + i * 28, mw - 12, 25,
               0x18E3, LAKATOS_CYAN, LAKATOS_WHITE, labels[i], 1);
  }
}

void stUiRender(StUiManager* ui) {
  if (!ui || !ui->os) return;
  const LakatosApi* os = ui->os;

  recomputeOverlappingBounds(ui);
  os->tft_fill_screen(LAKATOS_BLACK);
  renderTopSystemBar(ui);

  if (ui->winCount > 0) {
    if (ui->maximizedMode) {
      uint8_t topIdx = ui->zOrder[ui->winCount - 1];
      renderWindowFrameAndClient(ui, ui->windows[topIdx], true);
    } else {
      // Draw up to 3 top windows in back-to-front Z-order
      int startZ = (ui->winCount > 3) ? (ui->winCount - 3) : 0;
      for (int z = startZ; z < ui->winCount; z++) {
        uint8_t wIdx = ui->zOrder[z];
        bool isFront = (z == ui->winCount - 1);
        renderWindowFrameAndClient(ui, ui->windows[wIdx], isFront);
      }
    }
  }

  if (ui->newMenuOpen) {
    renderNewWindowMenu(ui);
  }
  lakatos_draw_exit_wedge(os);
  ui->needsRedraw = false;
}

// ---------------------------------------------------------------------------
// Touch Dispatch Inside Focused Tool Windows
// ---------------------------------------------------------------------------
static void evalWorkspaceBuffer(StUiManager* ui, WorkspaceWinState* ws, bool printResult) {
  char codeBuf[1024];
  stTextBufferJoin(&ws->editor, codeBuf, sizeof(codeBuf));
  ui->vm->lastError[0] = '\0';
  Oop res = stEval(ui->vm, codeBuf);
  if (ui->vm->lastError[0] != '\0') {
    snprintf(ws->lastEvalResult, sizeof(ws->lastEvalResult), "%s", ui->vm->lastError);
    ws->lastEvalError = true;
    stTranscriptAppendCStr(ui->vm, "! ");
    stTranscriptAppendCStr(ui->vm, ui->vm->lastError);
    stTranscriptAppendCStr(ui->vm, "\n");
  } else {
    char resStr[96];
    stFormatOop(ui->vm, res, resStr, sizeof(resStr));
    snprintf(ws->lastEvalResult, sizeof(ws->lastEvalResult), "=> %s", resStr);
    ws->lastEvalError = false;
    if (printResult) {
      stTranscriptAppendCStr(ui->vm, "=> ");
      stTranscriptAppendCStr(ui->vm, resStr);
      stTranscriptAppendCStr(ui->vm, "\n");
    }
  }
  ui->needsRedraw = true;
}

static void handleWorkspaceTouch(StUiManager* ui, StWindow& w, int cx, int cy, int cw, int ch, int tx, int ty) {
  WorkspaceWinState& ws = w.ws;
  int toolbarH = 20;
  int statusH  = 14;
  int availH   = ch - toolbarH - statusH - 4;
  int editorH  = (availH * 52) / 100;
  if (editorH < 44) editorH = 44;
  int transH   = availH - editorH;

  int edY = cy + 1;
  int tbY = edY + editorH + 1;
  int trY = tbY + toolbarH + 1;

  // 1. Tap inside Code Editor -> select line
  if (ty >= edY && ty < edY + editorH) {
    int row = (ty - (edY + 2)) / 12;
    int lIdx = ws.editor.scrollTop + row;
    if (lIdx >= 0 && lIdx < ws.editor.lineCount) {
      if (ws.editor.selectedLine == lIdx) {
        // Second tap on the already-selected line opens the Line Editor!
        runLineEditorModal(ui, "EDIT WORKSPACE LINE",
                           ws.editor.lines[lIdx], kMaxLineChars);
      } else {
        ws.editor.selectedLine = lIdx;
      }
      ui->needsRedraw = true;
    }
    return;
  }

  // 2. Tap on Action Toolbar (`tbY .. tbY + toolbarH`)
  if (ty >= tbY && ty <= tbY + toolbarH) {
    int bw  = (cw - 8) / 6;
    int btn = (tx - (cx + 2)) / bw;
    if (btn == 0) {
      evalWorkspaceBuffer(ui, &ws, false);
    } else if (btn == 1) {
      evalWorkspaceBuffer(ui, &ws, true);
    } else if (btn == 2) {
      int ln = ws.editor.selectedLine;
      if (ln >= 0 && ln < ws.editor.lineCount) {
        runLineEditorModal(ui, "EDIT WORKSPACE LINE",
                           ws.editor.lines[ln], kMaxLineChars);
        ui->needsRedraw = true;
      }
    } else if (btn == 3) {
      // +Line
      if (ws.editor.lineCount < kMaxEditorLines) {
        int ins = ws.editor.selectedLine + 1;
        for (int i = ws.editor.lineCount; i > ins; i--) {
          memcpy(ws.editor.lines[i], ws.editor.lines[i - 1], kMaxLineChars);
        }
        ws.editor.lines[ins][0] = '\0';
        ws.editor.lineCount++;
        ws.editor.selectedLine = ins;
        ui->needsRedraw = true;
      }
    } else if (btn == 4) {
      // -Line
      if (ws.editor.lineCount > 1) {
        int del = ws.editor.selectedLine;
        for (int i = del; i + 1 < ws.editor.lineCount; i++) {
          memcpy(ws.editor.lines[i], ws.editor.lines[i + 1], kMaxLineChars);
        }
        ws.editor.lineCount--;
        if (ws.editor.selectedLine >= ws.editor.lineCount) {
          ws.editor.selectedLine = ws.editor.lineCount - 1;
        }
        ui->needsRedraw = true;
      } else {
        ws.editor.lines[0][0] = '\0';
        ui->needsRedraw = true;
      }
    } else if (btn >= 5) {
      // Cycle built-in demo scripts
      ws.demoIdx = (ws.demoIdx + 1) % kNumDemos;
      stTextBufferSet(&ws.editor, kDemoScripts[ws.demoIdx].code);
      snprintf(ws.lastEvalResult, sizeof(ws.lastEvalResult),
               "Loaded Demo %s", kDemoScripts[ws.demoIdx].title);
      ws.lastEvalError = false;
      ui->needsRedraw = true;
    }
    return;
  }

  // 3. Tap on Transcript [Clr] button
  if (ty >= trY && ty <= trY + 18 && tx >= cx + cw - 38) {
    stTranscriptClear(ui->vm);
    ui->needsRedraw = true;
  }
}

static void handleClassBrowserTouch(StUiManager* ui, StWindow& w, int cx, int cy, int cw, int ch, int tx, int ty) {
  ClassBrowserWinState& cb = w.cb;
  int subBarH = 19;
  int statusH = 14;
  int bodyY   = cy + subBarH + 2;
  int bodyH   = ch - subBarH - statusH - 3;

  if (!cb.inSourceView) {
    // Sub-bar buttons
    if (ty >= cy && ty <= cy + subBarH + 2) {
      if (tx < cx + 51) {
        // [+Class]: prompt for new subclass name
        char clsSpec[64] = "MyObject";
        if (runLineEditorModal(ui, "NEW SUBCLASS NAME", clsSpec, sizeof(clsSpec)) && clsSpec[0]) {
          Oop newCls = stCreateClass(ui->vm, 0, clsSpec, cb.selectedClass, "x y");
          if (newCls != kOopNil) {
            cb.selectedClass = newCls;
            snprintf(cb.statusMsg, sizeof(cb.statusMsg), "Created subclass %s", clsSpec);
            cb.statusError = false;
          }
        }
      } else if (tx >= cx + 51 && tx < cx + 99) {
        cb.isClassSide = false;
        cb.methodPage  = 0;
        Oop mList[64];
        int n = collectMethodList(ui->vm, cb.selectedClass, false, mList, 64);
        cb.selectedSelector = (n > 0) ? mList[0] : kOopNil;
        loadSelectedMethodIntoEditor(ui->vm, &cb);
      } else if (tx >= cx + 99 && tx < cx + 148) {
        cb.isClassSide = true;
        cb.methodPage  = 0;
        Oop mList[64];
        int n = collectMethodList(ui->vm, cb.selectedClass, true, mList, 64);
        cb.selectedSelector = (n > 0) ? mList[0] : kOopNil;
        loadSelectedMethodIntoEditor(ui->vm, &cb);
      } else if (tx >= cx + 148 && tx < cx + 200) {
        // [+Meth]: start a new method template in source view
        stTextBufferSet(&cb.methodEditor, "myMethod\n  ^ 42");
        cb.inSourceView = true;
        strcpy(cb.statusMsg, "Edit method lines, then tap [Accept/Compile]");
        cb.statusError = false;
      } else {
        // [Edit Src >]
        loadSelectedMethodIntoEditor(ui->vm, &cb);
        cb.inSourceView = true;
      }
      ui->needsRedraw = true;
      return;
    }

    int leftW      = (cw * 45) / 100;
    int rightX     = cx + leftW + 2;
    int rightW     = cw - leftW - 4;
    int pagerH     = 18;
    int listH      = bodyH - pagerH - 2;
    int rowsPerPage= listH / 18;
    if (rowsPerPage < 3) rowsPerPage = 3;
    if (rowsPerPage > 6) rowsPerPage = 6;
    int rowH       = listH / rowsPerPage;
    int pgY        = bodyY + bodyH - pagerH;

    Oop clsList[kMaxClasses];
    int nCls = collectClassList(ui->vm, clsList, kMaxClasses);
    int totalClsPages = (nCls + rowsPerPage - 1) / rowsPerPage;
    if (totalClsPages < 1) totalClsPages = 1;

    Oop mList[64];
    int nMeth = collectMethodList(ui->vm, cb.selectedClass, cb.isClassSide, mList, 64);
    int totalMethPages = (nMeth + rowsPerPage - 1) / rowsPerPage;
    if (totalMethPages < 1) totalMethPages = 1;

    // Pager buttons
    if (ty >= pgY && ty <= pgY + pagerH + 2) {
      if (tx < rightX) {
        if (tx < cx + 44) cb.classPage = (cb.classPage + totalClsPages - 1) % totalClsPages;
        else if (tx >= cx + leftW - 44) cb.classPage = (cb.classPage + 1) % totalClsPages;
      } else {
        if (tx < rightX + 44) cb.methodPage = (cb.methodPage + totalMethPages - 1) % totalMethPages;
        else if (tx >= rightX + rightW - 44) cb.methodPage = (cb.methodPage + 1) % totalMethPages;
      }
      ui->needsRedraw = true;
      return;
    }

    // Row selection
    if (ty >= bodyY && ty < pgY) {
      int r = (ty - (bodyY + 2)) / rowH;
      if (r >= 0 && r < rowsPerPage) {
        if (tx < rightX) {
          int idx = cb.classPage * rowsPerPage + r;
          if (idx >= 0 && idx < nCls) {
            cb.selectedClass = clsList[idx];
            cb.methodPage    = 0;
            int nm = collectMethodList(ui->vm, cb.selectedClass, cb.isClassSide, mList, 64);
            cb.selectedSelector = (nm > 0) ? mList[0] : kOopNil;
            loadSelectedMethodIntoEditor(ui->vm, &cb);
            snprintf(cb.statusMsg, sizeof(cb.statusMsg), "Class selected (%d methods)", nm);
            cb.statusError = false;
            ui->needsRedraw = true;
          }
        } else {
          int idx = cb.methodPage * rowsPerPage + r;
          if (idx >= 0 && idx < nMeth) {
            if (cb.selectedSelector == mList[idx]) {
              // Tapping an already-selected method opens the Source View
              loadSelectedMethodIntoEditor(ui->vm, &cb);
              cb.inSourceView = true;
            } else {
              cb.selectedSelector = mList[idx];
              loadSelectedMethodIntoEditor(ui->vm, &cb);
              snprintf(cb.statusMsg, sizeof(cb.statusMsg), "%s", cb.methodEditor.lines[0]);
              cb.statusError = false;
            }
            ui->needsRedraw = true;
          }
        }
      }
    }
    return;
  }

  // View B: Method Source & Live Compiler View
  if (ty >= cy && ty <= cy + subBarH + 2) {
    if (tx <= cx + 56) {
      cb.inSourceView = false;
      ui->needsRedraw = true;
    } else if (tx >= cx + cw - 94) {
      // [Accept/Compile]
      char srcBuf[512];
      stTextBufferJoin(&cb.methodEditor, srcBuf, sizeof(srcBuf));
      Oop meth = stCompileMethod(ui->vm, cb.selectedClass, cb.isClassSide, srcBuf, -1);
      if (meth != kOopNil) {
        cb.selectedSelector = stSlotAt(ui->vm, meth, SLOT_METH_SELECTOR);
        char sname[40];
        stCopyCString(ui->vm, cb.selectedSelector, sname, sizeof(sname));
        snprintf(cb.statusMsg, sizeof(cb.statusMsg), "Compiled #%s OK!", sname);
        cb.statusError = false;
      } else {
        snprintf(cb.statusMsg, sizeof(cb.statusMsg), "%s", ui->vm->lastError);
        cb.statusError = true;
      }
      ui->needsRedraw = true;
    }
    return;
  }

  int tbH = 19;
  int edH = bodyH - tbH - 2;
  int tbY = bodyY + edH + 1;

  if (ty >= bodyY && ty < bodyY + edH) {
    int r = (ty - (bodyY + 2)) / 12;
    int lIdx = cb.methodEditor.scrollTop + r;
    if (lIdx >= 0 && lIdx < cb.methodEditor.lineCount) {
      if (cb.methodEditor.selectedLine == lIdx) {
        runLineEditorModal(ui, "EDIT METHOD LINE",
                           cb.methodEditor.lines[lIdx], kMaxLineChars);
      } else {
        cb.methodEditor.selectedLine = lIdx;
      }
      ui->needsRedraw = true;
    }
  } else if (ty >= tbY && ty <= tbY + tbH + 2) {
    int bw  = (cw - 8) / 4;
    int btn = (tx - (cx + 2)) / bw;
    if (btn == 0) {
      int ln = cb.methodEditor.selectedLine;
      if (ln >= 0 && ln < cb.methodEditor.lineCount) {
        runLineEditorModal(ui, "EDIT METHOD LINE",
                           cb.methodEditor.lines[ln], kMaxLineChars);
        ui->needsRedraw = true;
      }
    } else if (btn == 1) {
      if (cb.methodEditor.lineCount < kMaxEditorLines) {
        int ins = cb.methodEditor.selectedLine + 1;
        for (int i = cb.methodEditor.lineCount; i > ins; i--) {
          memcpy(cb.methodEditor.lines[i], cb.methodEditor.lines[i - 1], kMaxLineChars);
        }
        strcpy(cb.methodEditor.lines[ins], "  ");
        cb.methodEditor.lineCount++;
        cb.methodEditor.selectedLine = ins;
        ui->needsRedraw = true;
      }
    } else if (btn == 2) {
      if (cb.methodEditor.lineCount > 1) {
        int del = cb.methodEditor.selectedLine;
        for (int i = del; i + 1 < cb.methodEditor.lineCount; i++) {
          memcpy(cb.methodEditor.lines[i], cb.methodEditor.lines[i + 1], kMaxLineChars);
        }
        cb.methodEditor.lineCount--;
        if (cb.methodEditor.selectedLine >= cb.methodEditor.lineCount) {
          cb.methodEditor.selectedLine = cb.methodEditor.lineCount - 1;
        }
        ui->needsRedraw = true;
      }
    } else if (btn >= 3) {
      loadSelectedMethodIntoEditor(ui->vm, &cb);
      strcpy(cb.statusMsg, "Reverted method source");
      cb.statusError = false;
      ui->needsRedraw = true;
    }
  }
}

static void handleFileBrowserTouch(StUiManager* ui, StWindow& w, int cx, int cy, int cw, int ch, int tx, int ty) {
  FileBrowserWinState& fb = w.fb;
  int topBarH = 19;
  int statusH = 14;
  int bodyY   = cy + topBarH + 2;
  int bodyH   = ch - topBarH - statusH - 3;

  // Top VFS Bar
  if (ty >= cy && ty <= cy + topBarH + 2) {
    if (tx < cx + 55) {
      strcpy(fb.currentDir, "/flash");
      refreshFileBrowserDir(ui->os, &fb);
    } else if (tx < cx + 99) {
      strcpy(fb.currentDir, "/sd");
      refreshFileBrowserDir(ui->os, &fb);
    } else if (tx < cx + 145) {
      strcpy(fb.currentDir, "/net");
      refreshFileBrowserDir(ui->os, &fb);
    } else if (tx < cx + 193) {
      // [.. UP]
      if (fb.inFileView) {
        fb.inFileView = false;
      } else if (strcmp(fb.currentDir, "/flash") != 0 &&
                 strcmp(fb.currentDir, "/sd") != 0 &&
                 strcmp(fb.currentDir, "/net") != 0) {
        char* slash = strrchr(fb.currentDir, '/');
        if (slash && slash != fb.currentDir) *slash = '\0';
        refreshFileBrowserDir(ui->os, &fb);
      }
    } else {
      // [+Save WS]: save active Workspace code to `<currentDir>/workspace.st`
      const char* wsCode = "Smalltalk";
      char joinedWs[1024];
      for (int i = ui->winCount - 1; i >= 0; i--) {
        StWindow& cand = ui->windows[ui->zOrder[i]];
        if (cand.active && cand.type == ST_WIN_WORKSPACE) {
          stTextBufferJoin(&cand.ws.editor, joinedWs, sizeof(joinedWs));
          wsCode = joinedWs;
          break;
        }
      }
      char dstPath[128];
      snprintf(dstPath, sizeof(dstPath), "%s/workspace.st", fb.currentDir);
      if (ui->os && ui->os->fs_write && ui->os->fs_write(dstPath, wsCode, strlen(wsCode))) {
        refreshFileBrowserDir(ui->os, &fb);
        snprintf(fb.statusMsg, sizeof(fb.statusMsg), "Saved %s", dstPath);
      } else {
        snprintf(fb.statusMsg, sizeof(fb.statusMsg), "Save failed: %s", dstPath);
      }
    }
    ui->needsRedraw = true;
    return;
  }

  if (!fb.inFileView) {
    int pagerH = 18;
    int listH  = bodyH - pagerH - 2;
    int rowsPerPage = listH / 19;
    if (rowsPerPage < 3) rowsPerPage = 3;
    if (rowsPerPage > 6) rowsPerPage = 6;
    int rowH   = listH / rowsPerPage;
    int pgY    = bodyY + bodyH - pagerH;
    int totalPages = (fb.entryCount + rowsPerPage - 1) / rowsPerPage;
    if (totalPages < 1) totalPages = 1;

    if (ty >= pgY && ty <= pgY + pagerH + 2) {
      if (tx < cx + 50) fb.pageIdx = (fb.pageIdx + totalPages - 1) % totalPages;
      else if (tx < cx + 108) refreshFileBrowserDir(ui->os, &fb);
      else if (tx >= cx + cw - 54) fb.pageIdx = (fb.pageIdx + 1) % totalPages;
      ui->needsRedraw = true;
      return;
    }

    if (ty >= bodyY && ty < pgY) {
      int r = (ty - (bodyY + 2)) / rowH;
      int idx = fb.pageIdx * rowsPerPage + r;
      if (r >= 0 && r < rowsPerPage && idx >= 0 && idx < fb.entryCount) {
        openFileInBrowser(ui->os, &fb, idx);
        ui->needsRedraw = true;
      }
    }
    return;
  }

  // In File View
  int actH = 19;
  int prvH = bodyH - actH - 2;
  int actY = bodyY + prvH + 1;

  if (ty >= actY && ty <= actY + actH + 2) {
    int bw  = (cw - 8) / 4;
    int btn = (tx - (cx + 2)) / bw;
    if (btn == 0) {
      fb.inFileView = false;
      fb.confirmDeleteArmed = false;
      ui->needsRedraw = true;
    } else if (btn == 1) {
      // [File-In]: evaluate `fb.previewBuf` in VM!
      Oop res = stEval(ui->vm, fb.previewBuf);
      char rbuf[64];
      stFormatOop(ui->vm, res, rbuf, sizeof(rbuf));
      snprintf(fb.statusMsg, sizeof(fb.statusMsg), "File-In => %s", rbuf);
      ui->needsRedraw = true;
    } else if (btn == 2) {
      // [-> Work]: spawn or populate a Workspace with `fb.previewBuf`
      Oop strOop = stNewString(ui->vm, fb.previewBuf);
      stUiSpawnWindow(ui, ST_WIN_WORKSPACE, strOop);
    } else if (btn >= 3) {
      // 2-tap [Delete] -> [Confirm?]
      if (!fb.confirmDeleteArmed) {
        fb.confirmDeleteArmed = true;
        fb.confirmDeleteMs = (ui->os && ui->os->millis) ? ui->os->millis() : 0;
        strcpy(fb.statusMsg, "Tap [Confirm?] again to delete file");
      } else {
        char fullPath[160];
        snprintf(fullPath, sizeof(fullPath), "%s/%s", fb.currentDir, fb.selectedFile);
        if (ui->os && ui->os->fs_rm) ui->os->fs_rm(fullPath);
        refreshFileBrowserDir(ui->os, &fb);
      }
      ui->needsRedraw = true;
    }
  }
}

void stUiHandleTouch(StUiManager* ui, const FilteredTouchEvent& ev) {
  if (!ui || !ev.justClicked) return;
  int tx = ev.x;
  int ty = ev.y;

  // 1. If `+NEW` dropdown menu is open, handle menu selection or close on outside tap
  if (ui->newMenuOpen) {
    int mx = 148, my = 23, mw = 170, mh = 122;
    if (tx >= mx && tx <= mx + mw && ty >= my && ty <= my + mh) {
      int item = (ty - (my + 5)) / 28;
      ui->newMenuOpen = false;
      if (item == 0) {
        stUiSpawnWindow(ui, ST_WIN_WORKSPACE, kOopNil);
      } else if (item == 1) {
        stUiSpawnWindow(ui, ST_WIN_CLASS_BROWSER, ui->vm->classes[CLS_OBJECT]);
      } else if (item == 2) {
        stUiSpawnWindow(ui, ST_WIN_FILE_BROWSER, kOopNil);
      } else if (item >= 3) {
        stRunGC(ui->vm);
        char msg[80];
        snprintf(msg, sizeof(msg), "[GC #%lu] Live Oops: %u | Free Arena: %u B\n",
                 (unsigned long)ui->vm->gcCount,
                 (unsigned)ui->vm->liveObjCount,
                 (unsigned)((kPayloadWords - ui->vm->payloadUsedWords) * 4));
        stTranscriptAppendCStr(ui->vm, msg);
        ui->needsRedraw = true;
      }
      return;
    }
    ui->newMenuOpen = false;
    ui->needsRedraw = true;
    return;
  }

  // 2. Top System & Window Switcher Bar (`ty < 22` or NW corner)
  if (lakatos_is_exit_wedge(ui->os, tx, ty)) {
    ui->shouldExit = true;
    if (ui->os && ui->os->exit) ui->os->exit(0);
    return;
  }
  if (ty < 22) {
    if (tx >= 240 && tx < 281) {
      ui->newMenuOpen = !ui->newMenuOpen;
      ui->needsRedraw = true;
      return;
    }
    if (tx >= 281) {
      ui->maximizedMode = !ui->maximizedMode;
      recomputeOverlappingBounds(ui);
      ui->needsRedraw = true;
      return;
    }
    // Window switcher tabs (`x = tabX0..238`)
    int tabX0   = lakatos_corner_size(ui->os) + 4;
    int tabStep = (238 - tabX0) / 4;
    int maxTabs = (ui->winCount > 4) ? 4 : ui->winCount;
    int startZ  = ui->winCount - maxTabs;
    if (tx >= tabX0 && tabStep > 0) {
      int tabIdx = (tx - tabX0) / tabStep;
      if (tabIdx >= 0 && tabIdx < maxTabs) {
        uint8_t targetWin = ui->zOrder[startZ + tabIdx];
        bringWindowToFront(ui, targetWin);
      }
    }
    return;
  }

  if (ui->winCount <= 0) return;
  recomputeOverlappingBounds(ui);

  // 3. Check if user tapped the frontmost window vs an exposed background window title bar
  uint8_t frontIdx = ui->zOrder[ui->winCount - 1];
  StWindow& frontW = ui->windows[frontIdx];

  if (!ui->maximizedMode) {
    // If tap is above `frontW.y`, check background windows from front-1 down to 0
    if (ty < frontW.y) {
      for (int z = ui->winCount - 2; z >= 0; z--) {
        uint8_t bgIdx = ui->zOrder[z];
        StWindow& bgW = ui->windows[bgIdx];
        if (tx >= bgW.x && tx < bgW.x + bgW.w && ty >= bgW.y && ty < bgW.y + bgW.h) {
          bringWindowToFront(ui, bgIdx);
          return;
        }
      }
    }
  }

  // 4. Inside `frontW`
  if (tx < frontW.x || tx >= frontW.x + frontW.w ||
      ty < frontW.y || ty >= frontW.y + frontW.h) {
    return;
  }

  int titleH = 16;
  if (ty <= frontW.y + titleH) {
    // Title bar buttons: `[Max/Ovl]` and 2-tap `[X]` (`[Cls?]`)
    if (tx >= frontW.x + frontW.w - 36) {
      if (!frontW.closeConfirmArmed) {
        frontW.closeConfirmArmed = true;
        frontW.closeConfirmMs = (ui->os && ui->os->millis) ? ui->os->millis() : 0;
        ui->needsRedraw = true;
      } else {
        closeWindowAt(ui, frontIdx);
      }
    } else if (tx >= frontW.x + frontW.w - 70) {
      ui->maximizedMode = !ui->maximizedMode;
      recomputeOverlappingBounds(ui);
      ui->needsRedraw = true;
    } else if (frontW.closeConfirmArmed) {
      frontW.closeConfirmArmed = false;
      ui->needsRedraw = true;
    }
    return;
  }

  if (frontW.closeConfirmArmed) {
    frontW.closeConfirmArmed = false;
    ui->needsRedraw = true;
  }

  int cx = frontW.x + 1;
  int cy = frontW.y + titleH + 1;
  int cw = frontW.w - 2;
  int ch = frontW.h - titleH - 2;

  if (frontW.type == ST_WIN_WORKSPACE) {
    handleWorkspaceTouch(ui, frontW, cx, cy, cw, ch, tx, ty);
  } else if (frontW.type == ST_WIN_CLASS_BROWSER) {
    handleClassBrowserTouch(ui, frontW, cx, cy, cw, ch, tx, ty);
  } else if (frontW.type == ST_WIN_FILE_BROWSER) {
    handleFileBrowserTouch(ui, frontW, cx, cy, cw, ch, tx, ty);
  }
}

bool stUiStep(StUiManager* ui) {
  if (!ui || !ui->os) return false;
  const LakatosApi* os = ui->os;

  // Auto-expire 2-tap close/delete confirmations after 3 seconds
  uint32_t now = os->millis ? os->millis() : 0;
  for (int i = 0; i < ui->winCount; i++) {
    StWindow& w = ui->windows[ui->zOrder[i]];
    if (w.closeConfirmArmed && (uint32_t)(now - w.closeConfirmMs) > 3000u) {
      w.closeConfirmArmed = false;
      ui->needsRedraw = true;
    }
    if (w.type == ST_WIN_FILE_BROWSER && w.fb.confirmDeleteArmed &&
        (uint32_t)(now - w.fb.confirmDeleteMs) > 3000u) {
      w.fb.confirmDeleteArmed = false;
      ui->needsRedraw = true;
    }
  }

  if (ui->needsRedraw) {
    stUiRender(ui);
  }

  FilteredTouchEvent ev = touchFilterPoll(&ui->touch, os);
  if (ev.justClicked) {
    stUiHandleTouch(ui, ev);
    if (ui->shouldExit) return false;
    if (ui->needsRedraw) {
      stUiRender(ui);
    }
  }

  // USB Serial input: allow typing into the selected line of the active Workspace/Browser,
  // or pressing Ctrl-E (0x05) to evaluate, or Ctrl-C (0x03) to exit to Lakatos OS.
  while (os->serial_available && os->serial_available() > 0) {
    int ch = os->serial_read();
    if (ch == 3) {
      ui->shouldExit = true;
      if (os->exit) os->exit(0);
      return false;
    }
    if (ui->winCount > 0) {
      StWindow& frontW = ui->windows[ui->zOrder[ui->winCount - 1]];
      if (frontW.type == ST_WIN_WORKSPACE) {
        TextBuffer& tb = frontW.ws.editor;
        char* ln = tb.lines[tb.selectedLine];
        size_t l = strlen(ln);
        if (ch == 5) {  // Ctrl-E: Print It
          evalWorkspaceBuffer(ui, &frontW.ws, true);
        } else if (ch == 8 || ch == 127) {
          if (l > 0) { ln[l - 1] = '\0'; ui->needsRedraw = true; }
        } else if (ch >= 32 && ch < 127 && l + 1 < kMaxLineChars) {
          ln[l] = (char)ch;
          ln[l + 1] = '\0';
          ui->needsRedraw = true;
        }
      }
    }
  }

  if (ui->needsRedraw) {
    stUiRender(ui);
  }
  return !ui->shouldExit;
}
