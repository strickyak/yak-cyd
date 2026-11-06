#include "lakatos_api.h"
#include "st_ui.h"
#include "st_vm.h"
#include <stdio.h>
#include <string.h>

static VMState     gVm;
static StUiManager gUi;

extern "C" void app_main(const LakatosApi* os) {
  if (!os) return;

  stVmInit(&gVm, os);
  if (!gVm.objects || !gVm.payload) {
    char err[96];
    int n = snprintf(err, sizeof(err),
                     "\r\n[SmallerTalk-80] Heap alloc failed (free_heap=%u B)\r\n",
                     (unsigned)(os->free_heap_bytes ? os->free_heap_bytes() : 0));
    if (n > 0 && os->serial_write) os->serial_write(err, (size_t)n);
    os->tft_fill_screen(LAKATOS_BLACK);
    os->tft_set_text_color(LAKATOS_RED, LAKATOS_BLACK);
    os->tft_draw_string("SmallerTalk: Out of Heap", 20, 110, 2);
    os->delay_ms(2000);
    os->exit(1);
    return;
  }

  stUiInit(&gUi, &gVm, os);
  stUiRender(&gUi);

  if (os->serial_write) {
    char msg[128];
    int n = snprintf(msg, sizeof(msg),
                     "\r\n[SmallerTalk-80] Ready: %u classes, %u symbols, %u live objects, free heap %u B\r\n",
                     (unsigned)gVm.classCount, (unsigned)gVm.symbolCount,
                     (unsigned)gVm.liveObjCount, (unsigned)os->free_heap_bytes());
    if (n > 0) os->serial_write(msg, (size_t)n);
  }

  while (stUiStep(&gUi)) {
    os->delay_ms(10);
  }

  stMemDestroy(&gVm);
  os->exit(0);
}
