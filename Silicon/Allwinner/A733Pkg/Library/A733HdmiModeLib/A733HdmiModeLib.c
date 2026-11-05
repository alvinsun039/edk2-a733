/** @file
  Shared HDMI preset-mode table.

  The Setup "Display Mode" presets map 1:1 onto this table
  (DISPLAY_MODE_* order).  Lives in a BASE library so both the HDMI
  driver (A733HdmiMode) and the Setup UI (EDID-filtered mode list) use
  the same data without a driver-to-driver link.

  Copyright (c) 2026, edk2-a733 contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/A733HdmiModeLib.h>

CONST A733_HDMI_MODE  mHdmiModes[] = {

  // DISPLAY_MODE_640_480_60 (25.175 MHz: 24*107/102 = 25.176)
  { 0,  25175000, 640,  800,  656,  752,  480, 525, 490,  492,  FALSE, FALSE, 106, 101 },
  // DISPLAY_MODE_800_600_60
  { 0,  40000000, 800,  1056, 840,  968,  600, 628, 601,  605,  TRUE,  TRUE,  4,   2   },
  // DISPLAY_MODE_1024_768_60
  { 0,  65000000, 1024, 1344, 1048, 1184, 768, 806, 771,  777,  TRUE,  FALSE, 64,  23  },
  // DISPLAY_MODE_1152_864_60
  { 0,  108000000, 1152, 1600, 1216, 1344, 864, 900, 864,  888,  TRUE,  TRUE,  8,   1   },
  // DISPLAY_MODE_1280_720_60 (CEA VIC 4)
  { 4,  74250000, 1280, 1650, 1390, 1430, 720, 750, 725,  730,  TRUE,  TRUE,  98,  31  },
  // DISPLAY_MODE_1280_768_60
  { 0,  79500000, 1280, 1664, 1344, 1472, 768, 798, 771,  778,  TRUE,  FALSE, 52,  15  },
  // DISPLAY_MODE_1280_800_60 (83.46 MHz: 24*167/48 = 83.5)
  { 0,  83460000, 1280, 1440, 1304, 1368, 800, 823, 801,  808,  FALSE, TRUE,  166, 47  },
  // DISPLAY_MODE_1280_960_60
  { 0,  108000000, 1280, 1800, 1376, 1440, 960, 1000, 961, 964,  TRUE,  TRUE,  8,   1   },
  // DISPLAY_MODE_1280_1024_60
  { 0,  108000000, 1280, 1688, 1328, 1440, 1024, 1066, 1025, 1028, TRUE,  TRUE,  8,   1   },
  // DISPLAY_MODE_1360_768_60
  { 0,  85500000, 1360, 1792, 1504, 1536, 768, 800, 771,  790,  TRUE,  TRUE,  56,  15  },
  // DISPLAY_MODE_1400_1050_60 (121.75 MHz: 24*81/16 = 121.5)
  { 0,  121750000, 1400, 1916, 1560, 1680, 1050, 1083, 1050, 1059, TRUE, TRUE,  80,  15  },
  // DISPLAY_MODE_1440_900_60
  { 0,  106500000, 1440, 1904, 1600, 1712, 900, 934, 900,  910,  FALSE, TRUE,  70,  15  },
  // DISPLAY_MODE_1600_900_60
  { 0,  108000000, 1600, 1800, 1648, 1680, 900, 1000, 904, 912,  FALSE, TRUE,  8,   1   },
  // DISPLAY_MODE_1600_1200_60
  { 0,  162000000, 1600, 2160, 1664, 1856, 1200, 1250, 1201, 1235, TRUE,  TRUE,  26,  3   },
  // DISPLAY_MODE_1680_1050_60
  { 0,  146250000, 1680, 2240, 1808, 1912, 1050, 1089, 1050, 1063, TRUE,  TRUE,  194, 31  },
  // DISPLAY_MODE_1920_1080_60 (CEA VIC 16 - the anchor mode)
  { 16, 148500000, 1920, 2200, 2008, 2052, 1080, 1125, 1084, 1089, TRUE,  TRUE,  98,  15  },
  // DISPLAY_MODE_1920_1200_60 (154 MHz: 24*77/12 = 154)
  { 0,  154000000, 1920, 2592, 2008, 2052, 1200, 1245, 1203, 1206, TRUE,  TRUE,  76,  11  },
  // DISPLAY_MODE_2048_1152_60
  { 0,  162000000, 2048, 2200, 2096, 2128, 1152, 1200, 1156, 1160, TRUE,  TRUE,  26,  3   },
  // DISPLAY_MODE_2560_1080_60 (198 MHz: 24*33/4 = 198)
  { 0,  198000000, 2560, 2880, 2648, 2680, 1080, 1125, 1084, 1089, TRUE,  TRUE,  32,  3   },
  // DISPLAY_MODE_2560_1440_60
  { 0,  241500000, 2560, 2720, 2608, 2640, 1440, 1481, 1444, 1448, TRUE,  TRUE,  160, 15  },
  // DISPLAY_MODE_2560_1600_60
  { 0,  268500000, 2560, 2880, 2648, 2680, 1600, 1641, 1604, 1608, TRUE,  TRUE,  178, 15  },
  // DISPLAY_MODE_3440_1440_60 (319.75 MHz: 24*253/19 = 319.58)
  { 0,  319750000, 3440, 3600, 3488, 3520, 1440, 1481, 1444, 1448, TRUE,  TRUE,  252, 18  },
  // DISPLAY_MODE_3840_2160_30 (CEA VIC 93)
  { 93, 297000000, 3840, 4120, 4016, 4056, 2160, 2200, 2164, 2168, TRUE,  TRUE,  98,  7   },
  // DISPLAY_MODE_3840_2160_60 (CEA VIC 97)
  { 97, 594000000, 3840, 4120, 4016, 4056, 2160, 2200, 2164, 2168, TRUE,  TRUE,  98,  3   },
  // DISPLAY_MODE_4096_2160_30 (CEA VIC 94)
  { 94, 297000000, 4096, 4376, 4272, 4312, 2160, 2200, 2164, 2168, TRUE,  TRUE,  98,  7   },
  // DISPLAY_MODE_4096_2160_60 (CEA VIC 98)
  { 98, 594000000, 4096, 4376, 4272, 4312, 2160, 2200, 2164, 2168, TRUE,  TRUE,  98,  3   },
};

/**
  Return the preset mode table and its entry count.

  @param[out] Count  Entry count (0 when the table is empty).

  @return  Pointer to the first preset mode entry.
**/
const A733_HDMI_MODE *
A733HdmiGetPresetTable (
  OUT UINT32  *Count
  )
{
  if (Count != NULL) {
    *Count = sizeof (mHdmiModes) / sizeof (mHdmiModes[0]);
  }

  return mHdmiModes;
}
