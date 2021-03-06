/**************************************************************************/
/*!
    @file     msc_uf2.c
    @author   hathach (tinyusb.org)

    @section LICENSE

    Software License Agreement (BSD License)

    Copyright (c) 2018, Adafruit Industries (adafruit.com)
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:
    1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
    3. Neither the name of the copyright holders nor the
    names of its contributors may be used to endorse or promote products
    derived from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ''AS IS'' AND ANY
    EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
/**************************************************************************/

#include "msc_uf2.h"

#if CFG_TUD_MSC

#include "pstorage.h"

// for formatting fatfs when Softdevice is not enabled
#include "nrf_nvmc.h"


#define SECTORS_PER_FAT   7
#define ROOT_DIR_SECTOR   8

#define SECTOR_DATA       (1+SECTORS_PER_FAT+ROOT_DIR_SECTOR)

/*------------------------------------------------------------------*/
/* MACRO TYPEDEF CONSTANT ENUM
 *------------------------------------------------------------------*/
enum
{
  WRITE10_IDLE,
  WRITE10_ERASING,
  WRITE10_ERASED,
  WRITE10_WRITING,
  WRITE10_WRITTEN,
  WRITE10_FAILED
};

enum { FL_PAGE_SIZE = 4096 };

/*------------------------------------------------------------------*/
/* UF2
 *------------------------------------------------------------------*/
void read_block(uint32_t block_no, uint8_t *data);

/*------------------------------------------------------------------*/
/* VARIABLES
 *------------------------------------------------------------------*/
static uint8_t _page_cached[FL_PAGE_SIZE] ATTR_ALIGNED(4);

volatile static uint8_t _wr10_state;
static pstorage_handle_t _fat_psh = { .module_id = 0, .block_id = MSC_UF2_FLASH_ADDR_START } ;

static scsi_inquiry_data_t const mscd_inquiry_data =
{
    .is_removable         = 1,
    .version              = 2,
    .response_data_format = 2,
    .vendor_id            = "Adafruit",
    .product_id           = "Feather52840",
    .product_revision     = "1.0"
};

static scsi_read_capacity10_data_t const mscd_read_capacity10_data =
{
    .last_lba   = ENDIAN_BE(MSC_UF2_BLOCK_NUM-1), // read capacity
    .block_size = ENDIAN_BE(MSC_UF2_BLOCK_SIZE)
};

static scsi_sense_fixed_data_t mscd_sense_data =
{
    .response_code        = 0x70,
    .sense_key            = 0, // no errors
    .additional_sense_len = sizeof(scsi_sense_fixed_data_t) - 8
};

static scsi_read_format_capacity_data_t const mscd_format_capacity_data =
{
    .list_length     = 8,
    .block_num       = ENDIAN_BE(MSC_UF2_BLOCK_NUM), // write capacity
    .descriptor_type = 2, // TODO formatted media, refractor to const
    .block_size_u16  = ENDIAN_BE16(MSC_UF2_BLOCK_SIZE)
};

static scsi_mode_parameters_t const msc_dev_mode_para =
{
    .mode_data_length        = 3,
    .medium_type             = 0,
    .device_specific_para    = 0,
    .block_descriptor_length = 0
};

/*------------------------------------------------------------------*/
/*
 *------------------------------------------------------------------*/
static inline uint32_t lba2addr(uint32_t lba)
{
  return MSC_UF2_FLASH_ADDR_START + lba*MSC_UF2_BLOCK_SIZE;
}

static void fat12_mkfs(void);


/*------------------------------------------------------------------*/
/*
 *------------------------------------------------------------------*/
static void fat_pstorage_cb(pstorage_handle_t * p_handle, uint8_t op_code, uint32_t result, uint8_t  * p_data, uint32_t  data_len)
{
  if ( result != NRF_SUCCESS )
  {
    _wr10_state = WRITE10_FAILED;
    TU_ASSERT(false, );
  }

  if ( PSTORAGE_CLEAR_OP_CODE == op_code)
  {
    if ( WRITE10_ERASING == _wr10_state) _wr10_state = WRITE10_ERASED;
  }
  else if ( PSTORAGE_STORE_OP_CODE ==  op_code)
  {
    if ( WRITE10_WRITING == _wr10_state) _wr10_state = WRITE10_WRITTEN;
  }
}

/*------------------------------------------------------------------*/
/* API
 *------------------------------------------------------------------*/
void msc_uf2_init(void)
{
  pstorage_module_param_t  fat_psp = { .cb = fat_pstorage_cb};
  pstorage_register(&fat_psp, &_fat_psh);
}

void msc_uf2_mount(void)
{
  _wr10_state = WRITE10_IDLE;

  // reset every time it is plugged
}

void msc_uf2_umount(void)
{

}

//--------------------------------------------------------------------+
// tinyusb callbacks
//--------------------------------------------------------------------+
int32_t tud_msc_scsi_cb (uint8_t rhport, uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize)
{
  // read10 & write10 has their own callback and MUST not be handled here

  void const* ptr = NULL;
  uint16_t len = 0;

  // most scsi handled is input
  bool in_xfer = true;

  switch (scsi_cmd[0])
  {
    case SCSI_CMD_INQUIRY:
      ptr = &mscd_inquiry_data;
      len = sizeof(scsi_inquiry_data_t);
    break;

    case SCSI_CMD_READ_CAPACITY_10:
      ptr = &mscd_read_capacity10_data;
      len = sizeof(scsi_read_capacity10_data_t);
    break;

    case SCSI_CMD_REQUEST_SENSE:
      ptr = &mscd_sense_data;
      len = sizeof(scsi_sense_fixed_data_t);
    break;

    case SCSI_CMD_READ_FORMAT_CAPACITY:
      ptr = &mscd_format_capacity_data;
      len = sizeof(scsi_read_format_capacity_data_t);
    break;

    case SCSI_CMD_MODE_SENSE_6:
      ptr = &msc_dev_mode_para;
      len = sizeof(msc_dev_mode_para);
    break;

    case SCSI_CMD_TEST_UNIT_READY:
      ptr = NULL;
      len = 0;
    break;

    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
      ptr = NULL;
      len = 0;
    break;

    default:
      // negative is error -> Data stage is STALL, status = failed
      return -1;
  }

  // return len must not larger than bufsize
  TU_ASSERT( bufsize >= len );

  if ( ptr && len )
  {
    if(in_xfer)
    {
      memcpy(buffer, ptr, len);
    }else
    {
      // SCSI output
    }
  }

  //------------- clear sense data if it is not request sense command -------------//
  if ( SCSI_CMD_REQUEST_SENSE != scsi_cmd[0] )
  {
    mscd_sense_data.sense_key                  = SCSI_SENSEKEY_NONE;
    mscd_sense_data.additional_sense_code      = 0;
    mscd_sense_data.additional_sense_qualifier = 0;
  }

  return len;
}

/*------------------------------------------------------------------*/
/* Tinyusb Flash READ10 & WRITE10
 *------------------------------------------------------------------*/
static bool fl_page_erase(uint32_t addr)
{
  _fat_psh.block_id = addr;
  return NRF_SUCCESS == pstorage_clear(&_fat_psh, FL_PAGE_SIZE);
}

static bool fl_page_write(uint32_t addr, uint8_t* buf, uint16_t bufsize)
{
  _fat_psh.block_id = addr;
  return NRF_SUCCESS == pstorage_store(&_fat_psh, buf, bufsize, 0);
}

int32_t tud_msc_read10_cb (uint8_t rhport, uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize)
{
  (void) rhport; (void) lun;

  // since we return block size each, offset should always be zero
  TU_ASSERT(offset == 0, -1);

  uint32_t count = 0;

  while ( count < bufsize )
  {
    read_block(lba, buffer);

    lba++;
    buffer += 512;
    count  += 512;
  }

  return count;
}

int32_t tud_msc_write10_cb (uint8_t rhport, uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize)
{
  (void) rhport; (void) lun;

  return bufsize;

  uint32_t addr = lba2addr(lba) + offset;

  /* 0. Check if flash is the same as data -> skip if matches
   * 1. queue flash erase pstorage_clear(), return 0 until erasing is done
   * 2. queue flash writing, return 0 until writing is complete
   * 3. return written bytes.
   *
   * Note since CFG_TUD_MSC_BUFSIZE is 4KB, bufsize is cap at 4KB
   */

  switch(_wr10_state)
  {
    case WRITE10_IDLE:
    {
      // No need to write if flash's content matches with data
      if ( 0 == memcmp(buffer, (void*) addr, bufsize) ) return bufsize;

      uint32_t page_addr = align4k(addr);
      uint32_t off4k     = offset4k(addr);

      // Cache contents from start of page to current address
      if ( off4k )
      {
        memcpy(_page_cached, (uint8_t*) page_addr, off4k);
      }

      // Copy new data
      memcpy(_page_cached+off4k, buffer, bufsize);

      // Cache contents after data to end of page
      if ( off4k + bufsize < FL_PAGE_SIZE)
      {
        memcpy(_page_cached+off4k+bufsize, (uint8_t*) (addr+bufsize), FL_PAGE_SIZE - (off4k + bufsize ) );
      }

      // Start erasing
      TU_ASSERT( fl_page_erase(align4k(addr)), -1);

      _wr10_state = WRITE10_ERASING;

      // Tell tinyusb that we are not ready to consume its data
      // The stack will keep the data and call again
      return 0;
    }
    break;

    case WRITE10_ERASING:
      // still erasing, nothing else to do
      return 0;
    break;

    case WRITE10_ERASED:
      // Start writing
      TU_ASSERT( fl_page_write(align4k(addr), _page_cached, FL_PAGE_SIZE), -1);
      _wr10_state = WRITE10_WRITING;
      return 0;
    break;
    
    case WRITE10_WRITING:
      return 0;
    break;

    case WRITE10_WRITTEN:
      _wr10_state = WRITE10_IDLE; // back to idle

      // positive return means we complete the operation, tinyusb can receiving next write10
      return bufsize;
    break;

    case WRITE10_FAILED:
      _wr10_state = WRITE10_IDLE;
      return -1;
    break;

    default: return -1; break;
  }
}

#endif
