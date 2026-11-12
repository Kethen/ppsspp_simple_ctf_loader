// SPDX-License-Identifier: GPL-3.0-only

#include "pspimport.s"

// PPSSPP is missing UtilsForKernel

IMPORT_START "UtilsForUser",0x40090000
IMPORT_FUNC "UtilsForUser",0xF8FCD5BA,sceKernelUtilsSha1BlockInit_
IMPORT_FUNC "UtilsForUser",0x346F6DA8,sceKernelUtilsSha1BlockUpdate_
IMPORT_FUNC "UtilsForUser",0x585F1C09,sceKernelUtilsSha1BlockResult_
