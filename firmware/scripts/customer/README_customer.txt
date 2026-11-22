=======================================================
   FISH PUMP - FIRMWARE INSTALLATION GUIDE
=======================================================

This folder installs the firmware on a Fish Pump control board
(ESP32-S3 DevKitC-1, 16 MB). Windows 10 or 11 and a USB cable are all you need.

ATTENTION: plug the cable into the port labelled "COM" (or "UART"),
NOT the one labelled "USB". That port puts the board in download mode
by itself, no buttons needed.

-------------------------------------------------------
 INSTALL (new board, or reinstall)
-------------------------------------------------------
1. Plug the board into the PC.
2. Double-click FLASH.bat.
3. Type the COM port number when asked (for example 5). If only one
   port is listed, just press Enter. Type R to scan again.
4. Wait for "[OK] Firmware installed."
5. The window then shows the board's Wi-Fi name (FishPump-Setup) and
   its password. Write the password on a label and stick it on the device.

NOTE: install erases everything on the board first, including saved
settings. Use the web page for normal updates (see below).

-------------------------------------------------------
 FORGOT THE WI-FI PASSWORD?
-------------------------------------------------------
The password is calculated from the board's own hardware address.
Plug the board in by USB and double-click SHOW_PASSWORD.bat. Nothing is
erased or changed.

-------------------------------------------------------
 ERASE EVERYTHING
-------------------------------------------------------
Double-click ERASE_FLASH.bat and type YES. The board is then empty;
run FLASH.bat afterwards to install the firmware again.

-------------------------------------------------------
 NORMAL UPDATES (no cable)
-------------------------------------------------------
Updates are done from the web page or from ThingsBoard by the person who
looks after the system. The file for the web page ends in .ota.bin and is
the one named like fishpump-<version>-<key>.ota.bin.
