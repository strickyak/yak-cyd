/*
 * Linker script for standalone Lakatos applications (.lapp)
 *
 * The 768 KB app_slot flash partition (0x00180000) is dual-mapped by Lakatos:
 *   - Instruction space (IROM): 0x40300000 .. 0x403BFFFF
 *   - Read-only data   (DROM): 0x3F700000 .. 0x3F7BFFFF
 * And a 32 KB static RAM arena is reserved at:
 *   - Data/BSS space   (DRAM): 0x3FFC8000 .. 0x3FFCFFFF
 *     (0x3FFC8000..0x3FFC8007 holds the LakatosApi* pointer + reserved word)
 */

ENTRY(_lakatos_app_entry)

SECTIONS
{
  /* .text starts right after the 128-byte (0x80) LakatosAppHeader */
  . = 0x40300080;
  .text : ALIGN(16)
  {
    _text_start = ABSOLUTE(.);
    *(.entry.literal .entry.text)
    *(.literal .literal.* .text .text.*)
    *(.stub .gnu.linkonce.literal.* .gnu.linkonce.t.*.literal .gnu.linkonce.t.*)
    . = ALIGN(16);
    _text_end = ABSOLUTE(.);
  }

  _text_file_offset = 0x80;
  _text_size = _text_end - _text_start;
  _rodata_file_offset = _text_file_offset + _text_size;

  /* .rodata is accessed through the DROM mapping at 0x3F700000 + file_offset */
  . = 0x3F700000 + _rodata_file_offset;
  .rodata : ALIGN(16)
  {
    _rodata_start = ABSOLUTE(.);
    *(.rodata .rodata.* .gnu.linkonce.r.*)
    *(.srodata .srodata.*)
    . = ALIGN(4);
    __init_array_start = ABSOLUTE(.);
    KEEP(*(SORT(.init_array.*) .init_array))
    __init_array_end = ABSOLUTE(.);
    . = ALIGN(16);
    _rodata_end = ABSOLUTE(.);
  }

  _rodata_size = _rodata_end - _rodata_start;
  _data_file_offset = _rodata_file_offset + _rodata_size;

  /* .data and .bss live in the fixed 32 KB App DRAM Arena (0x3FFC8008..0x3FFCFFFF) */
  . = 0x3FFC8008;
  .data : AT(0x3F700000 + _data_file_offset) ALIGN(8)
  {
    _data_start = ABSOLUTE(.);
    *(.data .data.* .gnu.linkonce.d.*)
    *(.sdata .sdata.* .gnu.linkonce.s.*)
    . = ALIGN(8);
    _data_end = ABSOLUTE(.);
  }

  _data_size = _data_end - _data_start;

  /*
   * Overlay .bss sections of multi-headed native apps (piano, modtrack,
   * midiplay, radio) at the same RAM base address since only one head runs
   * at a time and the loader zeroes all 32 KB before entry.
   */
  . = ALIGN(8);
  _bss_start = ABSOLUTE(.);

  OVERLAY _bss_start : NOCROSSREFS AT(0x3F7B0000)
  {
    .bss.piano
    {
      *piano.o(.bss .bss.* .sbss .sbss.* .scommon COMMON)
    }
    .bss.modtrack
    {
      *modtrack.o(.bss .bss.* .sbss .sbss.* .scommon COMMON)
    }
    .bss.midiplay
    {
      *midiplay.o(.bss .bss.* .sbss .sbss.* .scommon COMMON)
    }
    .bss.radio
    {
      *radio.o(.bss .bss.* .sbss .sbss.* .scommon COMMON)
    }
    .bss.ham
    {
      *ham.o(.bss .bss.* .sbss .sbss.* .scommon COMMON)
    }
    .bss.smallertalk
    {
      *smallertalk*.o(.bss .bss.* .sbss .sbss.* .scommon COMMON)
    }
    .bss.ambient
    {
      *ambient.o(.bss .bss.* .sbss .sbss.* .scommon COMMON)
    }
  }

  .bss (NOLOAD) : ALIGN(8)
  {
    *(.bss .bss.* .gnu.linkonce.b.*)
    *(.sbss .sbss.* .gnu.linkonce.sb.* .scommon)
    *(COMMON)
    . = ALIGN(8);
    _bss_end = ABSOLUTE(.);
  }

  _bss_size = _bss_end - _bss_start;

  ASSERT((_bss_end <= 0x3FFD0000), "Lakatos app .data + .bss exceeds 32 KB static arena (use os->malloc for large buffers)")
  ASSERT(((_data_file_offset + _data_size) <= 0x0C0000), "Lakatos app image exceeds 768 KB app_slot")

  /DISCARD/ :
  {
    *(.eh_frame .eh_frame_hdr)
    *(.comment)
    *(.xtensa.info)
    *(.xt.prop .xt.prop.* .xt.lit .xt.lit.*)
  }
}
