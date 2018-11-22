# sdlSpeedometer
README Oct-2018

The sdlSpeedometer application is a marine instruemnt solution that features electronic instrument displays, typically used on private sailing yachts.
The look and feel of the visualized instruments tries to mimic the look of real physical instruments and will by design avoid a digital look.

This application is based on a Rasperry Pi and the SDL2 direct framebuffer library.

The instruments can be accessed one-by-one by clicking on the touch screen or the mouse.

The communication mechanism between this application with its GUI uses two paralell paths:
 - Data collected from a [BerryGPS-IMUv2](http://ozzmaker.com/new-products-berrygps-berrygps-imu) - GPS and 10DOF for The Raspberry Pi - Accelerometer, Gyroscope, Magnetometer and Barometric/Altitude Sensor.
 - Optionally data from a NMEA net Server such as the open source [kplex](http://www.stripydog.com/kplex/) application to drive other instrument from the yacht's network.

This instrument can work independently and always provide compass, heading, position, speed and roll even if all power fails on the yacht, if it has its own battery backup.

Currently there are five virtual instrument working (data source within brackets):

    Compass       : With heading and roll (BerryGPS-IMUv2)
    GPS           : Lo, Lat and Heading (BerryGPS-IMUv2)
    Log           : SOW, SOG (NMEA net)
    Wind          : Real, Relative and speed (NMEA net)
    Depth         : With low water warning and water temp (NMEA net)

There is also a page to perform compass calibration includning on-line fetch of declination values from [NOAA](https://www.ngdc.noaa.gov/geomag-web/calculators/calculateDeclination)

### Tested runtime environment

- Raspberry Pi - any model with Debian type OS, preferably in an embedded configuration without X and desktop stuff.
- NMEA Network Server (kplex) to feed the  yacht's set of instrument data either on the Pi or accessible in the network neighborhood.

Instructions (if needed) of how to build SDL2 libraries for framebuffer (no X) can be found in this article:
 [Hardware Accelerated SDL 2 on Raspberry Pi](http://blog.shahada.abubakar.net/post/hardware-accelerated-sdl-2-on-raspberry-pi)

### prerequisites
The packages needed are:
- SDL2-2.*
- SDL2_image-2.*
- SDL2_net-2.*
- SDL2_ttf-2.*

### Software used
- Raspbian Stretch Lite 2018-11-13


<img src="http://hedmanshome.se/sdlspeedometer.png" width=100%>
