/* NODISKEMU - SD/MMC to IEEE-488 interface/controller
   Copyright (C) 2007-2018  Ingo Korb <ingo@akana.de>

   NODISKEMU is a fork of sd2iec by Ingo Korb (et al.), http://sd2iec.de

   Inspired by MMC2IEC by Lars Pontoppidan et al.

   FAT filesystem access based on code from ChaN and Jim Brain, see ff.c|h.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA


   d64ops.c: D64 operations

*/

#include <stdint.h>
#include <string.h>
#include "config.h"
#include "buffers.h"
#include "dirent.h"
#include "errormsg.h"
#include "fatops.h"
#include "ff.h"
#include "parser.h"
#include "progmem.h"
#include "rtc.h"
#include "ustring.h"
#include "wrapops.h"
#include "d64ops.h"
#include "uart.h"

#define D41_ERROR_OFFSET 174848
#define D71_ERROR_OFFSET 349696
#define D81_ERROR_OFFSET 819200
#define D80_ERROR_OFFSET 533248
#define D82_ERROR_OFFSET 1066496

#define D41_BAM_TRACK           18
#define D41_BAM_SECTOR          0
#define D41_BAM_BYTES_PER_TRACK 4
#define D41_BAM_BITFIELD_BYTES  3

#define D81_BAM_TRACK           40
#define D81_BAM_SECTOR1         1
#define D81_BAM_SECTOR2         2
#define D81_BAM_OFFSET          10
#define D81_BAM_BYTES_PER_TRACK 6
#define D81_BAM_BITFIELD_BYTES  5

#define D71_BAM2_TRACK  53
#define D71_BAM2_SECTOR 0
#define D71_BAM2_BYTES_PER_TRACK 3
#define D71_BAM_COUNTER2OFFSET   0xdd

#define DNP_BAM_TRACK                     1
#define DNP_BAM_SECTOR                    2
#define DNP_BAM_LAST_TRACK_OFS            8
#define DNP_ROOTDIR_SECTOR               34 // only for formatting!
#define DNP_BAM_BYTES_PER_TRACK          32
#define DNP_DIRHEADER_DIR_TRACK           0
#define DNP_DIRHEADER_DIR_SECTOR          1
#define DNP_DIRHEADER_ROOTHDR_TRACK      32
#define DNP_DIRHEADER_ROOTHDR_SECTOR     33
#define DNP_DIRHEADER_PARENTHDR_TRACK    34
#define DNP_DIRHEADER_PARENTHDR_SECTOR   35
#define DNP_DIRHEADER_PARENTENTRY_TRACK  36
#define DNP_DIRHEADER_PARENTENTRY_SECTOR 37
#define DNP_DIRHEADER_PARENTENTRY_OFFSET 38
#define DNP_LABEL_OFFSET                  4
#define DNP_LABEL_AREA_SIZE             (28-4+1)
#define DNP_ID_OFFSET                    22

#define D80_BAM_TRACK           38
#define D80_DIR_TRACK           39
#define D80_BAM_SECTOR          0
#define D80_BAM_INTERLEAVE      3
#define D80_BAM_BYTES_PER_TRACK 5
#define D80_BAM_BITFIELD_BYTES  4

/* used for error info only */
#define MAX_SECTORS_PER_TRACK 40

typedef enum { BAM_BITFIELD, BAM_FREECOUNT } bamdata_t;

struct {
  uint8_t part;
  uint8_t track;
  uint8_t errors[MAX_SECTORS_PER_TRACK];
} errorcache;

static buffer_t *bam_buffer;  // recently-used buffer
static buffer_t *bam_buffer2; // secondary buffer
static uint8_t   bam_refcount;

/* ------------------------------------------------------------------------- */
/*  Forward declarations                                                     */
/* ------------------------------------------------------------------------- */

static uint8_t d64_opendir(dh_t *dh, path_t *path);

static void format_d41_image(uint8_t part, buffer_t *buf, uint8_t *name, uint8_t *idbuf);
static void format_d71_image(uint8_t part, buffer_t *buf, uint8_t *name, uint8_t *idbuf);
static void format_d81_image(uint8_t part, buffer_t *buf, uint8_t *name, uint8_t *idbuf);
static void format_d80_image(uint8_t part, buffer_t *buf, uint8_t *name, uint8_t *idbuf);
static void format_d82_image(uint8_t part, buffer_t *buf, uint8_t *name, uint8_t *idbuf);
static void format_dnp_image(uint8_t part, buffer_t *buf, uint8_t *name, uint8_t *idbuf);

/* ------------------------------------------------------------------------- */
/*  Utility functions                                                        */
/* ------------------------------------------------------------------------- */

static const PROGMEM struct param_s d41param = {
  18, 1, 35, 0x90, 0xa2, 10, 3, 0, format_d41_image
};

static const PROGMEM struct param_s d71param = {
  18, 1, 70, 0x90, 0xa2, 6, 3, 0, format_d71_image
};

static const PROGMEM struct param_s d81param = {
  40, 3, 80, 0x04, 0x16, 1, 1, 1, format_d81_image
};

static const PROGMEM struct param_s dnpparam = {
  1, 1, 0, DNP_LABEL_OFFSET, DNP_ID_OFFSET, 1, 1, 1, format_dnp_image
};

static const PROGMEM struct param_s d80param = {
  39, 1, 77, 6, 0x18, 5, 3, 0, format_d80_image
};

static const PROGMEM struct param_s d82param = {
  39, 1, 154, 6, 0x18, 5, 3, 1, format_d82_image
};


/**
 * get_param - get a format-dependent parameter
 * @part : partition number
 * @param: parameter to retrieve
 *
 * Returns a parameter that is format-dependent for the image type mounted
 * on the specified partition.
 */
static uint8_t get_param(uint8_t part, param_t param) {
  return *(((uint8_t *)&partition[part].d64data)+param);
}

/**
 * sector_lba - Transform a track/sector pair into a LBA sector number
 * @part  : partition number
 * @track : Track number
 * @sector: Sector number
 *
 * Calculates an LBA-style sector number for a given track/sector pair.
 */
/* This version used the least code of all tested variants. */
static uint16_t sector_lba(uint8_t part, uint8_t track, const uint8_t sector) {
  uint16_t offset = 0;

  track--; /* Track numbers are 1-based */

  switch (partition[part].imagetype & D64_TYPE_MASK) {
  case D64_TYPE_D41:
  case D64_TYPE_D71:
  default:
    if (track >= 35) {
      offset = 683;
      track -= 35;
    }
    if (track < 17)
      return track*21 + sector + offset;
    if (track < 24)
      return 17*21 + (track-17)*19 + sector + offset;
    if (track < 30)
      return 17*21 + 7*19 + (track-24)*18 + sector + offset;
    return 17*21 + 7*19 + 6*18 + (track-30)*17 + sector + offset;

  case D64_TYPE_D81:
    return track*40 + sector;

  case D64_TYPE_DNP:
    return track*256 + sector;

  case D64_TYPE_D80:
  case D64_TYPE_D82:
    if (track >= 78) {
      offset = 2083;
      track -= 77;
    }
    if (track < 39)
      return track*29 + sector + offset;
    if (track < 53)
      return 39*29 + (track-39)*27 + sector + offset;
    if (track < 64)
      return 39*29 + 14*27 + (track-53)*25 + sector + offset;
    return 39*29 + 14*27 + 11*25 + (track-64) + sector + offset;
  }
}

/**
 * sector_offset - Transform a track/sector pair into a D64 offset
 * @part  : partition number
 * @track : Track number
 * @sector: Sector number
 *
 * Calculates an offset into a D64 file from a track and sector number.
 */
static uint32_t sector_offset(uint8_t part, uint8_t track, const uint8_t sector) {
  return 256L * sector_lba(part,track,sector);
}

/**
 * sectors_per_track - number of sectors on given track
 * @part : partition number
 * @track: Track number
 *
 * This function returns the number of sectors on the given track
 * of a 1541/71/81 disk. Invalid track numbers will return invalid results.
 */
static uint16_t sectors_per_track(uint8_t part, uint8_t track) {
  switch (partition[part].imagetype & D64_TYPE_MASK) {
  case D64_TYPE_D41:
  case D64_TYPE_D71:
  default:
    if (track > 35)
      track -= 35;
    if (track < 18)
      return 21;
    if (track < 25)
      return 19;
    if (track < 31)
      return 18;
    return 17;

  case D64_TYPE_D81:
    return 40;

  case D64_TYPE_DNP:
    return 256;

  case D64_TYPE_D80:
  case D64_TYPE_D82:
    if (track > 77)
       track -= 77;
    if (track < 40)
       return 29;
    if (track < 54)
       return 27;
    if (track < 65)
       return 25;
    return 23;
  }
}

/**
 * checked_read_full - read a specified sector after range-checking
 * @part  : partition number
 * @track : track number to be read
 * @sector: sector number to be read
 * @offset: offset in sector to read from
 * @buf   : pointer to where the data should be read to
 * @len   : number of bytes to be read
 * @error : error number to be flagged if the range check fails
 *
 * This function checks if the track and sector are within the
 * limits for the image format and calls image_read to read
 * the data if they are. Returns the result of image_read or
 * 2 if the range check failed.
 */
static uint8_t checked_read_full(uint8_t part, uint8_t track, uint8_t sector, uint8_t offset,
                                 uint8_t *buf, uint16_t len, uint8_t error) {
uart_puts_P(PSTR(">CReadF:")); //TODO:BZ:DELME
uart_puthex(track);
uart_putc(' ');
uart_puthex(sector);
uart_putc(' ');
uart_puthex(offset);
uart_putcrlf();
uart_flush();
  if (track < 1 || track > get_param(part, LAST_TRACK) ||
      sector >= sectors_per_track(part, track)) {
    set_error_ts(error,track,sector);
    return 2;
  }

  if (partition[part].imagetype & D64_HAS_ERRORINFO) {
    /* Check if the sector is marked as bad */
    if (errorcache.part != part || errorcache.track != track) {
      /* Read the error info for this track */
      memset(errorcache.errors, 1, sizeof(errorcache.errors));

      switch (partition[part].imagetype & D64_TYPE_MASK) {
      case D64_TYPE_D41:
        if (image_read(part, D41_ERROR_OFFSET + sector_lba(part,track,0),
                       errorcache.errors, sectors_per_track(part, track)) >= 2)
          return 2;
        break;

      case D64_TYPE_D71:
        if (image_read(part, D71_ERROR_OFFSET + sector_lba(part,track,0),
                       errorcache.errors, sectors_per_track(part, track)) >= 2)
          return 2;
        break;

      case D64_TYPE_D81:
        if (image_read(part, D81_ERROR_OFFSET + sector_lba(part,track,0),
                       errorcache.errors, sectors_per_track(part, track)) >= 2)
          return 2;
        break;

      case D64_TYPE_D80:
        if (image_read(part, D80_ERROR_OFFSET + sector_lba(part,track,0),
                       errorcache.errors, sectors_per_track(part, track)) >= 2)
          return 2;
        break;

      case D64_TYPE_D82:
        if (image_read(part, D82_ERROR_OFFSET + sector_lba(part,track,0),
                       errorcache.errors, sectors_per_track(part, track)) >= 2)
          return 2;
        break;

      default:
        /* Should not happen unless someone enables error info
           for additional image formats */
        return 2;

      }
      errorcache.part  = part;
      errorcache.track = track;
    }

    /* Calculate error message from the code */
    if (errorcache.errors[sector] >= 2 && errorcache.errors[sector] <= 11) {
      /* Most codes can be mapped directly */
      set_error_ts(errorcache.errors[sector]-2+20,track,sector);
      return 2;
    }
    if (errorcache.errors[sector] == 15) {
      /* Drive not ready */
      set_error(74);
      return 2;
    }
    /* 1 is OK, unknown values are accepted too */
  }

  uart_flush();
  return image_read(part, sector_offset(part,track,sector)+offset, buf, len);
}

/**
 * checked_read - read a specified sector after range-checking
 * @part  : partition number
 * @track : track number to be read
 * @sector: sector number to be read
 * @buf   : pointer to where the data should be read to
 * @len   : number of bytes to be read
 * @error : error number to be flagged if the range check fails
 *
 * This function checks if the track and sector are within the
 * limits for the image format and calls image_read to read
 * the data if they are. Returns the result of image_read or
 * 2 if the range check failed.
 */
static uint8_t checked_read(uint8_t part, uint8_t track, uint8_t sector,
                            uint8_t *buf, uint16_t len, uint8_t error) {
  return checked_read_full(part, track, sector, 0, buf, len, error);
}

/**
 * update_timestamp - update timestamp of a directory entry
 * @buffer: pointer to the directory entry
 *
 * This function updates the time stamp of a directory entry
 * at the address pointed to by @buffer with the current
 * date/time if available or the default timestamp if not.
 */
static void update_timestamp(uint8_t *buffer) {
#ifdef HAVE_RTC
  struct tm time;

  read_rtc(&time);
  buffer[DIR_OFS_YEAR]   = time.tm_year % 100;
  buffer[DIR_OFS_MONTH]  = time.tm_mon + 1;
  buffer[DIR_OFS_DAY]    = time.tm_mday;
  buffer[DIR_OFS_HOUR]   = time.tm_hour;
  buffer[DIR_OFS_MINUTE] = time.tm_min;
#else
  buffer[DIR_OFS_YEAR]   = 82;
  buffer[DIR_OFS_MONTH]  = 8;
  buffer[DIR_OFS_DAY]    = 31;
  buffer[DIR_OFS_HOUR]   = 0;
  buffer[DIR_OFS_MINUTE] = 0;
#endif
}

/**
 * write_entry - write a single directory entry
 * @part : partition number
 * @dh   : pointer to d64dh pointing to the entry
 * @buf  : pointer to the buffer where the data should be read from
 * @flush: if true, data is flushed to disk immediately
 *
 * This function writes a single directory entry specified by @dh
 * from the buffer @buf.
 * Assumes that it is never called with an invalid track/sector.
 * Returns the same as image_write
 */
static uint8_t write_entry(uint8_t part, struct d64dh *dh, uint8_t *buf, uint8_t flush) {
  return image_write(part, sector_offset(part, dh->track, dh->sector) +
                           dh->entry * 32, buf, 32, flush);
}

/**
 * read_entry - read a single directory entry
 * @part: partition number
 * @dh  : pointer to d64dh pointing to the entry
 * @buf : pointer to the buffer where the data should be stored
 *
 * This function reads a single directory entry specified by dh
 * into the buffer buf which needs to be at least 32 bytes big.
 * Assumes that it is never called with an invalid track/sector.
 * Returns the same as image_read (0 success, 1 partial read, 2 failed)
 */
static uint8_t read_entry(uint8_t part, struct d64dh *dh, uint8_t *buf) {
  return image_read(part, sector_offset(part, dh->track, dh->sector) +
                          dh->entry * 32, buf, 32);
}

/**
 * strnsubst - substitute one character with another in a buffer
 * @buffer : pointer to the buffer
 * @len    : length of buffer
 * @oldchar: character to be replaced
 * @newchar: character to be used as a replacement
 *
 * This functions changes all occurences of oldchar in the first len
 * byte of buffer with newchar. Although it is named str*, it doesn't
 * care about zero bytes in any way.
 */
static void strnsubst(uint8_t *buffer, uint8_t len, uint8_t oldchar, uint8_t newchar) {
  uint8_t i=len-1;

  do {
    if (buffer[i] == oldchar)
      buffer[i] = newchar;
  } while (i--);
}

/* fill a sector with 0 0xff 0* to generate a new empty directory sector */
/* needs a 256 byte work area in *data */
static uint8_t clear_dir_sector(uint8_t part, uint8_t t, uint8_t s, uint8_t *data) {
  memset(data, 0, 256);

  data[1] = 0xff;
  return image_write(part, sector_offset(part, t, s), data, 256, 0);
}


/* ------------------------------------------------------------------------- */
/*  BAM buffer handling                                                      */
/* ------------------------------------------------------------------------- */

/**
 * bam_buffer_flush - write BAM buffer to disk
 * @buf: pointer to the BAM buffer
 *
 * This function writes the contents of the BAM buffer to the disk image.
 * Returns 0 if successful, != 0 otherwise.
 */
static uint8_t bam_buffer_flush(buffer_t *buf) {
  uint8_t res;

  if (buf->mustflush && buf->pvt.bam.part < max_part) {
    res = image_write(buf->pvt.bam.part,
                      sector_offset(buf->pvt.bam.part,
                                    buf->pvt.bam.track,
                                    buf->pvt.bam.sector),
                      buf->data, 256, 1);
    buf->mustflush = 0;

    return res;
  } else
    return 0;
}

/**
 * d64_bam_commit - write BAM buffers to disk
 *
 * This function is the exported interface to force the BAM buffer
 * contents to disk. Returns 0 if successful, != 0 otherwise.
 */
uint8_t d64_bam_commit(void) {
  uint8_t res = 0;

  if (bam_buffer)
    res |= bam_buffer->cleanup(bam_buffer);

  if (bam_buffer2)
    res |= bam_buffer2->cleanup(bam_buffer2);

  return 0;
}

/**
 * bam_buffer_alloc - allocates a buffer for the BAM
 * @buf: pointer to the BAM buffer pointer
 *
 * This function tries to allocate a buffer for holding the BAM,
 * using the pointer pointed to by @buf. Returns 0 if successful,
 * != 0 otherwise.
 */
static uint8_t bam_buffer_alloc(buffer_t **buf) {
  *buf = alloc_system_buffer();
  if (!*buf)
    return 1;

  (*buf)->secondary    = BUFFER_SYS_BAM;
  (*buf)->pvt.bam.part = 255;
  (*buf)->cleanup      = bam_buffer_flush;
  stick_buffer(*buf);

  return 0;
}

/**
 * bam_buffer_swap - swap the two BAM buffers
 *
 * Thus function swaps the pointers to the two BAM buffers
 */
static void bam_buffer_swap(void) {
  buffer_t *tmp = bam_buffer;
  bam_buffer    = bam_buffer2;
  bam_buffer2   = tmp;
}

/**
 * bam_buffer_match - checks if a BAM buffer contains a specific sector
 * @buf   : BAM buffer to check
 * @part  : partition
 * @track : track
 * @sector: sector
 *
 * This function checks if the BAM buffer @buf currently holds the data
 * for the given @part, @track and @sector. Returns 1 if it does,
 * 0 otherwise.
 */
static uint8_t bam_buffer_match(buffer_t *buf, uint8_t part,
                                uint8_t track, uint8_t sector) {
  if (buf->pvt.bam.part   == part  &&
      buf->pvt.bam.track  == track &&
      buf->pvt.bam.sector == sector)
    return 1;
  else
    return 0;
}

/**
 * move_bam_window - read correct BAM sector into window.
 * @part  : partition
 * @track : track number
 * @type  : type of pointer requested
 * @ptr   : pointer to track information in BAM sector
 *
 * This function reads the correct BAM sector into memory for the requested
 * track, flushing an existing BAM sector to disk if needed.  It also
 * calculates the correct pointer into the BAM sector for the appropriate
 * track.  Since the BAM contains both sector counts and sector allocation
 * bitmaps, type is used to signal which reference is desired.
 * This function may swap the BAM buffer pointers, after it returns
 * bam_buffer is always the buffer with the requested sector.
 * Returns 0 if successful, != 0 otherwise.
 */
static uint8_t move_bam_window(uint8_t part, uint8_t track, bamdata_t type, uint8_t **ptr) {
  uint8_t res;
  uint8_t t,s, pos;

  switch(partition[part].imagetype & D64_TYPE_MASK) {
  case D64_TYPE_D41:
  default:
    t   = D41_BAM_TRACK;
    s   = D41_BAM_SECTOR;
    pos = D41_BAM_BYTES_PER_TRACK * track + (type == BAM_BITFIELD ? 1:0);
    break;

  case D64_TYPE_D71:
    if (track > 35 && type == BAM_BITFIELD) {
      t   = D71_BAM2_TRACK;
      s   = D71_BAM2_SECTOR;
      pos = (track - 36) * D71_BAM2_BYTES_PER_TRACK;
    } else {
      t = D41_BAM_TRACK;
      s = D41_BAM_SECTOR;
      if (track > 35) {
        pos = (track - 36) + D71_BAM_COUNTER2OFFSET;
      } else {
        pos = D41_BAM_BYTES_PER_TRACK * track + (type == BAM_BITFIELD ? 1:0);
      }
    }
    break;

  case D64_TYPE_D81:
    t   = D81_BAM_TRACK;
    s   = (track < 41 ? D81_BAM_SECTOR1 : D81_BAM_SECTOR2);
    if (track > 40)
      track -= 40;
    pos = D81_BAM_OFFSET + track * D81_BAM_BYTES_PER_TRACK + (type == BAM_BITFIELD ? 1:0);
    break;

  case D64_TYPE_DNP:
    t   = DNP_BAM_TRACK;
    s   = DNP_BAM_SECTOR + (track >> 3);
    pos = (track & 0x07) * 32;
    break;

  case D64_TYPE_D80:
  case D64_TYPE_D82:
    t    = D80_BAM_TRACK;
    s    = D80_BAM_SECTOR;
    while (track > 50) {
      s += D80_BAM_INTERLEAVE;
      track -= 50;
    }
    pos = 1 + D80_BAM_BYTES_PER_TRACK * track + (type == BAM_BITFIELD ?  1:0);
    break;
  }

  if (!bam_buffer_match(bam_buffer, part, t, s)) {
    /* check if the second BAM buffer exists */
    if (bam_buffer2) {
      /* unconditionally swap to the other BAM buffer        */
      /* either it has the data or it was less-recently used */

      /* Note for future optimization:
       * If buf1 is invalid and buf2 valid but not matching this will swap and
       * use buf2 for the data instead of buf1 - using buf1
       * while keeping buf2 without swapping would be better.
       */
      bam_buffer_swap();

      /* check again, now in the other buffer */
      if (bam_buffer_match(bam_buffer, part, t, s))
        goto found;
    } else {
      /* allocate and swap to the second BAM buffer if the first one is valid */
      if (bam_buffer->pvt.bam.part != 255) {
        if (bam_buffer_alloc(&bam_buffer2)) {
          /* allocation failed, reset error and continue with just one buffer */
          set_error(ERROR_OK);
        } else {
          /* switch to the new buffer */
          bam_buffer_swap();
        }
      }
    }

    /* Need to read the BAM sector - flush only the target buffer */
    if (bam_buffer->cleanup(bam_buffer))
      return 1;

    res = image_read(part, sector_offset(part, t, s), bam_buffer->data, 256);
    if(res)
      return res;

    bam_buffer->pvt.bam.part   = part;
    bam_buffer->pvt.bam.track  = t;
    bam_buffer->pvt.bam.sector = s;
  }

 found:
  *ptr = bam_buffer->data + pos;
  return 0;
}

/**
 * is_free - checks if the given sector is marked as free
 * @part  : partition
 * @track : track number
 * @sector: sector number
 *
 * This function checks if the given sector is marked as free in
 * the BAM of drive "part". Returns 0 if allocated, >0 if free, <0 on error.
 */
static int8_t is_free(uint8_t part, uint8_t track, uint8_t sector) {
  uint8_t res;
  uint8_t *ptr = NULL;

  res = move_bam_window(part,track,BAM_BITFIELD,&ptr);
  if(res)
    return -1;

  if (partition[part].imagetype == D64_TYPE_DNP)
    return (ptr[sector>>3] & (0x80>>(sector&7))) != 0;
  else
    return (ptr[sector>>3] & (1<<(sector&7))) != 0;
}

/**
 * sectors_free - returns the number of free sectors on a given track
 * @part  : partition
 * @track : track number
 *
 * This function returns the number of free sectors on the given track
 * of partition part.
 */
static uint16_t sectors_free(uint8_t part, uint8_t track) {
  uint8_t *trackmap = NULL;

  if (track < 1 || track > get_param(part, LAST_TRACK))
    return 0;

  switch (partition[part].imagetype & D64_TYPE_MASK) {

  case D64_TYPE_DNP:
    if(move_bam_window(part,track,BAM_FREECOUNT,&trackmap))
      return 0;

    uint16_t blocks = 0;
    for (uint8_t i=0;i < DNP_BAM_BYTES_PER_TRACK;i++) {
      // From http://everything2.com/title/counting%25201%2520bits
      uint8_t b = (trackmap[i] & 0x55) + (trackmap[i]>>1 & 0x55);
      b = (b & 0x33) + (b >> 2 & 0x33);
      b = (b & 0x0f) + (b >> 4 & 0x0f);
      blocks += b;
    }
    return blocks;

  case D64_TYPE_D71:
  case D64_TYPE_D81:
  case D64_TYPE_D41:
  case D64_TYPE_D80:
  case D64_TYPE_D82:
  default:
    if(move_bam_window(part,track,BAM_FREECOUNT,&trackmap))
      return 0;
    return *trackmap;
  }
}

/**
 * allocate_sector - mark a sector as used
 * @part  : partitoin
 * @track : track number
 * @sector: sector number
 *
 * This function marks the given sector as used in the BAM of drive part.
 * If the sector was already marked as used nothing is changed.
 * Returns 0 if successful, 1 on error.
 */
static uint8_t allocate_sector(uint8_t part, uint8_t track, uint8_t sector) {
uart_puts_P(PSTR(">Alloc:")); //TODO:BZ:DELME
uart_puthex(track);
uart_putc(' ');
uart_puthex(sector);
uart_putcrlf();
uart_flush();
  uint8_t *trackmap;
  int8_t res = is_free(part,track,sector);

  if (res < 0)
    return 1;

  if (res != 0) {
    /* do the bitfield first, since is_free already set it up for us. */
    if(move_bam_window(part,track,BAM_BITFIELD,&trackmap))
      return 1;

    bam_buffer->mustflush = 1;

    if (partition[part].imagetype == D64_TYPE_DNP) {
      /* For some reason DNP has its bitfield reversed */
      trackmap[sector>>3] &= (uint8_t)~(0x80>>(sector&7));

      /* DNP has no counter in its BAM */
      return 0;
    }

    trackmap[sector>>3] &= (uint8_t)~(1<<(sector&7));

    if(move_bam_window(part,track,BAM_FREECOUNT,&trackmap))
      return 1;

    if (trackmap[0] > 0) {
      trackmap[0]--;
      bam_buffer->mustflush = 1;
    }
  }
  return 0;
}

/**
 * free_sector - mark a sector as free
 * @part  : partition
 * @track : track number
 * @sector: sector number
 *
 * This function marks the given sector as free in the BAM of partition
 * part. If the sector was already marked as free nothing is changed.
 * Returns 0 if successful, 1 on error.
 */
static uint8_t free_sector(uint8_t part, uint8_t track, uint8_t sector) {
  uint8_t *trackmap;
  int8_t res = is_free(part,track,sector);

  if (res < 0)
    return 1;

  if (res == 0) {
    /* do the bitfield first, since is_free already set it up for us. */
    if(move_bam_window(part,track,BAM_BITFIELD,&trackmap))
      return 1;

    bam_buffer->mustflush = 1;

    if (partition[part].imagetype == D64_TYPE_DNP) {
      /* For some reason DNP has its bitfield reversed */
      trackmap[sector>>3] |= 0x80>>(sector&7);

      /* DNP has no counter in its BAM */
      return 0;
    }

    trackmap[sector>>3] |= 1<<(sector&7);

    if(move_bam_window(part,track,BAM_FREECOUNT,&trackmap))
      return 1;

    if(trackmap[0] < sectors_per_track(part, track)) {
      trackmap[0]++;
      bam_buffer->mustflush = 1;
    }
  }
  return 0;
}

/**
 * get_first_sector - calculate the first sector for a new file
 * @part  : partition
 * @track : pointer to a variable holding the track
 * @sector: pointer to a variable holding the sector
 *
 * This function calculates the first sector to be allocated for a new
 * file. The algorithm is based on the description found at
 * http://ist.uwaterloo.ca/~schepers/formats/DISK.TXT
 * Returns 0 if successful or 1 if any error occured.
 *
 * This code will not skip track 53 of a D71 if there are any free
 * sectors on it - this behaviour is consistent with that of a 1571
 * with a revision 3.0 ROM.
 */
static uint8_t get_first_sector(uint8_t part, uint8_t *track, uint8_t *sector) {
  int8_t distance = 1;

  uart_puts_P(PSTR(">GetFirstSec\r\n")); //TODO:BZ:DELME
  /* DNP uses a simple "first free" allocation scheme, starting at track 2.  */
  /* It is not known if this is the same algorithm as used in the original   */
  /* CMD drives, but track 1 seems to be semi-reserved for directory sectors.*/
  if (partition[part].imagetype == D64_TYPE_DNP) {
    *track = 2;
    while (sectors_free(part, *track) == 0) {
      (*track)++;

      if (*track == get_param(part, LAST_TRACK) ||
          *track == 0) {
        /* Wrap to track 1 */
        *track = 1;
      }

      if (*track == 2)
        /* If we're at track 2 again there are no free sectors anywhere */
        return 1;
    }
  } else {
    /* Look for a track with free sectors close to the directory */
    while (distance < get_param(part, LAST_TRACK)) {
      if (sectors_free(part, get_param(part, DIR_TRACK)-distance))
        break;

      /* Invert sign */
      distance = -distance;

      /* Increase distance every second try */
      if (distance > 0)
        distance++;
    }

    if (distance == get_param(part, LAST_TRACK)) {
      if (current_error == ERROR_OK)
        set_error(ERROR_DISK_FULL);
      return 1;
    }

    *track = get_param(part, DIR_TRACK)-distance;
  }

  /* Search for the first free sector on this track */
  for (*sector = 0;*sector < sectors_per_track(part, *track); *sector += 1)
    if (is_free(part, *track, *sector) > 0)
      return 0;

  /* If we're here the BAM is invalid or couldn't be read */
  if (current_error == ERROR_OK)
    set_error(ERROR_DISK_FULL);
  return 1;
}

/**
 * get_next_sector - calculate the next sector for a file
 * @part  : partition
 * @track : pointer to a variable holding the track
 * @sector: pointer to a variable holding the sector
 *
 * This function calculates the next sector to be allocated for a file
 * based on the current sector in the variables pointed to by track/sector.
 * The algorithm is based on the description found at
 * http://ist.uwaterloo.ca/~schepers/formats/DISK.TXT
 * Returns 0 if successful or 1 if any error occured.
 */
static uint8_t get_next_sector(uint8_t part, uint8_t *track, uint8_t *sector) {
  if (partition[part].imagetype == D64_TYPE_DNP) {
    uint8_t newtrack = *track;

    /* Find a track with free sectors */
    while (sectors_free(part, newtrack) == 0) {
      newtrack++;

      if (newtrack == get_param(part, LAST_TRACK) ||
          newtrack == 0) {
        /* Wrap to track 1 */
        newtrack = 1;
      }

      if (newtrack == *track)
uart_puts_P(PSTR(">NextSec: FAIL\r\n")); //TODO:BZ:DELME
        /* Returned to the start track -> no free sectors anywhere */
        return 1;
    }

    uint8_t newsector;

    if (newtrack == *track) {
      /* Same track: start at the previous sector */
      newsector = *sector;
    } else {
      /* New track: start at sector 0 */
      newsector = 0;
    }

    while (!is_free(part, newtrack, newsector))
      newsector++; // will automatically wrap from 255->0

uart_puts_P(PSTR(">NextSec: ")); //TODO:BZ:DELME
uart_puthex(newtrack); //TODO:BZ:DELME
uart_putc(' '); //TODO:BZ:DELME
uart_puthex(newsector); //TODO:BZ:DELME
uart_putcrlf();
    *track = newtrack;
    *sector = newsector;

    return 0;
  }

  uint8_t interleave,tries;

  if (*track == get_param(part, DIR_TRACK)) {
    if (sectors_free(part, get_param(part, DIR_TRACK)) == 0) {
      if (current_error == ERROR_OK)
        set_error(ERROR_DISK_FULL);
      return 1;
    }
    interleave = get_param(part, DIR_INTERLEAVE);
  } else
    interleave = get_param(part, FILE_INTERLEAVE);

  /* Look for a track with free sectors */
  tries = 0;
  while (tries < 3 && !sectors_free(part, *track)) {
    /* No more space on current track, try another */
    if (*track < get_param(part, DIR_TRACK))
      *track -= 1;
    else {
      *track += 1;
      /* Skip track 53 on D71 images */
      if ((partition[part].imagetype & D64_TYPE_MASK) == D64_TYPE_D71 &&
          *track == D71_BAM2_TRACK)
        *track += 1;
    }

    if (*track < 1) {
      *track = get_param(part, DIR_TRACK) + 1;
      *sector = 0;
      tries++;
    }
    if (*track > get_param(part, LAST_TRACK)) {
      *track = get_param(part, DIR_TRACK) - 1;
      *sector = 0;
      tries++;
    }
  }

  if (tries == 3) {
    if (current_error == ERROR_OK)
      set_error(ERROR_DISK_FULL);
    uart_puts_P(PSTR(">NextSec: FAIL2\r\n")); //TODO:BZ:DELME
    return 1;
  }

  /* Look for a sector at interleave distance */
  *sector += interleave;
  if (*sector >= sectors_per_track(part, *track)) {
    *sector -= sectors_per_track(part, *track);
    if (*sector != 0)
      *sector -= 1;
  }

  /* Increase distance until an empty sector is found */
  tries = 99;
  while (is_free(part,*track,*sector) <= 0 && tries--) {
    *sector += 1;
    if (*sector >= sectors_per_track(part, *track))
      *sector = 0;
  }

  uart_puts_P(PSTR(">NextSec: ")); //TODO:BZ:DELME
  uart_puthex(*track); //TODO:BZ:DELME
  uart_putc(' '); //TODO:BZ:DELME
  uart_puthex(*sector); //TODO:BZ:DELME
  uart_putcrlf();

  if (tries)
    return 0;

  if (current_error == ERROR_OK)
    set_error(ERROR_DISK_FULL);
  return 1;
}

/**
 * nextdirentry - read the next dir entry to ops_scratch
 * @dh: directory handle
 *
 * This function reads the next directory entry from the disk
 * into ops_scratch. Returns 1 if an error occured, -1 if there
 * are no more directory entries and 0 if successful. This
 * function will return every entry, even deleted ones.
 */
static int8_t nextdirentry(dh_t *dh) {
  /* End of directory entries in this sector? */
  if (dh->dir.d64.entry == 8) {
    /* Read link pointer */
    if (checked_read(dh->part, dh->dir.d64.track, dh->dir.d64.sector, ops_scratch, 2, ERROR_ILLEGAL_TS_LINK))
      return 1;

    /* Final directory sector? */
    if (ops_scratch[0] == 0)
      return -1;

    dh->dir.d64.track  = ops_scratch[0];
    dh->dir.d64.sector = ops_scratch[1];
    dh->dir.d64.entry  = 0;
  }

  if (dh->dir.d64.track < 1 || dh->dir.d64.track > get_param(dh->part, LAST_TRACK) ||
      dh->dir.d64.sector >= sectors_per_track(dh->part, dh->dir.d64.track)) {
    set_error_ts(ERROR_ILLEGAL_TS_LINK,dh->dir.d64.track,dh->dir.d64.sector);
    return 1;
  }

  if (read_entry(dh->part, &dh->dir.d64, ops_scratch))
    return 1;

  dh->dir.d64.entry++;

  return 0;
}

/**
 * find_empty_entry - find an empty directory entry
 * @path: path of the directory
 * @dh: directory handle
 *
 * This funtion finds an empty entry in the directory, creating
 * a new directory sector if required.
 * Returns 0 if successful or 1 if not.
 */
static uint8_t find_empty_entry(path_t *path, dh_t *dh) {
  int8_t res;

  d64_opendir(dh, path);
  do {
    res = nextdirentry(dh);
    if (res > 0)
      return 1;
  } while (res == 0 && ops_scratch[DIR_OFS_FILE_TYPE] != 0);

  /* Allocate a new directory sector if no empty entries were found */
  if (res < 0) {
    uint8_t t,s;

    t = dh->dir.d64.track;
    s = dh->dir.d64.sector;

    if (get_next_sector(path->part, &dh->dir.d64.track, &dh->dir.d64.sector))
      return 1;

    /* Link the old sector to the new */
    ops_scratch[0] = dh->dir.d64.track;
    ops_scratch[1] = dh->dir.d64.sector;
    if (image_write(path->part, sector_offset(path->part,t,s), ops_scratch, 2, 0))
      return 1;

    if (allocate_sector(path->part, dh->dir.d64.track, dh->dir.d64.sector))
      return 1;

    /* DNP only: Increment the block count in the parent directory */
    if (partition[path->part].imagetype == D64_TYPE_DNP) {
      if (image_read(path->part,
                     DNP_DIRHEADER_PARENTENTRY_TRACK + sector_offset(path->part,
                                                                     path->dir.dxx.track,
                                                                     path->dir.dxx.sector),
                     ops_scratch, 3))
        return 1;

      if (ops_scratch[0] != 0) {
        /* Read block count of entry in parent directory */
        if (image_read(path->part,
                       sector_offset(path->part, ops_scratch[0], ops_scratch[1]) + ops_scratch[2] + DIR_OFS_SIZE_LOW - 2,
                       ops_scratch + 3, 2))
          return 1;

        uint16_t *blocks = (uint16_t *)(ops_scratch + 3);
        (*blocks)++;

        /* Write new block count */
        if (image_write(path->part,
                        sector_offset(path->part, ops_scratch[0], ops_scratch[1]) + ops_scratch[2] + DIR_OFS_SIZE_LOW - 2,
                        ops_scratch + 3, 2, 1))
          return 1;
      }
    }

    /* Clear the new directory sector */
    memset(ops_scratch, 0, 32);
    ops_scratch[1] = 0xff;
    for (uint8_t i=0;i<256/32;i++) {
      dh->dir.d64.entry = i;
      if (write_entry(path->part, &dh->dir.d64, ops_scratch, 0))
        return 1;

      ops_scratch[1] = 0;
    }

    /* Mark full sector as used */
    ops_scratch[1] = 0xff;
    dh->dir.d64.entry = 0;

  } else {
    /* nextdirentry has already incremented this variable, undo it */
    dh->dir.d64.entry--;
  }

  return 0;
}

/**
 * d64_read - refill-callback used for reading
 * @buf: target buffer
 *
 * This is the callback used as refill for files opened for reading.
 */
static uint8_t d64_read(buffer_t *buf) {
  /* Store the current sector, used for append */
  buf->pvt.d64.track  = buf->data[0];
  buf->pvt.d64.sector = buf->data[1];
  uart_puts_P(PSTR(">DRead: ")); //TODO:BZ:DELME
  uart_puthex(buf->data[0]); //TODO:BZ:DELME
  uart_putc(' '); //TODO:BZ:DELME
  uart_puthex(buf->data[1]); //TODO:BZ:DELME
  uart_putcrlf();

  if (checked_read(buf->pvt.d64.part, buf->data[0], buf->data[1], buf->data, 256, ERROR_ILLEGAL_TS_LINK)) {
    free_buffer(buf);
    return 1;
  }

  buf->position = 2;

  if (buf->data[0] == 0) {
    /* Final sector of the file */
    buf->lastused = buf->data[1];
    buf->sendeoi  = 1;
  } else {
    buf->lastused = 255;
    buf->sendeoi  = 0;
  }

  return 0;
}

static uint8_t d64_flush_buf(buffer_t *buf)
{
  if(buf->dirty)
  {
    uart_puts_P(PSTR(">Flush: ")); //TODO:BZ:DELME
    uart_puthex(buf->pvt.d64.track); //TODO:BZ:DELME
    uart_putc(' '); //TODO:BZ:DELME
    uart_puthex(buf->pvt.d64.sector); //TODO:BZ:DELME
    uart_putcrlf();
    if (image_write(buf->pvt.d64.part,
                    sector_offset(buf->pvt.d64.part,
                                  buf->pvt.d64.track,
                                  buf->pvt.d64.sector),
                                  buf->data, 256, 1)) {
      free_buffer(buf);
      return 1;
    }
    mark_buffer_clean(buf);
  }
  return 0;
}

/**
 * d64_seek - seek-callback
 * @buf     : target buffer
 * @position: offset to seek to
 * @index   : offset within the record to seek to
 *
 * This is the function used as the seek callback.
 */
static uint8_t d64_seek(buffer_t *buf, uint32_t position, uint8_t index) {
  // everything here is 0 based!
  const uint8_t use_sss = get_param(buf->pvt.d64.part, USE_SUPER_SIDE_SECTOR);
  uint8_t link_lookup[2];

  buf->read = 0;
  buf->rpos = position+index;
  if(d64_flush_buf(buf))
    return 1;
  // special case, because rpos == 0 == no seek done at ALL!
  if(buf->rpos == 0) {
    uart_puts_P(PSTR(">Seek0\n\r")); //TODO:BZ:DELME
    if (checked_read_full(buf->pvt.d64.part, buf->pvt.d64.dh.track, buf->pvt.d64.dh.sector, (buf->pvt.d64.dh.entry * 32) + DIR_OFS_TRACK,
                          link_lookup, 2, ERROR_RECORD_MISSING))
      return 1;
  }
  else
  {
    uint32_t sector_count = position / 254;
    uint8_t ssector_index = sector_count / 720;
    uint8_t sidesector_no = (use_sss ? (ssector_index % 720) : sector_count) / 120;
    uint8_t sidesector_pos = ((sector_count % 120) * 2) + 0x10;
    uart_puts_P(PSTR(">Seek In:")); //TODO:BZ:DELME
    uart_puthex32(position); //TODO:BZ:DELME
    uart_putc(' ');
    uart_puthex32(sector_count); //TODO:BZ:DELME
    uart_putc(' ');
    uart_puthex(ssector_index); //TODO:BZ:DELME
    uart_putc(' ');
    uart_puthex(sidesector_no); //TODO:BZ:DELME
    uart_putc(' ');
    uart_puthex(sidesector_pos); //TODO:BZ:DELME
    uart_putcrlf(); //TODO:BZ:DELME
    uart_flush();
    memset(buf->data,0,2);
    buf->data[2] = 13;
    buf->position=2;
    buf->lastused=2;
    if(sidesector_no > 5)
    {
      set_error(ERROR_RECORD_MISSING);
      return 1;
    }
    if (checked_read_full(buf->pvt.d64.part, buf->pvt.d64.dh.track, buf->pvt.d64.dh.sector, (buf->pvt.d64.dh.entry * 32) + DIR_OFS_SIDE_TRACK,
                          link_lookup, 2, ERROR_RECORD_MISSING))
      return 1;
    uart_puts_P(PSTR(">P2:")); //TODO:BZ:DELME
    uart_puthex(link_lookup[0]); //TODO:BZ:DELME
    uart_putc(' '); //TODO:BZ:DELME
    uart_puthex(link_lookup[1]); //TODO:BZ:DELME
    uart_putcrlf(); //TODO:BZ:DELME
    uart_flush();
    if(use_sss)
    {
        if (checked_read_full(buf->pvt.d64.part,link_lookup[0], link_lookup[1], ssector_index * 2,
                              link_lookup, 2, ERROR_RECORD_MISSING))
          return 1;
        if(!link_lookup[0])
        {
            set_error(ERROR_RECORD_MISSING);
            return 1;
        }
    }
    while(sidesector_no > 0) {
        if (checked_read_full(buf->pvt.d64.part, link_lookup[0], link_lookup[1], 0, link_lookup, 2, ERROR_RECORD_MISSING))
          return 1;
        sidesector_no--;
    }
    if (checked_read_full(buf->pvt.d64.part, link_lookup[0], link_lookup[1], sidesector_pos, link_lookup, 2, ERROR_RECORD_MISSING))
      return 1;
    if(!link_lookup[0])
    {
        set_error(ERROR_RECORD_MISSING);
        return 1;
    }
  }
  buf->data[0] = link_lookup[0];
  buf->data[1] = link_lookup[1];
  if(d64_read(buf))
    return 1;
  buf->rpos = 0;
  buf->mustflush = 0;
  uint8_t rec_pos = 2 + ((position+index) % 254);
  buf->position   = rec_pos;
  buf->read       = 1;
  uart_puts_P(PSTR(">Seek To:")); //TODO:BZ:DELME
  uart_puthex(buf->pvt.d64.track); //TODO:BZ:DELME
  uart_putc(' ');
  uart_puthex(buf->pvt.d64.sector); //TODO:BZ:DELME
  uart_putc(' ');
  uart_puthex(rec_pos); //TODO:BZ:DELME
  uart_putcrlf();
  uart_flush();
  if(buf->pvt.d64.blocks == 0)
  {
      set_error(ERROR_RECORD_MISSING);
      return 1;
  }
  if((buf->data[0] == 0) && (buf->data[1] < buf->position))
    buf->data[1] = buf->position;
  buf->recno= (position + index) / buf->recordlen;
  return 0; // it exists!
}

// 23408640 total bytes poss with SSS
// 182880 bytes per SS trail in the SSS
// 30480 bytes per SS node in a SS trail
// 6 side sectors per SSS
/**
 * d64_force_seek - forced seek
 * @buf     : target buffer
 * @rpos    : byte offset to seek to
 *
 * Do a forced seek, creating data sectors if needed in
 * order to get to the given position.  The butter_t buf
 * will be updated with the correct sector data, etc.
 */
static uint8_t d64_force_seek(buffer_t *buf, uint32_t rpos)
{
  uart_puts_P(PSTR(">ForSeek:")); //TODO:BZ:DELME
  uart_puthex32(rpos); //TODO:BZ:DELME
  uart_putc(' '); //TODO:BZ:DELME
  uart_puthex16((uint16_t)(rpos / buf->recordlen)); //TODO:BZ:DELME
  uart_putc(' '); //TODO:BZ:DELME
  uart_puthex((uint8_t)(rpos % buf->recordlen)); //TODO:BZ:DELME
  uart_putcrlf();
  uart_flush();
  const uint8_t use_sss = get_param(buf->pvt.d64.part, USE_SUPER_SIDE_SECTOR);
  const uint8_t part = buf->pvt.d64.part;
  uint8_t ss_ts[2];
  uint8_t sss_ts[2] = { 0, 0 };
  uint8_t sss_buf[256];
  uint8_t sss_dirty = FALSE;
  uint32_t skip_rpos = 0;
  if(skip_rpos >= rpos)
    return d64_seek(buf, rpos / buf->recordlen, rpos % buf->recordlen);
  if (checked_read_full(part, buf->pvt.d64.dh.track, buf->pvt.d64.dh.sector,
                        (buf->pvt.d64.dh.entry * 32) + DIR_OFS_SIDE_TRACK,
                        ss_ts, 2, ERROR_ILLEGAL_TS_LINK))
    return 1;
  int16_t sssdex = 0;
  if(use_sss)
  {
    memcpy(sss_ts,ss_ts,2);
    if (checked_read_full(part, sss_ts[0], sss_ts[1], 0,
                          sss_buf, 256, ERROR_ILLEGAL_TS_LINK))
      return 1;
    // have sss, find last data block ts, counting up along the
    // way.  If it exists, great.
    for(sssdex = 254;sssdex>=0;sssdex-=2)
    {
      if(sss_buf[sssdex] != 0)
      {
        skip_rpos += 182880 * (sssdex/2);
        memcpy(ss_ts,sss_buf+sssdex,2);
        break;
      }
    }
    if(sssdex<0)
    {
      set_error_ts(ERROR_ILLEGAL_TS_LINK,sss_ts[0], sss_ts[1]);
      return 1;
    }
  }
  if(skip_rpos >= rpos)
    return d64_seek(buf, rpos / buf->recordlen, rpos % buf->recordlen);
  // have the highest side sector start, now find last side sector
  uint8_t ss_trl[16];
  uint8_t ss_buf[256];
  uint8_t ss_dirty = FALSE;
  uint8_t dblk_ts[2] = { 0, 0 };
uart_puts_P(PSTR(">FS:")); //TODO:BZ:DELME
uart_puthex(ss_ts[0]); //TODO:BZ:DELME
uart_putc(' '); //TODO:BZ:DELME
uart_puthex(ss_ts[1]); //TODO:BZ:DELME
uart_putcrlf(); //TODO:BZ:DELME
uart_flush();
  if (checked_read_full(part, ss_ts[0], ss_ts[1], 0,
      ss_trl, 16, ERROR_ILLEGAL_TS_LINK))
    return 1;
uart_puts_P(PSTR(">FS:")); //TODO:BZ:DELME
uart_puthex(ss_trl[4]); //TODO:BZ:DELME
uart_putc(' '); //TODO:BZ:DELME
uart_puthex(ss_trl[5]); //TODO:BZ:DELME
uart_putcrlf(); //TODO:BZ:DELME
uart_flush();
  uint8_t trldex =0;
  for(trldex = 14;trldex>=4;trldex-=2)
  {
    if(ss_trl[trldex] != 0)
    {
      skip_rpos += 30480 * ((trldex-4)/2);
      memcpy(ss_ts,ss_trl+trldex,2);
      break;
    }
  }
  if(trldex<4)
  {
    set_error_ts(ERROR_ILLEGAL_TS_LINK,ss_ts[0], ss_ts[1]);
    return 1;
  }
  if(skip_rpos >= rpos)
    return d64_seek(buf, rpos / buf->recordlen, rpos % buf->recordlen);
  // have the last side sector, find the last data sector now
  if (checked_read_full(part, ss_ts[0], ss_ts[1], 0,
                        ss_buf, 256, ERROR_ILLEGAL_TS_LINK))
    return 1;
  uint16_t datdex =0;
  for(datdex = 254;datdex>=16;datdex-=2)
  {
    if(ss_buf[datdex] != 0)
    {
      skip_rpos += 254 * ((trldex-16)/2);
      memcpy(dblk_ts,ss_buf+datdex,2);
      break;
    }
  }
  if(datdex<16)
  {
    set_error_ts(ERROR_ILLEGAL_TS_LINK,ss_ts[0], ss_ts[1]);
    return 1;
  }
  if(skip_rpos >= rpos)
    return d64_seek(buf, rpos / buf->recordlen, rpos % buf->recordlen);
  // we are at the last actual sector in the rel file,
  // now we just keep adding sectors until we are where we need to be.
  uint32_t added_size = 0;
  uint8_t dat_buf[256];
  while(skip_rpos < rpos)
  {
    uint8_t nblk_ts[2] = { dblk_ts[0], dblk_ts[1] };
    if (get_next_sector(part,nblk_ts,nblk_ts+1))
      return 1;
    if (allocate_sector(part,*nblk_ts,*(nblk_ts+1)))
      return 1;
    datdex+=2;
    if(datdex >= 256)
    {
      datdex = 16; // this is where it will be in the new node
      trldex += 2;
      if((trldex >= 16) && (!use_sss))
      {
        set_error(ERROR_DISK_FULL); // is this correct for no more rel file record space?!
        return 1;
      }
      // time for new ss node
      uint8_t nod_ts[2] = { ss_ts[0], ss_ts[1] } ;
      if (get_next_sector(part,nod_ts,nod_ts+1))
        return 1;
      if (allocate_sector(part,nod_ts[0],nod_ts[1]))
        return 1;
      if(trldex < 16)
      {
        // update the current, outgoing ss, which is now ALWAYS dirty
        ss_buf[trldex] = nod_ts[0];
        ss_buf[trldex+1] = nod_ts[1];
        ss_buf[0] = nod_ts[0];
        ss_buf[1] = nod_ts[1];
        if(image_write(part, sector_offset(part, ss_ts[0], ss_ts[1]), ss_buf, 256, 1))
          return 1;
        // also update all other nodes with location of new node
        for(uint8_t td = 4;td<trldex-2;td+=2)
        {
          if(image_write(part, sector_offset(part, ss_buf[td], ss_buf[td+1]), ss_buf+4, 12, 1))
            return 1;
        }
      }
      else
      if(ss_dirty)
      {
        if(image_write(part, sector_offset(part, ss_ts[0], ss_ts[1]), ss_buf, 256, 1))
          return 1;
      }
      ss_dirty = FALSE;
      if(trldex >= 16) // FINALLY deal with super side sector -use_sss implied here!
      {
        sssdex += 2;
        if(sssdex >= 256)
        {
          set_error(ERROR_DISK_FULL); // is this correct for no more rel file record space?!
          return 1;
        }
        // we are going to use the previously allocated node, and go from there
        trldex = 4; // this will be the new trail index
        sss_buf[sssdex] = nod_ts[0];
        sss_buf[sssdex+1] = nod_ts[1];
        sss_dirty = TRUE;
      }
      ss_dirty = TRUE;
      memcpy(ss_ts, nod_ts, 2);
      memset(ss_buf+4, 0, 252);
      ss_buf[0] = 0;
      ss_buf[1] = 0x10; // point at first blk byte
      ss_buf[4] = ss_ts[0];
      ss_buf[5] = ss_ts[1];
      datdex = 0x10;
    }
    memcpy(ss_buf+datdex,nblk_ts,2);
    if((ss_buf[0] == 0) && (ss_buf[1] < datdex+1))
      ss_buf[1] = datdex+1;
    memset(dat_buf,0,256);
    dat_buf[1] = 0xff;
    uint8_t firstByte = 2 + (buf->recordlen - (uint8_t)(skip_rpos % buf->recordlen));
    uint8_t laststart = 0;
    for(uint16_t i=firstByte;i<256;i+=buf->recordlen)
    {
      laststart = i;
      dat_buf[i]=0xff;
    }
    if(laststart + buf->recordlen >= 256)
      dat_buf[1] = laststart;
    if(image_write(part, sector_offset(part, nblk_ts[0], nblk_ts[1]), dat_buf, 256, 1))
      return 1;
    // block initialized, accomplish the link
    if(image_write(part, sector_offset(part, dblk_ts[0], dblk_ts[1]), nblk_ts, 2, 1))
      return 1;
    // good job, you're done.
    ss_dirty = TRUE;
    added_size++;
    memcpy(dblk_ts,nblk_ts,2);
    skip_rpos += 254;
  }
  if(ss_dirty == TRUE)
  {
    if(image_write(part, sector_offset(part, ss_ts[0], ss_ts[1]), ss_buf, 256, 1))
      return 1;
  }
  if(sss_dirty == TRUE)
  {
    if(image_write(part, sector_offset(part, sss_ts[0], sss_ts[1]), sss_buf, 256, 1))
      return 1;
  }
  // ok, I *think* write cleanup actually takes care of the file size?
  return d64_seek(buf, rpos / buf->recordlen, rpos % buf->recordlen);
}

/**
 * d64_write - refill-callback used for writing
 * @buf: target buffer
 *
 * This is the callback used as refill for files opened for writing.
 */
static uint8_t d64_write(buffer_t *buf) {
  uint8_t t, s, savederror;

  savederror = 0;
  t = buf->pvt.d64.track;
  s = buf->pvt.d64.sector;
  uart_puts_P(PSTR(">D64Write:")); //TODO:BZ:DELME
  uart_puthex(t); //TODO:BZ:DELME
  uart_putc(' ');
  uart_puthex(s); //TODO:BZ:DELME
  uart_putcrlf();

  buf->pvt.d64.blocks++;

  /* Mark as last sector in case something below fails */
  buf->data[0] = 0;
  buf->data[1] = buf->lastused;

  /* Find another free sector */
  if (get_next_sector(buf->pvt.d64.part, &t, &s)) {
    t = 0;
    savederror = current_error;
    goto storedata;
  }

  buf->data[0] = t;
  buf->data[1] = s;

  /* Allocate it */
  if (allocate_sector(buf->pvt.d64.part, t, s)) {
    free_buffer(buf);
    return 1;
  }

 storedata:
  /* Store data in the already-reserved sector */
  if (image_write(buf->pvt.d64.part,
                  sector_offset(buf->pvt.d64.part,
                                buf->pvt.d64.track,
                                buf->pvt.d64.sector),
                  buf->data, 256, 1)) {
    free_buffer(buf);
    return 1;
  }

  buf->pvt.d64.track  = t;
  buf->pvt.d64.sector = s;
  buf->position  = 2;
  buf->lastused  = 1;
  buf->mustflush = 0;
  mark_buffer_clean(buf);

  if (savederror) {
    set_error(savederror);
    free_buffer(buf);
    return 1;
  } else
    return 0;
}

static uint8_t d64_write_cleanup(buffer_t *buf) {
  uint8_t t,s;
  uart_puts_P(PSTR(">DCleanup: ")); //TODO:BZ:DELME
  uart_puthex(buf->pvt.d64.track); //TODO:BZ:DELME
  uart_putc(' '); //TODO:BZ:DELME
  uart_puthex(buf->pvt.d64.sector); //TODO:BZ:DELME
  uart_putcrlf();
  uart_flush();

  if((buf->recordlen == 0)
  ||((buf->data[0] == 0) && (buf->data[1] < buf->lastused)))
  {
    buf->data[0] = 0;
    buf->data[1] = buf->lastused;
  }

  t = buf->pvt.d64.track;
  s = buf->pvt.d64.sector;
  buf->pvt.d64.blocks++;

  /* Track=0 means that there was an error somewhere earlier */
  if (t == 0)
    return 1;

  /* Store data */
  if (image_write(buf->pvt.d64.part, sector_offset(buf->pvt.d64.part,t,s), buf->data, 256, 1))
    return 1;

  /* Update directory entry */
  if (read_entry(buf->pvt.d64.part, &buf->pvt.d64.dh, ops_scratch))
    return 1;

  ops_scratch[DIR_OFS_FILE_TYPE] |= FLAG_SPLAT;
  ops_scratch[DIR_OFS_SIZE_LOW]   = buf->pvt.d64.blocks & 0xff;
  ops_scratch[DIR_OFS_SIZE_HI]    = buf->pvt.d64.blocks >> 8;
  update_timestamp(ops_scratch);

  if (write_entry(buf->pvt.d64.part, &buf->pvt.d64.dh, ops_scratch, 1))
    return 1;

  buf->cleanup = callback_dummy;
  free_buffer(buf);

  return 0;
}

/* ------------------------------------------------------------------------- */
/*  fileops-API                                                              */
/* ------------------------------------------------------------------------- */

uint8_t d64_mount(path_t *path, uint8_t *name) {
  uint8_t imagetype;
  uint8_t part = path->part;
  uint32_t fsize = partition[part].imagehandle.fsize;

  switch (fsize) {
  case 174848:
    imagetype = D64_TYPE_D41;
    memcpy_P(&partition[part].d64data, &d41param, sizeof(struct param_s));
    break;

  case 175531:
    imagetype = D64_TYPE_D41 | D64_HAS_ERRORINFO;
    memcpy_P(&partition[part].d64data, &d41param, sizeof(struct param_s));
    break;

  case 349696:
    imagetype = D64_TYPE_D71;
    memcpy_P(&partition[part].d64data, &d71param, sizeof(struct param_s));
    break;

  case 351062:
    imagetype = D64_TYPE_D71 | D64_HAS_ERRORINFO;
    memcpy_P(&partition[part].d64data, &d71param, sizeof(struct param_s));
    break;

  case 819200:
    imagetype = D64_TYPE_D81;
    memcpy_P(&partition[part].d64data, &d81param, sizeof(struct param_s));
    break;

  case 822400:
    imagetype = D64_TYPE_D81 | D64_HAS_ERRORINFO;
    memcpy_P(&partition[part].d64data, &d81param, sizeof(struct param_s));
    break;

  case 533248:
    imagetype = D64_TYPE_D80;
    memcpy_P(&partition[part].d64data, &d80param, sizeof(struct param_s));
    break;

  case 535331:
    imagetype = D64_TYPE_D80 | D64_HAS_ERRORINFO;
    memcpy_P(&partition[part].d64data, &d80param, sizeof(struct param_s));
    break;

  case 1066496:
    imagetype = D64_TYPE_D82;
    memcpy_P(&partition[part].d64data, &d82param, sizeof(struct param_s));
    break;

  case 1070662:
    imagetype = D64_TYPE_D82 | D64_HAS_ERRORINFO;
    memcpy_P(&partition[part].d64data, &d82param, sizeof(struct param_s));
    break;

  default:
    if ((fsize % (256*256L)) != 0) {
      set_error(ERROR_IMAGE_INVALID);
      return 1;
    }

    /* sanity check: ignore 40-track D64 images */
    if (fsize == 196608) {
      uint8_t *ptr = ustrrchr(name, '.');

      if (ptr[2] == '6' && ptr[3] == '4') {
        set_error(ERROR_IMAGE_INVALID);
        return 1;
      }
    }

    imagetype = D64_TYPE_DNP;
    memcpy_P(&partition[part].d64data, &dnpparam, sizeof(struct param_s));
    partition[part].d64data.last_track = fsize / (256*256L);
  }

  /* allocate the first BAM buffer if required */
  if (bam_buffer == NULL) {
    if (bam_buffer_alloc(&bam_buffer))
      return 1;
  }

  partition[part].imagetype = imagetype;
  path->dir.dxx.track  = get_param(part, DIR_TRACK);
  path->dir.dxx.sector = get_param(part, DIR_START_SECTOR);

  bam_refcount++;

  if (imagetype & D64_HAS_ERRORINFO)
    /* Invalidate error cache */
    errorcache.part = 255;

  return 0;
}

static uint8_t d64_opendir(dh_t *dh, path_t *path) {
  dh->part = path->part;
  dh->dir.d64.track  = path->dir.dxx.track;
  dh->dir.d64.sector = path->dir.dxx.sector;
  dh->dir.d64.entry  = 0;

  if (partition[path->part].imagetype == D64_TYPE_DNP) {
    /* Read the real first directory sector from the header sector */
    uint8_t tmp[2];
    if (image_read(path->part,
                   sector_offset(path->part, dh->dir.d64.track, dh->dir.d64.sector),
                   tmp, 2))
      return 1;

    dh->dir.d64.track  = tmp[0];
    dh->dir.d64.sector = tmp[1];
  }
  return 0;
}

static int8_t d64_readdir(dh_t *dh, cbmdirent_t *dent) {
  int8_t res;

  do {
    res = nextdirentry(dh);
    if (res)
      return res;

    if (ops_scratch[DIR_OFS_FILE_TYPE] != 0)
      break;
  } while (1);

  memset(dent, 0, sizeof(cbmdirent_t));

  dent->opstype = OPSTYPE_DXX;
  dent->typeflags = ops_scratch[DIR_OFS_FILE_TYPE] ^ FLAG_SPLAT;

  if ((dent->typeflags & TYPE_MASK) > TYPE_DIR)
    /* Change invalid types to DEL */
    /* FIXME: Should exclude DIR on non-DNP */
    dent->typeflags &= (uint8_t)~TYPE_MASK;

  dent->pvt.dxx.dh = dh->dir.d64;
  dent->pvt.dxx.dh.entry -= 1; /* undo increment in nextdirentry */
  dent->blocksize = ops_scratch[DIR_OFS_SIZE_LOW] + 256 * ops_scratch[DIR_OFS_SIZE_HI];
  dent->remainder = 0xff;
  memcpy(dent->name, ops_scratch + DIR_OFS_FILE_NAME, CBM_NAME_LENGTH);
  strnsubst(dent->name, 16, 0xa0, 0);

  /* sanitize values in case they were not stored */
  dent->date.minute=  ops_scratch[DIR_OFS_MINUTE] % 60;
  dent->date.hour  =  ops_scratch[DIR_OFS_HOUR] % 24;
  dent->date.day   = (ops_scratch[DIR_OFS_DAY]   - 1) % 31 + 1;
  dent->date.month = (ops_scratch[DIR_OFS_MONTH] - 1) % 12 + 1;
  dent->date.year  =  ops_scratch[DIR_OFS_YEAR] % 100;
  /* adjust year into 1980..2079 range */
  if (dent->date.year < 80)
    dent->date.year += 100;

  return 0;
}

/* Reads and converts a string from the dir header sector (BAM for D41/D71) to the buffer */
/* Used by d64_get(disk|dir)label and d64_getid */
static uint8_t read_string_from_dirheader(path_t *path, uint8_t *buffer, param_t what, uint8_t size) {
  uint8_t sector;

  if (partition[path->part].imagetype == D64_TYPE_DNP)
    sector = path->dir.dxx.sector;
  else
    sector = 0;

  if (image_read(path->part,
                 sector_offset(path->part, path->dir.dxx.track, sector)
                 + get_param(path->part, what),
                 buffer, size))
    return 1;

  strnsubst(buffer, size, 0xa0, 0x20);
  return 0;
}

static uint8_t d64_getdirlabel(path_t *path, uint8_t *label) {
  return read_string_from_dirheader(path, label, LABEL_OFFSET, 16);
}

static uint8_t d64_getdisklabel(uint8_t part, uint8_t *label) {
  if (partition[part].imagetype == D64_TYPE_DNP) {
    /* Read directly from root dir header */
    if (image_read(part, 256 + DNP_LABEL_OFFSET, label, 16))
      return 1;
  } else {
    /* Use getdirlabel instead */
    path_t curpath;

    curpath.part = part;
    curpath.dir = partition[part].current_dir;
    if (d64_getdirlabel(&curpath, label))
      return 1;
  }

  /* Zero-terminate label */
  uint8_t *ptr = label+16;
  *ptr-- = 0;
  while (ptr != label && *ptr == ' ')
    *ptr-- = 0;

  return 0;
}

static uint8_t d64_getid(path_t *path, uint8_t *id) {
  return read_string_from_dirheader(path, id, ID_OFFSET, 5);
}

static uint16_t d64_freeblocks(uint8_t part) {
  uart_puts_P(PSTR(">FreeBlocks\r\n")); //TODO:BZ:DELME
  uint16_t blocks = 0;
  uint8_t i;

  for (i = 1; i != 0 && i <= get_param(part, LAST_TRACK); i++) {
    /* Skip directory track */
    switch (partition[part].imagetype & D64_TYPE_MASK) {
    case D64_TYPE_D81:
      if (i == D81_BAM_TRACK)
        continue;
      break;

    case D64_TYPE_DNP:
      /* DNP reserves a partial track, handled below */
      break;

    case D64_TYPE_D41:
    case D64_TYPE_D71:
    default:
      if (i == D41_BAM_TRACK || i == D71_BAM2_TRACK)
        continue; // continue the for loop
      break;      // break out of the switch

    case D64_TYPE_D80:
    case D64_TYPE_D82:
      if (i == D80_DIR_TRACK)
        continue; // continue the for loop
      break;      // break out of the switch
    }

    if ((partition[part].imagetype & D64_TYPE_MASK)
        == D64_TYPE_DNP && i == 1) {
      /* DNP: ignore sectors 0-63 on track 1 */
      for (uint16_t j = 64; j < 256; j++) {
        if (is_free(part, 1, j) > 0)
          blocks++;
      }

    } else {
      blocks += sectors_free(part,i);
    }
  }

  return blocks;
}

static void d64_open_read(path_t *path, cbmdirent_t *dent, buffer_t *buf) {
  uart_puts_P(PSTR(">OpenRead\r\n")); //TODO:BZ:DELME
  /* Read the directory entry of the file */
  if (read_entry(path->part, &dent->pvt.dxx.dh, ops_scratch))
    return;

  buf->data[0] = ops_scratch[DIR_OFS_TRACK];
  buf->data[1] = ops_scratch[DIR_OFS_SECTOR];

  buf->pvt.d64.part = path->part;

  buf->read    = 1;
  buf->refill  = d64_read;
  buf->seek    = d64_seek;
  stick_buffer(buf);

  buf->refill(buf);
}

static int d64_mod_for_write(path_t *path, cbmdirent_t *dent, buffer_t *buf)
{
  uart_puts_P(PSTR(">ModForWrite\r\n")); //TODO:BZ:DELME
  buf->pvt.d64.dh     = dent->pvt.dxx.dh;
  buf->pvt.d64.blocks = ops_scratch[DIR_OFS_SIZE_LOW] + 256 * ops_scratch[DIR_OFS_SIZE_HI]-1;
  buf->read       = 0;
  buf->position   = buf->lastused+1;
  if (buf->position == 0)
    buf->mustflush = 1;
  else
    buf->mustflush = 0;
  buf->refill     = d64_write;
  buf->cleanup    = d64_write_cleanup;
  buf->seek       = d64_seek;
  mark_write_buffer(buf);

  update_timestamp(ops_scratch);
  write_entry(buf->pvt.d64.part, &buf->pvt.d64.dh, ops_scratch, 1);

  return 0;
}

static int d64_open_append(path_t *path, cbmdirent_t *dent, buffer_t *buf)
{
  uart_puts_P(PSTR(">OpenAppend\r\n")); //TODO:BZ:DELME
  /* Append case: Open the file and read the last sector */
  d64_open_read(path, dent, buf);
  while (!current_error && buf->data[0])
    buf->refill(buf);

  if (current_error)
    return 1;

  /* Modify the buffer for writing */
  return d64_mod_for_write(path, dent, buf);
}

static int d64_open_create(path_t *path, cbmdirent_t *dent, uint8_t type, buffer_t *buf, uint8_t recordlen)
{
  uart_puts_P(PSTR(">OpenCreate\r\n")); //TODO:BZ:DELME
  dh_t dh;
  uint8_t *ptr;

  /* Non-append case */
  if((type == TYPE_REL) && (recordlen == 0))
  {
    set_error(ERROR_NO_CHANNEL);
    return 1;
  }

  /* Search for an empty directory entry */
  if (find_empty_entry(path, &dh))
    return 1;

  /* Create directory entry in ops_scratch */
  uint8_t *name = dent->name;
  memset(ops_scratch + 2, 0, sizeof(ops_scratch) - 2);  /* Don't overwrite the link pointer! */
  memset(ops_scratch + DIR_OFS_FILE_NAME, 0xa0, CBM_NAME_LENGTH);
  ptr = ops_scratch + DIR_OFS_FILE_NAME;
  while (*name) *ptr++ = *name++;
  ops_scratch[DIR_OFS_FILE_TYPE] = type;

  /* Find a free sector and allocate it */
  uint8_t t,s;

  if (get_first_sector(path->part,&t,&s))
    return 1;

  ops_scratch[DIR_OFS_TRACK]  = t;
  ops_scratch[DIR_OFS_SECTOR] = s;

  if (allocate_sector(path->part,t,s))
    return 1;

  // finish up the directory entry
  update_timestamp(ops_scratch);
  if(type == TYPE_REL)
  {
    /* Find a free side sector and allocate it */
    uint8_t st,ss;
    if (get_first_sector(path->part,&st,&ss))
      return 1;

    if (allocate_sector(path->part,st,ss))
      return 1;

    uint8_t data[256];
    const uint8_t use_sss = get_param(path->part, USE_SUPER_SIDE_SECTOR);
    ops_scratch[DIR_OFS_SIDE_TRACK]  = st;
    ops_scratch[DIR_OFS_SIDE_SECTOR] = ss;
    if(use_sss)
    {
      uint8_t sst=st;
      uint8_t sss=ss;
      if (get_next_sector(path->part,&sst,&sss))
        return 1;

      if (allocate_sector(path->part,sst,sss))
        return 1;

      memset(data,0,256);
      data[0] = st;
      data[1] = ss;
      if(image_write(path->part, sector_offset(path->part, sst, sss), data, 256, 1))
        return 1;

      ops_scratch[DIR_OFS_SIDE_TRACK]  = sst;
      ops_scratch[DIR_OFS_SIDE_SECTOR] = sss;
    }

    ops_scratch[DIR_OFS_RECORD_LEN] = recordlen;

    // write first side sector;
    memset(data,0,256);
    data[1] = 0x11;
    data[3] = recordlen;
    data[4] = st;
    data[5] = ss;
    data[0x10] = t;
    data[0x11] = s;
    if(image_write(path->part, sector_offset(path->part, st, ss), data, 256, 1))
      return 1;
  }

  /* Write the directory entry */
  if (write_entry(path->part, &dh.dir.d64, ops_scratch, 1))
    return 1;

  /* Prepare the data buffer */
  mark_write_buffer(buf);
  buf->position       = 2;
  buf->lastused       = 2;
  buf->cleanup        = d64_write_cleanup;
  buf->refill         = d64_write;
  buf->seek           = d64_seek;
  buf->data[2]        = 13; /* Verified on VICE */
  buf->pvt.d64.dh     = dh.dir.d64;
  buf->pvt.d64.part   = path->part;
  buf->pvt.d64.track  = t;
  buf->pvt.d64.sector = s;
  buf->recordlen = 0;
  if(type == TYPE_REL)
  {
    uint8_t lastb=2;
    memset(buf->data,0,256);
    for(uint16_t i=2;i<256;i+=recordlen)
    {
      lastb=i;
      buf->data[i] = 0xff;
    }
    buf->data[1] = lastb + recordlen - 1;
    buf->pvt.d64.blocks++;
    buf->recordlen = recordlen;
    //buf->mustflush = 1;
    //mark_buffer_dirty(buf); // this just didn't work
    if(image_write(path->part, sector_offset(path->part, t, s), buf->data, 256, 1))
      return 1;
  }
  return 0;
}

static void d64_open_write(path_t *path, cbmdirent_t *dent, uint8_t type, buffer_t *buf, uint8_t append) {
  uart_puts_P(PSTR(">OpenWrite\r\n")); //TODO:BZ:DELME
  /* Check for read-only image file */
  if (!(partition[path->part].imagehandle.flag & FA_WRITE)) {
    set_error(ERROR_WRITE_PROTECT);
    return;
  }

  if (append) {
    d64_open_append(path, dent, buf);
    return;
  }
  d64_open_create(path, dent, type, buf, -1);
}

static uint8_t d64_alloc_in_ss(buffer_t *buf, uint8_t t, uint8_t s)
{
  uart_puts_P(PSTR(">AllocSS:")); //TODO:BZ:DELME
  uart_puthex(t); //TODO:BZ:DELME
  uart_putc(' ');
  uart_puthex(s); //TODO:BZ:DELME
  uart_putcrlf();
  uart_flush();
  const uint8_t use_sss = get_param(buf->pvt.d64.part, USE_SUPER_SIDE_SECTOR);
  const uint8_t part = buf->pvt.d64.part;
  uint8_t sec_buf[256];
  if (checked_read_full(part, buf->pvt.d64.dh.track, buf->pvt.d64.dh.sector,
                        (buf->pvt.d64.dh.entry * 32) + DIR_OFS_SIDE_TRACK,
                        sec_buf, 2, ERROR_ILLEGAL_TS_LINK))
    return 1;
  uint16_t sssdex=0;
  uint8_t ssssec[2] = {sec_buf[0], sec_buf[1] };
  if(use_sss)
  {
    // 23408640 total bytes poss with SSS
    // 182880 bytes per SS trail in the SSS
    // 30480 bytes per block in a single SS
    // 6 side sectors per SSS
    if (checked_read_full(part, sec_buf[0], sec_buf[1], 0,
                          sec_buf, 256, ERROR_ILLEGAL_TS_LINK))
      return 1;
    for(;sssdex<256;sssdex+=2)
      if(sec_buf[sssdex] == 0)
        break;
    sec_buf[0] = sec_buf[sssdex-2];
    sec_buf[1] = sec_buf[sssdex-1];
  }
  // get first side sector to find the last one
  if (checked_read_full(part, sec_buf[0], sec_buf[1], 0,
                        sec_buf, 16, ERROR_ILLEGAL_TS_LINK))
    return 1;
  uint8_t ssec[2] = {sec_buf[14], sec_buf[15] };
  uint8_t ss_last_dex=0;
  for(ss_last_dex=14;ss_last_dex>=4;ss_last_dex-=2)
    if(sec_buf[ss_last_dex] != 0)
    {
      ssec[0]=sec_buf[ss_last_dex];
      ssec[1]=sec_buf[ss_last_dex+1];
      break;
    }
  if(ss_last_dex<4)
  {
    set_error_ts(ERROR_ILLEGAL_TS_LINK,sec_buf[0], sec_buf[1]);
    return 1;
  }
  // now we have the last side sector.  look for an opening...
  if (checked_read_full(part, ssec[0], ssec[1], 0,
                        sec_buf, 256, ERROR_ILLEGAL_TS_LINK))
    return 1;
  for(int idx = 16;idx < 256;idx += 2)
  {
    if(sec_buf[idx] == 0) {
      sec_buf[idx] = t;
      sec_buf[idx+1] = s;
      if(sec_buf[1]<idx+1)
        sec_buf[1] = (uint8_t)(idx+1);
      if (image_write(part, sector_offset(part, ssec[0], ssec[1]),
                      sec_buf, 256, 0))
        return 1;
      return 0;
    }
  }
  if(ss_last_dex == 5)
  {
    if(use_sss)
    {
      if (checked_read_full(part, ssssec[0], ssssec[1], 0,
                            sec_buf, 256, ERROR_ILLEGAL_TS_LINK))
        return 1;
      for(;sssdex<256;sssdex+=2)
        if(sec_buf[sssdex] == 0)
        {
          uint8_t st=ssssec[0];
          uint8_t ss=ssssec[1];
          if (get_next_sector(part,&st,&ss))
            return 1;
          if (allocate_sector(part,st,ss))
            return 1;
          buf->pvt.d64.blocks++;
          sec_buf[sssdex]=st;
          sec_buf[sssdex+1]=ss;
          if(image_write(part, sector_offset(part, ssssec[0], ssssec[1]), sec_buf, 256, 1))
            return 1;
          // write first side sector in this chain;
          memset(sec_buf,0,256);
          sec_buf[1] = 0x11;
          sec_buf[3] = buf->recordlen;
          sec_buf[4] = st;
          sec_buf[5] = ss;
          sec_buf[0x10] = t;
          sec_buf[0x11] = s;
          if(image_write(part, sector_offset(part, st, ss), sec_buf, 256, 1))
            return 1;
          return 0; // all done!
        }
    }
    set_error(ERROR_DISK_FULL); // is this correct for no more rel file record space?!
    return 1;
  }
  else
  {
    uint8_t st = ssec[0];
    uint8_t ss = ssec[1];
    if (get_next_sector(part,&st,&ss))
      return 1;
    if (allocate_sector(part,st,ss))
      return 1;
    buf->pvt.d64.blocks++;
    // its weird, but first step is to update the sec we have buffered
    memset(sec_buf,16,240);
    const uint8_t ss_offset = 4+(ss_last_dex*2)+2;
    sec_buf[ss_offset]=st; // add the new side sector link
    sec_buf[ss_offset+1]=ss;
    if(image_write(part, sector_offset(part, ssec[0], ssec[1]), sec_buf, 256, 1))
      return 1;
    // NOW we can write the new side sector!
    sec_buf[0] = 0;
    sec_buf[1] = 0x11;
    sec_buf[2] = ss_last_dex+1;
    sec_buf[0x10] = t;
    sec_buf[0x11] = s;
    if(image_write(part, sector_offset(part, st, ss), sec_buf, 256, 1))
      return 1;
    // now all that's left is updating every single SS that came before, starting with 0
    for(uint8_t prev_ss_dex=0;prev_ss_dex<ss_last_dex;prev_ss_dex++)
    {
      const uint8_t fst = sec_buf[4+(prev_ss_dex*2)];
      const uint8_t fss = sec_buf[4+(prev_ss_dex*2)+1];
      if(image_write(part, sector_offset(part, fst, fss)+ss_offset, sec_buf+ss_offset, 2, 1))
        return 1;
    }
  }
  return 0;
}

static uint8_t d64_readwrite(buffer_t *buf)
{
  uart_puts_P(PSTR(">RdWrCallback:")); //TODO:BZ:DELME
  uart_puthex32(buf->rpos); //TODO:BZ:DELME
  uart_putcrlf();
  uart_flush();
  if(buf->rpos) // this is a special case FORCING a record position
  {
    if(d64_flush_buf(buf))
      return 1;
    if(d64_force_seek(buf,buf->rpos))
      return 1;
    buf->rpos = 0; // clear the rpos flag
    uart_flush();
    return 0; // nothing left to do?
  }
  // 1. reading and ran out of buffer
  if(!buf->dirty && (buf->mustflush == 0)) // this is the read condition
  {
    if(d64_flush_buf(buf))
      return 1;

    d64_read(buf);  // move to the next readable sector from the current one
    mark_write_buffer(buf);
    uart_flush();
    return 0;
  }
  uint32_t startBytePos = ((uint32_t)buf->recno) * (uint32_t)buf->recordlen;
  uint32_t nextBytePos  = startBytePos + (uint32_t)buf->recordlen;
  // 2. writing and ran out of buffer
  if(buf->mustflush == 1) { // still reading/writing to same record, just new sector
    // the new sector already exists, so just read it up and continue
    if(buf->data[0] > 0) {
      if(d64_flush_buf(buf))
        return 1;
      d64_read(buf);  // move to the next readable sector from the current one
      mark_write_buffer(buf);
      uart_flush();
      return 0;
    }
    // the new sector doesn't exist, so create it, and move along
    if(d64_write(buf))
      return 1;
    if(d64_alloc_in_ss(buf,buf->pvt.d64.track, buf->pvt.d64.sector))
      return 1;
    memset(buf->data,0,256);
    uint8_t firstByte = 2 + (nextBytePos % 254);
    for(uint16_t i=firstByte;i<256;i+=buf->recordlen)
      buf->data[i]=0xff;
    buf->recno++;
    uart_flush();
    return 0;
  }
  // 3. writing and reached and-of-record, so...
  uint32_t ssno = startBytePos / 254;
  uint32_t esno = nextBytePos / 254;
  if(ssno == esno) {
    buf->recno++;
    buf->position = 2 + (nextBytePos % 254);
    uart_flush();
    return 0;
  }
  if(buf->data[0] != 0) {
    if(d64_read(buf))
      return 1;
    buf->recno++;
    buf->position = 2 + (nextBytePos % 254);
    uart_flush();
    return 0;
  }
  if(d64_write(buf))
    return 1;
  if(d64_alloc_in_ss(buf,buf->pvt.d64.track, buf->pvt.d64.sector))
    return 1;
  memset(buf->data,0,256);
  uint8_t firstByte = 2 + (nextBytePos % 254);
  for(uint16_t i=firstByte;i<256;i+=buf->recordlen)
    buf->data[i]=0xff;
  buf->recno++;
  buf->position = 2 + (nextBytePos % 254);
  uart_flush();
  return 0;
}

static void d64_open_rel(path_t *path, cbmdirent_t *dent, buffer_t *buf, uint8_t length, uint8_t mode) {
  uart_puts_P(PSTR(">OpenRel:")); //TODO:BZ:DELME
  uart_puthex(length);
  uart_putc(' ');
  uart_puthex(mode);
  uart_putcrlf(); //TODO:BZ:DELME
  uart_flush();
  // path: part, dxx.track, dxx.sector- of First dir entry
  // dent: name, blocksize, remainder, pvt.dxx.dh: track, sector, entry# of dile
  // NOTE: 70 no channel 0 0 happens on mismatch of recordlen, but only if you "p"
  // NOTE: 50 record not present happens if you P to a record not there yet
  if(mode == OPEN_MODIFY)
  {
    // the file already exists
    d64_open_read(path, dent, buf);
    if(d64_mod_for_write(path, dent, buf))
    {
      set_error(ERROR_NO_CHANNEL);
      return;
    }
    buf->read      = 1;
    buf->position  = 2; // always starts at beginning when reading
    mark_buffer_dirty(buf);
    //compare length to record length, return error if mismatch? didnt see that behavior in VICE
  }
  else
  {
    if((length == 0)||(length>255))
      set_error(ERROR_NO_CHANNEL);
    else
      d64_open_create(path, dent, TYPE_REL, buf, length);
  }
  buf->rpos       = 0; // signal that no POS command has been given
  buf->recordlen  = length;
  buf->recno      = 0;
  buf->refill     = d64_readwrite;
  buf->cleanup    = d64_write_cleanup;
  buf->seek       = d64_seek;
}

static uint8_t d64_delete(path_t *path, cbmdirent_t *dent) {
  uint8_t linkbuf[2];

  /* Read the directory entry of the file */
  if (read_entry(path->part, &dent->pvt.dxx.dh, ops_scratch))
    return 255;

  /* Free the sector chain in the BAM */
  linkbuf[0] = ops_scratch[DIR_OFS_TRACK];
  linkbuf[1] = ops_scratch[DIR_OFS_SECTOR];

  do {
    free_sector(path->part, linkbuf[0], linkbuf[1]);

    if (checked_read(path->part, linkbuf[0], linkbuf[1], linkbuf, 2, ERROR_ILLEGAL_TS_LINK))
      return 255;
  } while (linkbuf[0]);

  if(ops_scratch[DIR_OFS_FILE_TYPE] == TYPE_REL)
  {
    linkbuf[0] = ops_scratch[DIR_OFS_SIDE_TRACK];
    linkbuf[1] = ops_scratch[DIR_OFS_SIDE_SECTOR];
    do {
      free_sector(path->part, linkbuf[0], linkbuf[1]);

      if (checked_read(path->part, linkbuf[0], linkbuf[1], linkbuf, 2, ERROR_ILLEGAL_TS_LINK))
        return 255;
    } while (linkbuf[0]);
  }

  /* Clear directory entry */
  ops_scratch[DIR_OFS_FILE_TYPE] = 0;
  if (write_entry(path->part, &dent->pvt.dxx.dh, ops_scratch, 1))
    return 255;

  return 1;
}

static void d64_read_sector(buffer_t *buf, uint8_t part, uint8_t track, uint8_t sector) {
  uart_puts_P(PSTR(">D64ReadSec\r\n")); //TODO:BZ:DELME
  checked_read(part, track, sector, buf->data, 256, ERROR_ILLEGAL_TS_COMMAND);
}

static void d64_write_sector(buffer_t *buf, uint8_t part, uint8_t track, uint8_t sector) {
  uart_puts_P(PSTR(">D64WriteSec\r\n")); //TODO:BZ:DELME
  if (track < 1 || track > get_param(part, LAST_TRACK) ||
      sector >= sectors_per_track(part, track)) {
    set_error_ts(ERROR_ILLEGAL_TS_COMMAND,track,sector);
  } else
    image_write(part, sector_offset(part,track,sector), buf->data, 256, 1);
}

static void d64_rename(path_t *path, cbmdirent_t *dent, uint8_t *newname) {
  uint8_t *ptr;

  /* Read the directory entry of the file */
  if (read_entry(path->part, &dent->pvt.dxx.dh, ops_scratch))
    return;

  memset(ops_scratch + DIR_OFS_FILE_NAME, 0xa0, CBM_NAME_LENGTH);
  ptr = ops_scratch + DIR_OFS_FILE_NAME;
  while (*newname) *ptr++ = *newname++;

  write_entry(path->part, &dent->pvt.dxx.dh, ops_scratch, 1);
}

/**
 * d64_raw_directory - open directory track as file
 * @buf: target buffer
 *
 * This function opens the directory track as a file on the
 * buffer passed in buf. Used when $ is opened on a secondary
 * address > 0.
 */
void d64_raw_directory(path_t *path, buffer_t *buf) {
  /* Copy&Waste from d64_open_read */
  buf->data[0] = path->dir.dxx.track;
  if (partition[path->part].imagetype == D64_TYPE_DNP)
    buf->data[1] = path->dir.dxx.sector;
  else
    buf->data[1] = 0;

  buf->pvt.d64.part = path->part;

  buf->read    = 1;
  buf->refill  = d64_read;
  buf->seek    = d64_seek;
  stick_buffer(buf);

  buf->refill(buf);
}

/**
 * d64_chdir - chdir for Dxx files
 * @path   : path object of the location of dirname
 * @dirname: directory to be changed into
 *
 * Changes the directory in the path object for DNP files,
 * returns an error for everything else.
 * Returns 0 if successful, 1 otherwise.
 */
static uint8_t d64_chdir(path_t *path, cbmdirent_t *dirname) {
  uart_puts_P(PSTR(">ChDir\r\n")); //TODO:BZ:DELME
  if (partition[path->part].imagetype != D64_TYPE_DNP)
    return image_chdir(path,dirname);

  if (dirname->name[0] == 0) {
    /* Empty string: root directory */
    path->dir.dxx.track  = 1;
    path->dir.dxx.sector = 1;
    return 0;
  }

  if (dirname->name[0] == '_' && dirname->name[1] == 0) {
    /* Move up a directory, unmount if at the root */
    uint8_t parent[2];

    if (image_read(path->part,
                   sector_offset(path->part, path->dir.dxx.track, path->dir.dxx.sector) + DNP_DIRHEADER_PARENTHDR_TRACK,
                   parent, 2))
      return 1;

    if (parent[0] == 0)
      /* Already at the root directory */
      return image_unmount(path->part);

    path->dir.dxx.track  = parent[0];
    path->dir.dxx.sector = parent[1];
    return 0;
  }

  if (read_entry(path->part, &dirname->pvt.dxx.dh, ops_scratch))
    return 1;

  path->dir.dxx.track  = ops_scratch[DIR_OFS_TRACK];
  path->dir.dxx.sector = ops_scratch[DIR_OFS_SECTOR];

  return 0;
}

/**
 * d64_mkdir - mkdir for Dxx files
 * @path   : path object of the location of dirname
 * @dirname: directory to be created
 *
 * Creates a subdirectory for DNP files, returns an error
 * for everything else.
 */
static void d64_mkdir(path_t *path, uint8_t *dirname) {
  dh_t dh;
  buffer_t *buf;
  uint8_t h_track,h_sector,d_track,d_sector;
  uint8_t *ptr, *name;

  if (partition[path->part].imagetype != D64_TYPE_DNP) {
    set_error(ERROR_SYNTAX_UNABLE);
    return;
  }

  buf = alloc_buffer();
  if (buf == NULL)
    return;

  /* Find an empty directory entry */
  if (find_empty_entry(path, &dh))
    return;

  /* Find a free track/sector for the dir header */
  h_track  = path->dir.dxx.track;
  h_sector = path->dir.dxx.sector;

  if (get_next_sector(path->part, &h_track, &h_sector)) {
    set_error(ERROR_DISK_FULL);
    return;
  }

  /* Allocate it now so the next call to get_next doesn't return it again */
  if (allocate_sector(path->part, h_track, h_sector))
    return;

  /* Find a free track/sector for the first directory sector */
  d_track  = h_track;
  d_sector = h_sector;

  if (get_next_sector(path->part, &d_track, &d_sector)) {
    set_error(ERROR_DISK_FULL);
    free_sector(path->part, h_track, h_sector);
    return;
  }

  if (allocate_sector(path->part, d_track, d_sector))
    return;

  /* Build a directory header */
  memset(buf->data, 0, 256);
  memset(buf->data + DNP_LABEL_OFFSET, 0xa0, DNP_LABEL_AREA_SIZE);

  /* Unfortunately this generates much smaller code on AVR */
  ptr = buf->data;
  *ptr++ = d_track;
  *ptr++ = d_sector;
  *ptr++ = 'H';

  /* Note: The CMD FD manual says these bytes point to the root header, */
  /*       but all observed DNP files actually store the t/s of the     */
  /*       sector itself here.                                          */
  ptr = buf->data + DNP_DIRHEADER_ROOTHDR_TRACK;
  *ptr++ = h_track;
  *ptr++ = h_sector;

  *ptr++ = path->dir.dxx.track;
  *ptr++ = path->dir.dxx.sector;

  *ptr++ = dh.dir.d64.track;
  *ptr++ = dh.dir.d64.sector;
  *ptr++ = dh.dir.d64.entry * 32 + 2;

  /* Fill name and ID */
  ptr = buf->data + DNP_LABEL_OFFSET;
  name = dirname;
  while (*name) *ptr++ = *name++;

  ptr = buf->data + DNP_ID_OFFSET;
  *ptr++ = dirname[0];
  *ptr++ = dirname[1];
   ptr++;
  *ptr++ = '1';
  *ptr++ = 'H';

  /* Write directory header sector */
  if (image_write(path->part,
                  sector_offset(path->part, h_track, h_sector),
                  buf->data, 256, 0))
    return;

  /* Create empty directory sector */
  memset(buf->data, 0, 256);
  buf->data[1] = 0xff;
  if (image_write(path->part,
                  sector_offset(path->part, d_track, d_sector),
                  buf->data, 256, 0))
    return;

  /* Create the directory entry */
  /* FIXME: Isn't similiar code duplicated in open_write and rename? */
  memset(ops_scratch + 2, 0, sizeof(ops_scratch) - 2);
  memset(ops_scratch + DIR_OFS_FILE_NAME, 0xa0, CBM_NAME_LENGTH);

  ptr = ops_scratch + DIR_OFS_FILE_NAME;
  while (*dirname) *ptr++ = *dirname++;

  ops_scratch[DIR_OFS_FILE_TYPE] = TYPE_DIR | FLAG_SPLAT;
  ops_scratch[DIR_OFS_TRACK]     = h_track;
  ops_scratch[DIR_OFS_SECTOR]    = h_sector;
  ops_scratch[DIR_OFS_SIZE_LOW]  = 2;
  update_timestamp(ops_scratch);

  image_write(path->part, sector_offset(path->part, dh.dir.d64.track, dh.dir.d64.sector)
                          + dh.dir.d64.entry * 32 + 2, ops_scratch + 2, 30, 1);
}

/**
 * d64_invalidate - invalidate internal state
 *
 * This function invalidates all cached state when
 * a card change is detected.
 */
void d64_invalidate(void) {
  free_buffer(bam_buffer);
  bam_buffer   = NULL;
  free_buffer(bam_buffer2);
  bam_buffer2  = NULL;
  bam_refcount = 0;
}

/**
 * d64_unmount - unmount disk image
 * @part: partition number
 *
 * This function in called on image unmount and handles
 * refcounting for the BAM buffers.
 */
void d64_unmount(uint8_t part) {
  /* invalidate BAM buffers that point to the current partition */
  if (bam_buffer) {
    bam_buffer->cleanup(bam_buffer);
    if (bam_buffer->pvt.bam.part == part)
      bam_buffer->pvt.bam.part = 255;
  }

  if (bam_buffer2) {
    bam_buffer2->cleanup(bam_buffer2);
    if (bam_buffer2->pvt.bam.part == part)
      bam_buffer2->pvt.bam.part = 255;
  }

  /* decrease BAM buffer refcounter - it can never be zero while a Dxx is mounted*/
  if (--bam_refcount) {
    free_buffer(bam_buffer);
    free_buffer(bam_buffer2);
    bam_buffer  = NULL;
    bam_buffer2 = NULL;
  }
}


/* ------------------------------------------------------------------------- */
/*  Formatting disk images                                                   */
/* ------------------------------------------------------------------------- */

/* create a 1581/DNP BAM signature */
static void format_add_bam_signature(uint8_t doschar, uint8_t *idbuf) {
  uint8_t *ptr = bam_buffer->data + 2;

  *ptr++ = doschar;
  *ptr++ = doschar ^ 0xff;
  *ptr++ = idbuf[0];
  *ptr++ = idbuf[1];
  *ptr++ = 0xc0;
}

/* copy name and id into some buffer */
static void format_copy_label(uint8_t part, uint8_t *data, uint8_t *name, uint8_t *idbuf) {
  uint8_t *ptr;
  uint8_t count;

  ptr = data + get_param(part, LABEL_OFFSET);
  memset(ptr, 0xa0, 25); /* 25=name+id+dos+spaces+padding */

  count = 16;
  while (*name && count--)
    *ptr++ = *name++;

  memcpy(data + get_param(part, ID_OFFSET), idbuf, 5);
}

static void format_d41_image(uint8_t part, buffer_t *buf, uint8_t *name, uint8_t *idbuf) {
  /* allocate BAM and first directory sector */
  for (uint8_t s=0; s<2; s++)
    allocate_sector(part, D41_BAM_TRACK, s);

  /* 18/0 is now available via bam_buffer */
  uint8_t *ptr = bam_buffer->data;
  *ptr++ = 18;
  *ptr++ = 1;
  *ptr++ = 0x41;

  /* copy disk label and ID */
  idbuf[3] = '2';
  idbuf[4] = 'A';
  format_copy_label(part, bam_buffer->data, name, idbuf);
  /* two additional 0xa0 characters on 5.25" disks */
  bam_buffer->data[0xa9] = 0xa0;
  bam_buffer->data[0xaa] = 0xa0;

  clear_dir_sector(part, D41_BAM_TRACK, 1, buf->data);
}

static void format_d71_image(uint8_t part, buffer_t *buf, uint8_t *name, uint8_t *idbuf) {
  /* almost everything is the same as D41 */
  format_d41_image(part, buf, name, idbuf);

  /* add double-sided marker in 18/0 (still in bam_buffer) */
  bam_buffer->data[3] = 0x80;

  /* allocate all of track 53 */
  for (uint8_t s=0; s<19; s++)
    allocate_sector(part, D71_BAM2_TRACK, s);
}

static void format_d81_image(uint8_t part, buffer_t *buf, uint8_t *name, uint8_t *idbuf) {
  /* allocate 40/0 to 40/3 */
  for (uint8_t s=0; s<4; s++)
    allocate_sector(part, D81_BAM_TRACK, s);

  /* bam_buffer buffer now holds 40/1 */
  bam_buffer->data[0] = 40;
  bam_buffer->data[1] = 2;
  format_add_bam_signature('D', idbuf);
  // mustflush was already set by allocate_sector

  /* switch bam_buffer to 40/2 */
  (void)sectors_free(part, 41);
  bam_buffer->data[0] = 0;
  bam_buffer->data[1] = 0xff;
  format_add_bam_signature('D', idbuf);
  bam_buffer->mustflush = 1;

  /* build contents of 40/0 */
  uint8_t *ptr = buf->data;
  *ptr++ = 40;
  *ptr++ = 3;
  *ptr++ = 'D';

  /* copy disk label and ID to buffer */
  idbuf[3] = '3';
  idbuf[4] = 'D';
  format_copy_label(part, buf->data, name, idbuf);

  /* write 40/0 */
  if (image_write(part, /*sector_offset(part, 40, 0)*/ (40-1)*40*256L,
                  buf->data, 256, 0))
    return;

  clear_dir_sector(part, D81_BAM_TRACK, 3, buf->data);
}

static void format_dnp_image(uint8_t part, buffer_t *buf, uint8_t *name, uint8_t *idbuf) {
  /* Note: Formatting is only accepted while in the root dir, so the
           d64param struct doesn't need to be changed */
  for (uint8_t s=0; s<35; s++)
    allocate_sector(part, DNP_BAM_TRACK, s);

  /* add BAM signature - first BAM sector is in bam_buffer because of allocate_sector */
  format_add_bam_signature('H', idbuf);
  bam_buffer->data[DNP_BAM_LAST_TRACK_OFS] = get_param(part, LAST_TRACK);

  /* build root dirheader */
  uint8_t *ptr = buf->data;
  memset(ptr, 0, 256);
  *ptr++ = 1;
  *ptr++ = 34;
  *ptr++ = 'H';
  idbuf[3] = '1';
  idbuf[4] = 'H';
  format_copy_label(part, buf->data, name, idbuf);
  buf->data[DNP_DIRHEADER_ROOTHDR_TRACK]  = 1;
  buf->data[DNP_DIRHEADER_ROOTHDR_SECTOR] = 1;

  /* write 1/1 */
  if (image_write(part, /*sector_offset(part, 1, 1)*/ 256,
                  buf->data, 256, 0))
    return;

  clear_dir_sector(part, 1, DNP_ROOTDIR_SECTOR, buf->data);
}

static void format_d80_image(uint8_t part, buffer_t *buf, uint8_t *name, uint8_t *idbuf) {
  // TODO: implement D80 format
}

static void format_d82_image(uint8_t part, buffer_t *buf, uint8_t *name, uint8_t *idbuf) {
  // TODO: implement D82 format
}

static void d64_format(uint8_t part, uint8_t *name, uint8_t *id) {
  buffer_t *buf;
  uint8_t  idbuf[5];
  uint16_t t;
  uint16_t  s;

  /* allow format on DNP only when in the root directory */
  if (partition[part].imagetype == D64_TYPE_DNP &&
      (partition[part].current_dir.dxx.track  != 1 ||
       partition[part].current_dir.dxx.sector != 1)) {
    /* just ignore, CMD HD doesn't return an error either */
    return;
  }

  /* grab a buffer as work area */
  buf = alloc_buffer();
  if (buf == NULL)
    return;

  mark_write_buffer(buf);
  unstick_buffer(buf);
  mark_buffer_dirty(buf);
  memset(buf->data, 0, 256);

  /* Flush BAM buffers and mark their contents as invalid */
  d64_bam_commit();
  bam_buffer->pvt.bam.part = 0xff;
  if (bam_buffer2)
    bam_buffer2->pvt.bam.part = 0xff;

  if (id != NULL) {
    /* Clear the data area of the disk image */
    for (t=1; t<=get_param(part, LAST_TRACK); t++) {
      for (s=0; s<sectors_per_track(part, t); s++) {
        if (image_write(part, sector_offset(part, t, s),
                        buf->data, 256, 0))
          return;
      }
    }

    /* Copy the new ID into the buffer */
    idbuf[0] = id[0];
    idbuf[1] = id[1];
  } else {
    /* Read the old ID into the buffer */
    path_t path;
    path.part = part;
    path.dir.dxx.track  = get_param(part, DIR_TRACK);
    path.dir.dxx.sector = 1; // only relevant for DNP
    if (d64_getid(&path, idbuf))
      return;

    /* clear the entire directory track */
    /* This is not accurate, but I do not care. */
    t = get_param(part, DIR_TRACK);
    for (s=0; s < sectors_per_track(part, t); s++) {
      if (image_write(part, sector_offset(part, t, s),
                      buf->data, 256, 0))
        return;
    }
  }
  idbuf[2] = 0xa0;

  /* Mark all sectors as free */
  for (t=1; t<=get_param(part, LAST_TRACK); t++) {
    for (s=0; s<sectors_per_track(part, t); s++)
      free_sector(part,t,s);
  }

  /* call imagetype-specific format function */
  partition[part].d64data.format_function(part, buf, name, idbuf);

  /* FIXME: Clear the error info block */
}


/* ------------------------------------------------------------------------- */
/*  ops struct                                                               */
/* ------------------------------------------------------------------------- */

const PROGMEM fileops_t d64ops = {
  d64_open_read,
  d64_open_write,
  d64_open_rel,
  d64_delete,
  d64_getdisklabel,
  d64_getdirlabel,
  d64_getid,
  d64_freeblocks,
  d64_read_sector,
  d64_write_sector,
  d64_format,
  d64_opendir,
  d64_readdir,
  d64_mkdir,
  d64_chdir,
  d64_rename
};
