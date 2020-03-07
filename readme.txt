Advanced Development Mod Tools 0.1
] 1 build after VHLT v34
] 7th of March 2020

Based on Solokiller's VHLT-clean repo: https://github.com/Solokiller/VHLT-clean/tree/remove-xash

New features in v35:
* -maxentrange parameter for HLCSG, defines the maximum world boundaries. It's 8192 in vanilla HL, but if you need to use this for ADM or SC, you can bump it up to as much as you want. 32768 by default.
* BOLIDHINT, a combination of SOLIDHINT and BEVEL. In other words, it brings cleaner BSP subdivision (from SOLIDHINT), and it does not expand clipnodes on edges where brushes meet (from BEVEL). Usable on terrain mostly.

Vluzacn's readme:
---- How to install ----

1. Open Hammer or Batch Compiler's configuration dialog.
  Set CSG, BSP, VIS, RAD tool pathes to the 'hlcsg.exe', 'hlbsp.exe', 'hlvis.exe', 'hlrad.exe' in 'tools' folder.
  If you are running 64-bit Windows, use the 'hlcsg_x64.exe', 'hlbsp_x64.exe', 'hlvis_x64.exe' and 'hlrad_x64.exe'.
  The main benefit of the 64-bit version is no memory allocation failures, because the 64-bit tools have access to more than 2GB of system memory.

2. Open Hammer's configuration dialog.
  Add 'zhlt.wad' into your wad list.
  Add 'zhlt.fgd' into your fgd list.

3. You may also be interested in the these tools (which are well supported by this compiler):
- Larger grid patch for Hammer
    http://forums.svencoop.com/showthread.php?p=461330#post461330
- Floating point coordinates output patch for Hammer
    http://forums.svencoop.com/showthread.php?t=39037


---- About ----

Vluzacn's ZHLT v34 (2015/8/17)
This is Vluzacn's (email: vluzacn@163.com) 34th custom build of ZHLT 3.4 (http://zhlt.info/)
More information are available at  http://forums.svencoop.com/showthread.php?t=38059