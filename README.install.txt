Dec-2018

This application is intended to be built on an basic Debian release such as the Raspbian Stretch Lite without any desktop stuff.
It will probably mess up a desktop based system if the "install" rules in the makefile are executed as is.
In such case the command "sudo systemctl set-default multi-user.target" could be usefull to bring down the system to runlevel 3 (no desktop) and then continue with this application from that level.

Since external X based applicatoon will run full screen without any window manager, some adjustment can be made to have them correctly positioned at the screen:

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
 - Run sudo ./sdlSpeedometer-config to set your preferences for this application.

 - run make install
 
 - runt sudo reboot 

When the Speedometer starts you can still run sdlSpeedometer-config from the GPS GUI page. Click the tools icon.
