#pragma once

#include "st_vm.h"
#include "touch_filter.h"

static constexpr int kMaxWindows        = 6;
static constexpr int kMaxEditorLines    = 16;
static constexpr int kMaxLineChars      = 80;
static constexpr int kMaxFileEntries    = 24;
static constexpr int kFilePreviewCap    = 768;

struct TextBuffer {
  char lines[kMaxEditorLines][kMaxLineChars];
  int  lineCount;
  int  selectedLine;
  int  scrollTop;
};

struct WorkspaceWinState {
  TextBuffer editor;
  int        transcriptScrollLines;
  int        demoIdx;
  char       lastEvalResult[96];
  bool       lastEvalError;
};

struct ClassBrowserWinState {
  Oop        selectedClass;
  Oop        selectedSelector;
  bool       isClassSide;
  bool       inSourceView;
  int        classPage;
  int        methodPage;
  TextBuffer methodEditor;
  char       statusMsg[80];
  bool       statusError;
};

struct FileBrowserWinState {
  char           currentDir[96];
  LakatosFsEntry entries[kMaxFileEntries];
  int            entryCount;
  int            pageIdx;
  bool           inFileView;
  char           selectedFile[64];
  int64_t        selectedFileSize;
  char           previewBuf[kFilePreviewCap];
  int            previewScrollLine;
  bool           confirmDeleteArmed;
  uint32_t       confirmDeleteMs;
  char           statusMsg[80];
};

struct StWindow {
  bool     active;
  uint8_t  type;          // ST_WIN_WORKSPACE, ST_WIN_CLASS_BROWSER, ST_WIN_FILE_BROWSER
  uint8_t  instanceId;    // 1, 2, ...
  char     title[28];
  char     tabLabel[12];
  int16_t  x;
  int16_t  y;
  int16_t  w;
  int16_t  h;
  bool     closeConfirmArmed;
  uint32_t closeConfirmMs;

  union {
    WorkspaceWinState    ws;
    ClassBrowserWinState cb;
    FileBrowserWinState  fb;
  };
};

struct StUiManager {
  VMState*          vm;
  const LakatosApi* os;
  TouchFilterState  touch;

  StWindow          windows[kMaxWindows];
  uint8_t           zOrder[kMaxWindows];  // zOrder[0] = backmost, zOrder[winCount-1] = focused frontmost
  int               winCount;
  uint8_t           nextInstanceSeq;

  bool              maximizedMode;        // false = Overlapping desktop, true = Maximized active window
  bool              newMenuOpen;
  bool              needsRedraw;
  bool              shouldExit;
};

// Initializes the UI manager and spawns the initial 3 functional windows:
// FileBrowser (/flash), ClassBrowser (SmallInteger), and Workspace/Transcript (topmost).
void stUiInit(StUiManager* ui, VMState* vm, const LakatosApi* os);

// Instantiates a new window of `winType` (`ST_WIN_*`) and brings it to the front.
int  stUiSpawnWindow(StUiManager* ui, int winType, Oop argOop = kOopNil);

// Renders the entire 320x240 screen.
void stUiRender(StUiManager* ui);

// Processes a stabilized touch event (or call `stUiStep` in main loop).
void stUiHandleTouch(StUiManager* ui, const FilteredTouchEvent& ev);

// Polls touch + serial and redraws when dirty; returns false when exiting to Lakatos OS.
bool stUiStep(StUiManager* ui);

// Helper to join a `TextBuffer`'s lines into a single `\n`-separated string.
void stTextBufferJoin(const TextBuffer* tb, char* outBuf, size_t maxLen);

// Helper to split a `\n`-separated string into a `TextBuffer`.
void stTextBufferSet(TextBuffer* tb, const char* text);
