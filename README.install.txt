Dec-2021

This application is intended to be built on an basic level Debian release such as the Raspberry Pi OS Lite with Xorg added.
Further development can then proceed from a host computer via SSH (-X) to the Pi.

Since external X based applicatoon will run full screen possibly without any window manager, some adjustment can be made to have them correctly positioned at the screen:

Examples for 1 800x480 touch screen device:

OpenCPN:
~/.opencpn/opencpn.conf:
  ClientPosX=0
  ClientPosY=0
  ClientSzX=800
  ClientSzY=480

zyGrib:
~/data/config/zygrib.ini
  mainWindowPos=@Point(0 0)
  mainWindowSize=@Size(800 480)

zyGrib's install rules installs everything in the users home path, so to have them system wide accessible do this:

cp ~/zyGrib/zyGrib /usr/local/bin/zyGrib
.. but first check the execution path in that text file.

Before executing "make install" check these items:

 - If not done already run (as root) raspi-config to set WiFi, localization, i2c and serial port according to the HOWTOs links in the README.md file.

 - run make install : Install the basic run-time files.

 - run make install_x : Install the autostarted X configuration.

 - run make install_kms  : Install the autostarted kmsdrm configuration.

 - Run sudo ./sdlSpeedometer-config to set your preferences for this application.

 - Do NOT run make install_(x/kms) on a desktop system. Instead install desired components manually.

For testing:
 run make start/stop/status
 
 - runt sudo reboot 

When the Speedometer starts you can still run sdlSpeedometer-config from the GPS GUI page. Click the tools icon.

