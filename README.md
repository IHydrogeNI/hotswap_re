# hotswap_re

A from-scratch reimplementation of the **HotSwap!** tray applet (Kazuyuki Nakayama, v6.3) for
safely hot-swapping SATA/eSATA/NVMe/USB disks, optical drives and other removable devices,
with fixes for the instability the original shows when Windows re-reads its devices.

* Plain C, Win32 API only. **No C runtime, no VC++ redistributable.** The executables import
  only `kernel32`, `user32`, `shell32`, `advapi32`, `setupapi`, `ole32` and `oleaut32`, which ship
  with every Windows release.
* Runs on **Windows XP SP2+ through Windows 11**:
  * `bin\x86\hotswap_re.exe` for 32-bit Windows (XP and later)
  * `bin\x64\hotswap_re.exe` for 64-bit Windows (XP x64 / Vista and later)

  Windows does not let 32-bit programs remove devices on 64-bit Windows, so use the build that
  matches the OS. The x86 build says so if it is started on 64-bit Windows.
* Needs administrator rights (the manifest asks for elevation on Vista+).

## Features (same as HotSwap! 6.3)

| | |
|---|---|
| Left click | List of hot-swappable devices: "Safely hotswap *name* (DISK n) - E:, F:" |
| Ctrl + left click | Select non-hotswap devices (internal disks that do not report hot-plug) to add to the list |
| Right click | Scan for hardware changes, device types (fixed / removable / BD-DVD-CD / safely removable devices), language, autostart, Display PDO, Spindown, Offline, Usage, About, Uninstall, Exit |
| 18 languages | English, Chinese (Simplified/Traditional), Czech, Dutch, French, German, Greek, Hebrew, Hungarian, Italian, Japanese, Korean, Polish, Portuguese, Russian, Spanish, Swedish (translations taken from the original; new messages are English) |
| Spindown | ATA STANDBY IMMEDIATE before removal (plus SCSI START STOP UNIT fallback for USB bridges) |
| Offline | Takes the disk offline (non-persistent) before removal, Vista+ |
| Autostart | Task Scheduler logon task with highest privileges and no time limit on Vista+ (UAC skips elevated Run entries); `HKCU\...\Run` on XP |
| Uninstall | Removes the settings and the autostart entry |

Command line:

```
hotswap_re [drive letter | path | PDO | DISK=# [-Q] | -S | -L | -?]

  E:  or  E  or  C:\mnt\backup      the disk holding that volume
  \Device\0000003f                  by physical device object name
  DISK=1                            by PhysicalDrive number
  -Q   no message boxes             -S   scan for hardware changes (also brings back a removed disk
                                         that is still plugged in)
  -L   list detected devices        -?   help
Exit codes: 0 ok, 1 not found, 2 removal failed/vetoed, 3 bad arguments or privileges
```

Settings are stored in `HKCU\Software\hotswap_re`.

## What was fixed compared to HotSwap!

1. **No half-removed disks.** The original called `SetupDiRemoveDevice` directly. When anything refused
   the removal, Windows queued the disk for removal at the next reboot (`CM_PROB_WILL_BE_REMOVED`, an
   error entry in Device Manager), and the disk later stopped "by itself" without its entry going away.
   hotswap_re first stops the device with `CM_Query_And_Remove_SubTree`. If that is refused, nothing
   changes and you are told which program or service refused it. Only after a clean stop is the device
   node uninstalled, so it disappears from Device Manager without needing a reboot.
2. **Removed disks stay removed while still plugged in.** Windows rescans the storage bus whenever a
   device changes: another USB/HID device plugged in, a PnP scan, Disk Management, the Storage service.
   The SATA controller then re-identifies every port, which spins the drive up and reinstalls the disk.
   No program can stop the controller from doing that. So after a removal, the tray applet watches the
   disk, using its SCSI address. If a rescan brings it back, the applet locks, spins down and removes it
   again within about 2 s. Measured on an ST3250620AS: back at 0.5 s, gone at 2.4 s, and in standby again
   afterwards. The watch ends when the drive is unplugged (its address disappears from the controller),
   after a restart, or when you use **Scan for hardware changes** / `-S`. The watching itself does not
   wake the drive. It only works while the tray applet is running, so enable Autostart.
3. **Scan for hardware changes repairs the original's leftovers.** Disks or optical drives stuck in
   `CM_PROB_WILL_BE_REMOVED` are cleaned up and re-detected without a reboot.
   Spindown (on by default) puts the drive in standby before the removal, and the drive stays in standby
   through the removal. Measured on an ST3250620AS: rescan after a spindown removal takes about 5 s longer
   than without one, because the drive has to spin up again.
3. **No stale device handles.** Nothing is cached. Every menu takes a fresh snapshot of the device tree, and
   the removal re-reads it again and matches the device by its instance ID. If the device went away or was
   re-numbered while the menu was open, you get a "no longer present" message instead of an action on the
   wrong device.
4. **Race-free enumeration.** Walks the live device tree instead of the size-then-fetch list calls, which fail
   when devices appear or disappear between the two calls.
5. **Busy volumes are retried.** The volume lock is retried for about 3 s, because indexers, antivirus and
   Explorer often hold a volume for a moment. All volumes on the device are handled, including ones mounted
   in folders without a drive letter.
6. **The tray stays responsive.** Removal and rescans run on a worker thread, and a second operation can't
   start while one is running.
7. **The tray icon survives Explorer restarts and early logon.** The `TaskbarCreated` message is let through
   the UAC message filter (an elevated process otherwise never receives it), and adding the icon is retried
   while the taskbar does not exist yet at logon.
8. **The system disk is protected.** The disk that holds Windows, and any controller it sits on, is never offered
   and is refused on the command line.
9. If the program is moved, the autostart task is updated to point at the new location.

## Building

Install [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) (the **msvcrt** build, any host),
then:

```
set LLVM_MINGW=C:\path\to\llvm-mingw-...-msvcrt-x86_64
build.bat
```

`tools\genlang.py` regenerates `src\lang_gen.h` from the original `HotSwap!.EXE` (needs `pip install pefile`),
and `tools\genicon.py` regenerates the icon.
