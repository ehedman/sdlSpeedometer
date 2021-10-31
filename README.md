# sdlSpeedometer
README Nov-2021

The sdlSpeedometer application is a marine instruemnt solution that features electronic instrument displays, typically used on private sailing yachts.
The look and feel of the visualized instruments tries to mimic the look of real physical instruments and will by design avoid a digital look.

This application is based on the Rasperry Pi 4B and the [Simple DirectMedia Layer - SDL](https://www.libsdl.org/)

For the Rasperry Pi 4B the X video backend is used. 

The instruments can be accessed one-by-one by clicking on a mouse or directly from the touch screen menu.

The communication mechanism between this application with its GUI and data sources uses two paralell paths:
 - Data collected from a [BerryGPS-IMUv2](http://ozzmaker.com/new-products-berrygps-berrygps-imu) - GPS and 10DOF sensor for The Raspberry Pi - Accelerometer, Gyroscope, Magnetometer and Barometric/Altitude Sensor.
 - Optionally data from a NMEA net Server such as the open source [kplex](http://www.stripydog.com/kplex/) application to drive other instrument from the yacht's network.

This instrument can work independently and always provide compass, heading, position, speed and roll even if all power fails on the yacht, if it has its own battery backup.

Currently there are eight virtual instrument working (data source within brackets):

    Compass       : With heading and roll (BerryGPS-IMUv2)
    GPS           : Lo, Lat and Heading (BerryGPS-IMUv2)
    Log           : SOW, SOG (NMEA net)
    Wind          : Real, Relative and speed (NMEA net)
    Depth         : With low water warning and water temp (NMEA net)
    Environment   : Page with Voltage, Current, Temp and Power plotting (proprietary NMEA net "$P" sentences)

There is also a page to perform compass calibration includning on-line fetch of declination values from [NOAA](https://www.ngdc.noaa.gov/geomag/calculators/magcalc.shtml)

### External Applications
sdlSpeedometer in itself is a very responsive application runing in an embedded system context with SDL2. However, sdlSpeedometer can be parametized to launch almost any external application by means of a configuration tool invoked from the GUI.

Two marine related applications  has been integrated successfully so far:

If [OpenCPN](https://opencpn.org/wiki/dokuwiki/doku.php?id=opencpn:opencpn_user_manual:getting_started:opencpn_installation:raspberrypi_rpi2) is found in the run-time PATH, an extra launch icon will appear in the GUI.

Likewise has [zyGrib](http://www.zygrib.org) been integrated after being built from source on a Raspberry Pi Model B+

Kodi can be added as an external application to be used as a Jukebox style player togheter with its [Kore](https://play.google.com/store/apps/details?id=org.xbmc.kore&hl=sv&gl=US) remote control phone app.

sdlSpeedometer has also a built-in RFB (VNC) server function so that an external VNC client can connect a slave instrument on a computer and/or a tablet with a VNC client.

### Tested runtime environment
- Raspberry Pi 4B and a 7 inch touch display.
- NMEA Network Server (kplex) to feed the  yacht's set of instrument data running either on the Pi or accessible in the network neighborhood.

### System Software prerequisites
- xorg

### SDL2 Software prerequisites
The packages needed are:
- SDL2-2.*
- SDL2_image-2.*
- SDL2_net-2.*
- SDL2_ttf-2.*

### Library dependencies from Debian repos
- libcurl4-gnutls-dev
- i2c-tools
- libi2c-dev
- libsqlite3-dev
- libpng-dev
- libtiff5-dev
- libjpeg-dev
- libfreetype6-dev
- libts-dev
- libinput-dev
- libwebp-dev
- libvncserver-dev

### Other libraries 
- Optionally [plot-sdl](https://github.com/bertrandmartel/plot-sdl) to plot a live power shart.

### Application dependencies for running external applications from sdlSpeedometer
- dirmngr
- xterm
- xvkbd
- feh

### Software used
- [Raspberry Pi OS Lite](https://www.raspberrypi.com/software/operating-systems/#raspberry-pi-os-32-bit)

### HOWTOs
- [How to Enable i2c on the Raspberry Pi](https://www.raspberrypi-spy.co.uk/2014/11/enabling-the-i2c-interface-on-the-raspberry-pi/)
- [BerryGPS setup Guide for Raspberry Pi](http://ozzmaker.com/berrygps-setup-guide-raspberry-pi)
- [Create a Digital Compass with the Raspberry Pi](http://ozzmaker.com/compass1)

### See also
[An Open Source Yacht Glass Cockpit](https://github.com/ehedman/websocketNmea)

### Screenshots
<img src="http://hedmanshome.se/sdlspeedometer20.png" width=100%>
<img src="http://hedmanshome.se/sdlspeedometer21.png" width=100%>
<img src="http://hedmanshome.se/sdlspeedometer22.png" width=100%>
<img src="http://hedmanshome.se/sdlspeedometer23.png" width=100%>
<img src="http://hedmanshome.se/sdlspeedometer24.png" width=100%>
<img src="http://hedmanshome.se/sdlspeedometer9.png" width=100%>
<img src="http://hedmanshome.se/sdlspeedometer10.png" width=100%>
<img src="http://hedmanshome.se/sdlspeedometer27.png" width=100%>
- zyGrib just launched on a 7" touch display
<img src="http://hedmanshome.se/sdlspeedometer11.png" width=100%>
- sdlSpeedometers' configurator
<img src="http://hedmanshome.se/sdlspeedometer28.png" width=100%>
