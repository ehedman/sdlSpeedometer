# sdlSpeedometer
README Aug-2024

The sdlSpeedometer application is a marine instruemnt solution that features electronic instrument displays, typically used on private sailing yachts.
The look and feel of the visualized instruments tries to mimic the look of real physical instruments and will by design have less of a digital look.

This application is based on the Rasperry Pi and the [Simple DirectMedia Layer - SDL](https://www.libsdl.org/)

For the Raspberry Pi 4B with OS bookworm the wayland video can is used, though some features will not be available by not using X.

For the best user experience with all features enabled, a clean x11 configuration is recommended since many graphical subtasks also have this dependency.

The instruments can be accessed one-by-one by a mouse click or directly from the touch screen menu.

The communication mechanism between this application with its GUI and data sources uses two paralell paths:
 - Data collected from a [BerryGPS-IMUv2](http://ozzmaker.com/new-products-berrygps-berrygps-imu) - GPS and 10DOF sensor for The Raspberry Pi - Accelerometer, Gyroscope, Magnetometer and Barometric/Altitude Sensor.
 - Alternatively data from an NMEA-0183 network server data from an NMEA-0183 network server such as the open source [kplex](http://www.stripydog.com/kplex/) application to drive other instrument from the yacht's network.
 - Alternatively data from an NMEA-2K (SeatalkNG) to an NMEA-0183 USB dongle.

This instrument can work independently and always provide compass, heading, position, speed and roll even if all power fails on the yacht, if it has its own battery backup.

Currently there are eight virtual instrument working (data source within brackets):

    Compass       : With heading and roll (BerryGPS-IMUv2) and/or heading from NMEA-net
    GPS           : Lo, Lat and Heading (BerryGPS-IMUv2) and/or heading from NMEA-net
    Log           : SOW, SOG (NMEA-net)
    Wind          : Real, Relative and speed (NMEA-net)
    Depth         : With low water warning and water temp (NMEA-net)
    Environment   : Page with Voltage, Current, Temp and Power plotting (proprietary NMEA net "$P" sentences)
    Water         : Page with fresh water tank status and TDS quality (Requires https://github.com/ehedman/flowSensor)

There is also a page to perform compass calibration includning on-line fetch of declination values from [NOAA](https://www.ngdc.noaa.gov/geomag/calculators/magcalc.shtml)

### External Applications
sdlSpeedometer in itself is a very responsive application runing in an embedded system context with SDL2. However, sdlSpeedometer can be parametized to launch almost any external application by means of a configuration tool invoked from the GUI or from a shell. Run sdlSpeedometer-config to add XyGrip and/or Opencpn.

Kodi can be added as an external application to be used as a Jukebox style player togheter with its [Kore](https://play.google.com/store/apps/details?id=org.xbmc.kore&hl=sv&gl=US) remote control phone app.

sdlSpeedometer has also a built-in RFB (VNC) server function so that an external VNC client can connect a slave instrument on a computer and/or a tablet with a VNC client.

### Tested runtime environment
- Note this this is mainly an EMBEDDED solution based on the Lite versions of the Pi OS and is not suitable for installation in a desktop environment but running the stand alone binary for testing purposes is doable.
- Raspberry Pi 3B+ and 4B and a 7 inch touch display.
- NMEA Network Server (kplex) to feed the  yacht's set of instrument data running either on the Pi or accessible in the network neighborhood.
- This application will also work flawlessly under Windows WSL (Windows Subsystem for Linux).

### System Software prerequisites
- An updated Raspberry Pi OS Lite to start with
- sudo apt install whiptail gcc git make
- sudo apt install xorg (not on a workstation)

### SDL2 Software prerequisites
The SDL2 packages needed are:
- sudo apt install libsdl2-dev libsdl2-image-dev libsdl2-net-dev libsdl2-ttf-dev

### Library dependencies from Debian repos
- sudo apt install libcurl4-gnutls-dev i2c-tools libi2c-dev sqlite3 libsqlite3-dev libpng-dev
- sudo apt install libtiff5-dev libjpeg-dev libfreetype6-dev libts-dev libinput-dev
- sudo apt install libwebp-dev libvncserver-dev 

### Other libraries 
- Optionally [plot-sdl](https://github.com/bertrandmartel/plot-sdl) to plot a live power shart.

### Application dependencies for running external applications from sdlSpeedometer
- sudo apt install xterm onboard xloadimage wmctrl

### Optional application dependencies for improved user experiences for subtasks.
- sudo apt install devilspie2 xfwm4 yad xdotool

### External applications to be launched from sdlSpeedometer
- sudo apt install XyGrib
- sudo apt install opencpn
- sudo apt install kodi
- Use sdlSpeedometer-config to add these applications and also add sdlSpeedometer-stat (included) to show system status.

### Software used
- [Raspberry Pi OS Lite bullseye or bookworm](https://www.raspberrypi.com/software/operating-systems/)

### Build and install on a Pi
- make
- ./sdlSPeedometer-config (Check the configuration - default values ​​should do)
- ./sdlSpeedometer -i -g (-i,-g: do not use the BerryGPS hat)
- make install
- make install_x
- systemclt start sdlSpeedometer.service (will be enabled at boot time) or make start

### Build and test on the host (Mint, Ubuntu, Debian)
- make
- ./sdlSPeedometer-config (Check the configuration - default values ​​should do)
- ./sdlSpeedometer -i -g (-i,-g: do not use the BerryGPS hat)

### Enable audible warnings
- Set preferences with sdlSPeedometer-config
- Start sdlSpeedometer with "SDL_AUDIODRIVER=alsa ./sdlSpeedometer -p" and possible -i -g as well
- Eventually set these preferences in /etc/default/sdlSpeedometer after "make install_x" has been executed on a Pi

### HOWTOs
- [How to Enable i2c on the Raspberry Pi](https://www.raspberrypi-spy.co.uk/2014/11/enabling-the-i2c-interface-on-the-raspberry-pi/)
- [BerryGPS setup Guide for Raspberry Pi](http://ozzmaker.com/berrygps-setup-guide-raspberry-pi)
- [Create a Digital Compass with the Raspberry Pi](http://ozzmaker.com/compass1)

### Embedded display settings
This is an example to set up a 7 inch HDMI display by adding these lines into /boot/config.txt on a Bullseye linux:<br>
hdmi_cvt=800 480 60 6<br>
hdmi_group=2<br>
hdmi_mode=87<br>
hdmi_drive=2<br>

For bookworm add video=HDMI-A-1:800x480M@59 to /boot/cmdline.txt and this sample file to /usr/share/X11/xorg.conf.d/Xorg.conf

    Section "Device"
        Identifier "Card0"
        Option "HDMI-1"
    EndSection

    Section "Monitor"
            Identifier "HDMI-1"
            ModelName "LEN L1950wD"
            Modeline "800x480" 29.22  800 824 896 992  480 483 493 500 -hsync +vsync
            Option "PreferredMode" "800x480"

### See also
[An Open Source Yacht Glass Cockpit](https://github.com/ehedman/websocketNmea)

### Screenshots
<img src="https://hedmanshome.se/sdlspeedometer20.png" width=100%>
<img src="https://hedmanshome.se/sdlspeedometer21.png" width=100%>
<img src="https://hedmanshome.se/sdlspeedometer22.png" width=100%>
<img src="https://hedmanshome.se/sdlspeedometer23.png" width=100%>
<img src="https://hedmanshome.se/sdlspeedometer24.png" width=100%>
<img src="https://hedmanshome.se/sdlspeedometer9.png" width=100%>
<img src="https://hedmanshome.se/sdlspeedometer10.png" width=100%>
<img src="https://hedmanshome.se/sdlspeedometer27.png" width=100%>
- XyGrib just launched on a 7" touch display
<img src="https://hedmanshome.se/sdlspeedometer11.png" width=100%>
- sdlSpeedometers' configurator
<img src="https://hedmanshome.se/sdlspeedometer28.png" width=100%>
