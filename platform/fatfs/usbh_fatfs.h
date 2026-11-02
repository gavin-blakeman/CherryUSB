//*********************************************************************************************************************************
//
// PROJECT:							CherryUSB
// FILE:								usbh_fatfs.h
// SUBSYSTEM:						Header file for USB Host file system 
// LANGUAGE:						C
// TARGET OS:						None
// LIBRARY DEPENDANCE:	Standard
// NAMESPACE:						None
// AUTHOR:							Gavin Blakeman (GGB)
// LICENSE:             GPLv2
//
//                      Copyright 2026 Gavin Blakeman.                      
//
// OVERVIEW:            Implements the main function.
//
// HISTORY:             2026-10-30/GGB - File Created
//
//*********************************************************************************************************************************

#ifndef USBH_FATFS_H
#define USBH_FATFS_H

// Fat FS
#include "ff.h"
#include "diskio.h"

#ifdef __cplusplus
extern "C" {
#endif   

int USB_disk_initialize(void);
int USB_disk_read(BYTE *buff, LBA_t sector, UINT count);
int USB_disk_status(void);
int USB_disk_write(const BYTE *buff, LBA_t sector, UINT count);
int USB_disk_ioctl(BYTE cmd, void *buff);

#ifdef __cplusplus
}
#endif

#endif // USBH_FATFS_H