/*
 * sdlSpeedometer.c
 *
 *  Copyright (C) 2026 by Erland Hedman <erland@hedmanshome.se>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * Desription:
 * Use the BerryGPS-IMUv2 to collect GPS and compass data to be presented
 * on an SDL2 driven framebuffer display typically on a Raspberry Pi.
 * Optionally collect nautical NMEA-0183 sentences from a NMEA network server.
 *
 * BerryGPS-IMUv2 presentation:
 *   http://ozzmaker.com/new-products-berrygps-berrygps-imu/
 *
 * SDL2 for framebuffer instructions found here:
 *   http://blog.shahada.abubakar.net/post/hardware-accelerated-sdl-2-on-raspberry-pi
 *
 */
#include <X11/Xlib.h>
#include <SDL2/SDL_syswm.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_net.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <linux/videodev2.h>
#include <libavutil/log.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <inttypes.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <curl/curl.h>
#include <termios.h>
#include <libgen.h>
#include <sys/prctl.h>
#include <time.h>
#include <math.h>
#include <syslog.h>
#include <errno.h>

#include "sdlSpeedometer.h"

#define DEF_NMEA_SERVER "http://rpi3.hedmanshome.se"   // A test site running 24/7
#define DEF_NMEA_PORT   10110
#define DEF_VNC_PORT    5903

#define TIMEDATFMT  "%x - %H:%M %Z"

#define S_TIMEOUT   4       // Invalidate current sentences after # seconds without a refresh from talker.
#define TRGPS       2.5     // Min speed to be trusted as real movement from GPS RMC
#define NMPARSE(str, nsent) !strncmp(nsent, &str[3], strlen(nsent))

#define DEFAULT_SCREEN_SIZE     "800x480"   // Default screen size
#define DEFAULT_SCREEN_SCALE    1.0

#define DEFAULT_FONT        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf";

#define TTY_GPS             "/dev/ttyAMA0"    // RPI 4/5

#ifdef REV
#define SWREV REV
#else
#define SWREV __DATE__
#endif

#define SQLCONFIG "/tmp/sqlConfig.txt"  // Internal or External inject into config db.

#ifndef PATH_INSTALL
#define SOUND_PATH  "./sounds/"
#define IMAGE_PATH  "./img/"
#define SQLDBPATH   "speedometer.db"
#define SPAWNCMD    "./spawnSubtask"
#define CONFICMD    "./sdlSpeedometer-config"
#else
#define SOUND_PATH  "/usr/local/share/sounds/"
#define IMAGE_PATH  "/usr/local/share/images/"
#define SQLDBPATH   "/usr/local/etc/speedometer/speedometer.db"
#define SPAWNCMD    "/usr/local/bin/spawnSubtask"
#define CONFICMD    "/usr/local/bin/sdlSpeedometer-config"
#endif

#define DEFAULT_BACKGROUND IMAGE_PATH "Default-bg.bmp"

#define BLACK   1
#define WHITE   2
#define RED     3

static int useSyslog = 0;

static SDL_Texture* Background_Tx;

static collected_nmea cnmea;

static warnings warn;

static void logCallBack(char *userdata, int category, SDL_LogPriority priority, const char *message)
{
    FILE *out = priority == SDL_LOG_PRIORITY_ERROR? stderr : stdout;
    category = 0;

    if (useSyslog+category)
        syslog (LOG_NOTICE, message, getuid ());
    else
        fprintf(out, "[%s] %s\n", basename(userdata), message);
}

// The configuration database
static int configureDb(configuration *configParams)
{
    sqlite3 *conn;
    sqlite3_stmt *res;
    const char *tail;
    char buf[250];
    int rval = 0;
    int bval = 0;
    struct stat sb;

    /*
      If the config database file is missing, create a template
      so that we keep the system going.
    */

    memset(&sb, 0, sizeof(struct stat));

    (void)stat(SQLDBPATH, &sb);
    rval = errno;

    if (sqlite3_open_v2(SQLDBPATH, &conn, SQLITE_OPEN_READONLY, 0) || sb.st_size == 0) {
        (void)sqlite3_close(conn);

        if (sb.st_size == 0) rval = ENOENT;

        if (!sb.st_size) {

            switch (rval) {
                case EACCES:
                    SDL_Log("Configuration database %s: ", strerror(rval));
                    return 1;
                case ENOENT:
                    SDL_Log("Configuration database does not exist. A new default database will be created");
                    (void)sqlite3_open_v2(SQLDBPATH, &conn, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE , 0);
                    if (conn == NULL) {
                            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create a new database %s: ", (char*)sqlite3_errmsg(conn));
                            return 1;
                    }

                    sqlite3_prepare_v2(conn, "CREATE TABLE config (Id INTEGER PRIMARY KEY, \
                        rev TEXT, tty TEXT, baud INTEGER, server TEXT, port INTEGER, vncport INTEGER, audiodev TEXT, camurl TEXT)", -1, &res, &tail);
                    sqlite3_step(res);

                    sprintf(buf, "INSERT INTO config (rev,tty,baud,server,port,vncport,audiodev,camurl) VALUES ('%s','%s',9600,'%s',%d,%d,'%s','%s')", SWREV,TTY_GPS, DEF_NMEA_SERVER, DEF_NMEA_PORT, DEF_VNC_PORT, "hw:0,0","rtsp://cam:campw@cam-ip/stream");
                    sqlite3_prepare_v2(conn, buf, -1, &res, &tail);
                    sqlite3_step(res);

                    sqlite3_prepare_v2(conn, "CREATE TABLE calib (Id INTEGER PRIMARY KEY, magXmax INTEGER, magYmax INTEGER, magZmax INTEGER, magXmin INTEGER, magYmin INTEGER, magZmin INTEGER, declval REAL, cOffset INTEGER, rOffset REAL)", -1, &res, &tail);
                    sqlite3_step(res);
                    sprintf(buf, "INSERT INTO calib (magXmax,magYmax,magZmax,magXmin,magYmin,magZmin,declval,cOffset,rOffset) VALUES (%d,%d,%d,%d,%d,%d,%.2f,0,0.0)", \
                        dmagXmax,dmagYmax,dmagZmax,dmagXmin,dmagYmin,dmagZmin,ddeclval);
                    sqlite3_prepare_v2(conn, buf , -1, &res, &tail);
                    sqlite3_step(res);

                    sqlite3_prepare_v2(conn, "CREATE TABLE warnings (Id INTEGER PRIMARY KEY, depthw REAL, draftw REAL, lowvoltw REAL, highcurrw REAL)", -1, &res, &tail);
                    sqlite3_step(res);
                    sprintf(buf, "INSERT INTO warnings (depthw, draftw, lowvoltw, highcurrw) VALUES(5.0, 2.1, 11.7, 9.0)");
                    sqlite3_prepare_v2(conn, buf, -1, &res, &tail);
                    sqlite3_step(res);

                    sqlite3_prepare_v2(conn, "CREATE TABLE subtasks (Id INTEGER PRIMARY KEY, task TEXT, args TEXT, icon TEXT)", -1, &res, &tail);
                    sqlite3_step(res);
                    sprintf(buf, "INSERT INTO subtasks (task,args,icon) VALUES ('opencpn','-f','opencpn')");
                    sqlite3_prepare_v2(conn, buf, -1, &res, &tail);
                    sqlite3_step(res);
                    sprintf(buf, "INSERT INTO subtasks (task,args,icon) VALUES ('sdlSpeedometer-stat','','sdlSpeedometer-stat')");
                    sqlite3_prepare_v2(conn, buf, -1, &res, &tail);
                    sqlite3_step(res);
                    sprintf(buf, "INSERT INTO subtasks (task,args,icon) VALUES ('sdlSpeedometer-browser','','sdlSpeedometer-browser')");
                    sqlite3_prepare_v2(conn, buf, -1, &res, &tail);
                    sqlite3_step(res);
                    sprintf(buf, "INSERT INTO subtasks (task,args,icon) VALUES ('XyGrib','','XyGrib')");    
                    sqlite3_prepare_v2(conn, buf, -1, &res, &tail);
                    sqlite3_step(res);
                    sprintf(buf, "INSERT INTO subtasks (task,args,icon) VALUES ('kodi-standalone','','kodi-standalone')");
                    sqlite3_prepare_v2(conn, buf, -1, &res, &tail);
                    sqlite3_step(res);
                    sprintf(buf, "INSERT INTO subtasks (task,args,icon) VALUES ('sdlSpeedometer-venus','','sdlSpeedometer-venus')");
                    sqlite3_prepare_v2(conn, buf, -1, &res, &tail);
                    sqlite3_step(res);

                    rval = sqlite3_finalize(res);
                    break;
                default:
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Configuration database initialization failed: %s", strerror(rval));
                    return 1;
            }
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to handle configuration database %s: ", (char*)sqlite3_errmsg(conn));
            (void)sqlite3_close(conn);
            return 1;
        }
    }

    // Check revision of database
    rval = sqlite3_prepare_v2(conn, "select rev from config", -1, &res, &tail);        
    if (rval == SQLITE_OK && sqlite3_step(res) == SQLITE_ROW) {
        char *ptr;
        if(strcmp((ptr=(char*)sqlite3_column_text(res, 0)), SWREV)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Warning: Database version missmatch in %s", SQLDBPATH);
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Expected %s but current revision is %s", SWREV, ptr);
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "You may have to remove %s and restart this program to get it rebuilt!", SQLDBPATH);
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "A new database will require a re-calibration of the compass");
        }
    }

    // Fetch configuration
    rval = sqlite3_prepare_v2(conn, "select tty,baud,server,port,vncport, audiodev, camurl from config", -1, &res, &tail);        
    if (rval == SQLITE_OK && sqlite3_step(res) == SQLITE_ROW) {
        strcpy(configParams->tty,       (char*)sqlite3_column_text(res, 0));
        configParams->baud =            sqlite3_column_int(res, 1);
        strcpy(configParams->server,    (char*)sqlite3_column_text(res, 2));
        configParams->port =            sqlite3_column_int(res, 3);
        configParams->vncPort =         sqlite3_column_int(res, 4);
        strcpy(configParams->snd_card,  (char*)sqlite3_column_text(res, 5));
        strcpy(configParams->cam_url,   (char*)sqlite3_column_text(res, 6));
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to fetch configutation from database: %s", (char*)sqlite3_errmsg(conn));
    }

    // Fetch warnings
    rval = sqlite3_prepare_v2(conn, "select depthw, draftw, lowvoltw,highcurrw from warnings", -1, &res, &tail);        
    if (rval == SQLITE_OK && sqlite3_step(res) == SQLITE_ROW) {
        warn.depthw    = sqlite3_column_double(res, 0);
        warn.draftw    = sqlite3_column_double(res, 1);
        warn.lowvoltw  = sqlite3_column_double(res, 2);
        warn.highcurrw = sqlite3_column_double(res, 3);
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to fetch warning values from database: %s", (char*)sqlite3_errmsg(conn));
    }

    bval = SQLITE_BUSY;
    
    while(bval == SQLITE_BUSY) { // make sure ! busy ! db locked 
        bval = SQLITE_OK;
        sqlite3_stmt * res = sqlite3_next_stmt(conn, NULL);
        if (res != NULL) {
            bval = sqlite3_finalize(res);
            if (bval == SQLITE_OK) {
                bval = sqlite3_close(conn);
            }
        }
    }
    (void)sqlite3_close(conn);

    return rval;
}

 // Extract an item from an NMEA sentence
static char *getf(int pos, char *str)
{
    int len=strlen(str);
    int i,j,k;
    int npos=0;
    static char out[200];

    memset(out,0,sizeof(out));
    pos--;

    for (i=0; i<len;i++) {
        if (str[i]==',') {
            if (npos++ == pos) {
                strcat(out, &str[++i]);
                j=strlen(out);
                for (k=0; k<j; k++) {
                    if (out[k] == ',') {
                        out[k] = '\0';
                        i=len;
                        break;
                    }
                }
            }
        }
    }

    return (out);
}

// Configure the BerryGPS-IMUv2 GPS Serial port
static int portConfigure(int fd, configuration *configParams)
{

    struct termios oldtio,newtio;
    short baud;

    tcgetattr(fd,&oldtio);    // save current serial port settings
    bzero(&newtio, sizeof(newtio));    // clear struct for new port settings

    /*
      BAUDRATE: Set bps rate. You could also use cfsetispeed and cfsetospeed.
      CRTSCTS : output hardware flow control (only used if the cable has
                all necessary lines. See sect. 7 of Serial-HOWTO)
      CS8     : 8n1 (8bit,no parity,1 stopbit)
      CLOCAL  : local connection, no modem contol
      CREAD   : enable receiving characters
    */
    switch (configParams->baud) {
        case 4800:      baud = B4800; break;
        case 9600:      baud = B9600; break;
        case 38400:     baud = B38400; break;
        case 115200:    baud = B115200; break;
        default:        baud = B9600; break;
    }

    newtio.c_cflag = baud | CRTSCTS | CS8 | CLOCAL |  CREAD;

    /*
      IGNPAR  : ignore bytes with parity errors
      ICRNL   : map CR to NL (otherwise a CR input on the other computer
                will not terminate input)
       otherwise make device raw (no other input processing)
    */
    newtio.c_iflag = IGNPAR | ICRNL;

    newtio.c_oflag = 0;    // Raw output.

    /*
      ICANON  : enable canonical input
      disable all echo functionality, and don't send signals to calling program
    */
    newtio.c_lflag = ICANON;

    /*
      initialize all control characters
      default values can be found in /usr/include/termios.h, and are given
      in the comments, but we don't need them here
    */                               
    newtio.c_cc[VTIME]    = 0;     // inter-character timer unused 
    newtio.c_cc[VMIN]     = 6;     // blocking read until 6 character arrives                                   

    (void)tcflush(fd, TCIOFLUSH);   // now clean the modem line and activate the settings for the port

    if((tcsetattr(fd,TCSANOW, &newtio)) != 0) { // Set the attributes to the termios structure
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Error in setting serial attributes!");
        return -1;
    } else {
        SDL_Log("GPS@%s: BaudRate = %d, StopBits = 1,  Parity = none", configParams->tty, configParams->baud);
    }
  
    return 0;
}

// Validate an NMEA sentence
static int nmeaChecksum(char * str_p1, char * str_p2, int cnt)
{
    uint8_t checksum;
    int i, cs;
    int debug = 0;

    cs = checksum = 0;

    for (i = 0; i < cnt; i++) {
        if (str_p1[i] == '*') cs=i+1;
        if (str_p1[i] == '\r' || str_p1[i] == '\n') {
            str_p1[i] = '\0';
            if (str_p2 !=NULL && strnlen(&str_p1[i+2], cnt-i)) {
                // There is more sentence(s) in the buffer.
                memcpy(str_p2, &str_p1[i+2], cnt-i);
            }
            break;
        }
    }

    if (cs > 0) {
        for (i=0; i < cs-1; i++) {
            if (str_p1[i] == '$' || str_p1[i] == '!') continue;
            checksum ^= (uint8_t)str_p1[i];
        }
    }  

    if ( !cs || (checksum != (uint8_t)strtol(&str_p1[cs], NULL, 16))) {
        if (debug) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Checksum error in nmea sentence: 0x%02x/0x%02x - '%s'/'%s', pos %d", \
                checksum, (uint8_t)strtol(&str_p1[cs], NULL, 16), str_p1, &str_p1[cs], cs);
        }
        return 1;
    }
    return 0;
}

// returns the true wind speed given boat speed, apparent wind speed and apparent wind direction in degrees
// https://github.com/drasgardian/truewind
static double trueWindSpeed(double boatSpeed, double apparentWindSpeed, double apparentWindDirection)
{
    // convert degres to radians
    double apparentWindDirectionRadian = apparentWindDirection * (M_PI/180);

    return pow(pow(apparentWindSpeed*cos(apparentWindDirectionRadian) - boatSpeed,2) + (pow(apparentWindSpeed*sin(apparentWindDirectionRadian),2)), 0.5);
}

// returns the true wind direction given boat speed, apparent wind speed and apparent wind direction in degrees
// https://github.com/drasgardian/truewind
static double trueWindDirection(double boatSpeed, double apparentWindSpeed, double apparentWindDirection)
{

    int convert180 = 0;
    double twdRadians;
    double apparentWindDirectionRadian;
    double twdDegrees;

    // formula below works with values < 180
    if (apparentWindDirection > 180) {
        apparentWindDirection = 360 - apparentWindDirection;
        convert180 = 1;
    }

    // convert degres to radians
    apparentWindDirectionRadian = apparentWindDirection * (M_PI/180);

    twdRadians = (90 * (M_PI/180)) - atan((apparentWindSpeed*cos(apparentWindDirectionRadian) - boatSpeed) / (apparentWindSpeed*sin(apparentWindDirectionRadian)));

    // convert radians back to degrees
    twdDegrees = twdRadians*(180/M_PI);
    if (convert180) {
        twdDegrees = 360 - twdDegrees;
    }

    return twdDegrees;
}

#define MAX_LONGITUDE 180
#define MAX_LATITUDE   90

static float dms2dd(float coordinates, const char *val)
{ // Degrees Minutes Seconds to Decimal Degrees

    // Check limits
    if ((*val == 'm') && (coordinates < 0.0 && coordinates > MAX_LATITUDE)) {
        return 0;    
    }
    if (*val == 'p' && (coordinates < 0.0 && coordinates > MAX_LONGITUDE)) {
          return 0;
    }
   int b;   //to store the degrees
   float c; //to store de decimal
 
   // Calculate the value in format nn.nnnnnn 
   b = coordinates/100;
   c= (coordinates/100 - b)*100 ;
   c /= 60;
   c += b;
   
   return c;
}

// Collect serial GPS sentences
static int threadSerial(void *conf)
{
    char buffer[512];
    configuration *configParams = conf;
    int fd;

    SDL_Log("Starting up Serial GPS collector");

    if ((fd = open(configParams->tty, O_RDONLY | O_NOCTTY)) <0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not open GPS device %s", configParams->tty);
        return 0;
    }

    if (portConfigure(fd, configParams) <0) {
        close(fd);
        return 0;
    }

    tcflush(fd, TCIOFLUSH);

    configParams->numThreads++;

    while(configParams->runGps)
    {
        time_t ct;
        int cnt;
        float hdm;
  
        // The vessels network has precedence
        if (!(time(NULL) - cnmea.net_ts > S_TIMEOUT)) {
            SDL_Delay(1000);
            continue;
        }            

        memset(buffer, 0, sizeof(buffer));

        if ((cnt=read(fd, buffer, sizeof(buffer))) <0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not read GPS device %s %s %d", configParams->tty, strerror(errno), errno);
            SDL_Delay(40);
            continue;
        }

        buffer[strlen(buffer)-1] = '\0';
        if (!strlen(buffer)) continue;

        if(nmeaChecksum(buffer, NULL, strlen(buffer))) continue;

        ct = time(NULL);    // Get a timestamp for this turn 

        // RMC - Recommended minimum specific GPS/Transit data
        if (NMPARSE(buffer, "RMC")) {
            cnmea.rmc_gps_ts = ct;
            if ((cnmea.rmc=atof(getf(7, buffer))) >= TRGPS) {   // SOG
                cnmea.rmc_ts = ct;
                if ((hdm=atof(getf(8, buffer))) != 0)  {   // Track made good
                    cnmea.hdm=hdm;
                    cnmea.hdm_ts = ct;
                }
            }
            strcpy(cnmea.gll, getf(3, buffer));
            strcpy(cnmea.glo, getf(5, buffer));
            strcpy(cnmea.glns, getf(4, buffer));
            strcpy(cnmea.glne, getf(6, buffer));
            if (cnmea.rmc_tm_set == 0) {
                strcpy(cnmea.time, getf(1, buffer));
                strcpy(cnmea.date, getf(9, buffer));
                cnmea.rmc_tm_set = 1;
            }
            if(strlen(cnmea.gll))
                cnmea.gll_ts = ct;
            continue;
        }

        // GLL - Geographic Position, Latitude / Longitude
        if (ct - cnmea.gll_ts > S_TIMEOUT/2) { // If not from RMC
            if (NMPARSE(buffer, "GLL")) {
                strcpy(cnmea.gll, getf(1, buffer));
                strcpy(cnmea.glo, getf(3, buffer));
                strcpy(cnmea.glns, getf(2, buffer));
                strcpy(cnmea.glne, getf(4, buffer));
                cnmea.gll_ts = ct;
                continue;
            }
        }

        // VTG - Track made good and ground speed
        if (ct - cnmea.rmc_ts > S_TIMEOUT/2) { // If not from RMC
            if (NMPARSE(buffer, "VTG")) {
                if ((cnmea.rmc=atof(getf(5, buffer))) >= TRGPS) {   // SOG
                    cnmea.net_ts = cnmea.rmc_ts = ct;
                    if ((hdm=atof(getf(1, buffer))) != 0) { // Track made good
                        cnmea.hdm=hdm;
                        cnmea.hdm_ts = ct;
                    }
                }
                continue;
            }
        }

        if (ct - cnmea.rmc_ts > S_TIMEOUT/2) { // If not from RMC
            // HDG - Heading - Deviation and Variation 
            if (NMPARSE(buffer, "HDG")) {
                cnmea.hdm=atof(getf(1, buffer));
                cnmea.hdm_ts = ct;
                continue;
            }
            // HDT - Heading - True (obsoleted)
            if (NMPARSE(buffer, "HDT")) {
                cnmea.hdm=atof(getf(1, buffer));
                cnmea.hdm_ts = ct;
                continue;
            }

            // HDM Heading - Heading Magnetic (obsoleted)
            if (NMPARSE(buffer, "HDM")) {
                cnmea.hdm=atof(getf(1, buffer));
                cnmea.hdm_ts = ct;
                continue;
            }
        }
    }

    close(fd);

    SDL_Log("threadSerial stopped");

    configParams->numThreads--;

    return 0;
}

// Collect Gyroscope and Magnetometer (Compass) data
static int i2cCollector(void *conf)
{
    configuration *configParams = conf;

    int retry = 0;
    int bus = 1;
    const int dt=260;
    int rval;
    int connOk = 1;
    int update = dt;
    int doUpdate = 1;
    const char *tail;
    sqlite3_stmt *res;
    calibration calib;
    struct stat sb;
    FILE *fd;

    if( (configParams->i2cFile = i2cinit(bus)) < 0) {
	    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Unable to run the i2c system!");
        configParams->i2cFile = 0;
        return 0;
    }

    SDL_Log("Starting up i2c collector");

    configParams->numThreads++;

    while(configParams->runi2c)
    {
        time_t ct;
        float hdm;

        SDL_Delay(dt);

        if (configParams->conn && connOk) {
            if (update++ > dt / 10) {
                if (!stat(SQLCONFIG, &sb)) {
                    SDL_Delay(600);
                    char sqlbuf[150];
                    memset(sqlbuf, 0, sizeof(sqlbuf));
                    SDL_Log("Got new calibration:");
                    if ((fd = fopen(SQLCONFIG, "r")) != NULL) {
                        if (fread(sqlbuf, 1, sizeof(sqlbuf), fd) > 0) {
                            SDL_Log("  %s", &sqlbuf[12]);
                            if (sqlite3_prepare_v2(configParams->conn, sqlbuf, -1,  &res, &tail)  == SQLITE_OK) {
                                if (sqlite3_step(res) != SQLITE_DONE) {
                                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to step calibration data : %s", (char*)sqlite3_errmsg(configParams->conn)); 
                                }
                                sqlite3_finalize(res);
                            } else { SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to update calibration data : %s", (char*)sqlite3_errmsg(configParams->conn)); }
                        } else {
                            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to read calibration data : %s", strerror(errno));
                        }                      
                    } else {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open calibration data file : %s", strerror(errno));
                    }
                    unlink(SQLCONFIG);
                    doUpdate = 1;
                }

                if (doUpdate) {
                    rval = sqlite3_prepare_v2(configParams->conn, "select magXmax,magYmax,magZmax,magXmin,magYmin,magZmin,declval,cOffset,rOffset from calib", -1, &res, &tail);        
                    if (rval == SQLITE_OK && sqlite3_step(res) == SQLITE_ROW) {
                        // See: BerryIMU/compass_tutorial03_calibration
                        calib.magXmax = sqlite3_column_int(res, 0);
                        calib.magYmax = sqlite3_column_int(res, 1);
                        calib.magZmax = sqlite3_column_int(res, 2);
                        calib.magXmin = sqlite3_column_int(res, 3);
                        calib.magYmin = sqlite3_column_int(res, 4);
                        calib.magZmin = sqlite3_column_int(res, 5);
                        calib.declval = sqlite3_column_double(res, 6);
                        calib.coffset = sqlite3_column_int(res, 7);
                        calib.roffset = sqlite3_column_double(res, 8);
                    } else {
                        if (connOk) {
                            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to look up calibration data - using defults : %s", \
                                (char*)sqlite3_errmsg(configParams->conn));
                        }
                        connOk = 0;
                        calib.magXmax = dmagXmax;
                        calib.magYmax = dmagYmax;
                        calib.magZmax = dmagZmax;
                        calib.magXmin = dmagXmin;
                        calib.magYmin = dmagYmin;
                        calib.magZmin = dmagZmin;
                        calib.declval = ddeclval;
                        calib.coffset = calib.roffset = 0;
                    }
                    sqlite3_finalize(res);
                    doUpdate = 0;
                }
                update = 0;
            }
        }

        ct = time(NULL);    // Get a timestamp for this turn

        if ((hdm = i2cReadHdm(configParams->i2cFile, &calib)) < 0) {
            if (retry++ > 3) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Too many read errors, giving up i2c now!");
                break;
            } else continue;
        }

        cnmea.roll_i2cts = ct;
        cnmea.roll = i2cReadRoll(configParams->i2cFile, dt, &calib);

        // Take over if no NMEA
        if (ct - cnmea.hdm_ts > S_TIMEOUT) {
            cnmea.hdm = hdm;
            cnmea.hdm_i2cts = ct;
        }
    }


    if (configParams->conn) {
        rval = SQLITE_BUSY;
        while(rval == SQLITE_BUSY) { // make sure ! busy ! db locked 
            rval = SQLITE_OK;
            sqlite3_stmt * res = sqlite3_next_stmt(configParams->conn, NULL);
            if (res != NULL) {
                rval = sqlite3_finalize(res);
                if (rval == SQLITE_OK) {
                    rval = sqlite3_close(configParams->conn);
                }
            }
        }
        (void)sqlite3_close(configParams->conn);
    }

    close(configParams->i2cFile);
    configParams->i2cFile = 0;
    configParams->conn = NULL;

    SDL_Log("i2cCollector stopped");

    configParams->numThreads--;

    return 0;
}

// Optionally collect data fron NMEA network server
static int nmeaNetCollector(void* conf)
{
    configuration *configParams = conf;
    int hostResolved = 0;
    IPaddress serverIP;                     // The IP we will connect to
    TCPsocket clientSocket = NULL;          // The socket to use
    SDLNet_SocketSet socketSet = NULL;

    SDL_Log("Starting up NMEA net collector");

    cnmea.net_ts = time(NULL);

    configParams->netStat = 0;
  
     // Initialise SDL_net
    if (SDLNet_Init() < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot  Initialise SDL_net!");
        return 0;
    }

    if ((hostResolved = SDLNet_ResolveHost(&serverIP, configParams->server, configParams->port)) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to resolve the NMEA TCP Server @ %s:%d!", configParams->server, configParams->port);
        return 0;
    }
    Uint8 * dotQuad = (Uint8*)&serverIP.host;
    SDL_Log("Successfully resolved host %s to IP: %d.%d.%d.%d : port %d\n",configParams->server, dotQuad[0],dotQuad[1],dotQuad[2],dotQuad[3], configParams->port);

    int sretry = 0;

    configParams->numThreads++;

    while(1)
    {
        int retry = 0;
        int rretry = 0;
        int activeSockets;

        // Join a socket server whenever found.
        while (!(clientSocket = SDLNet_TCP_Open(&serverIP))) {
            if (retry ++ < 3) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Try to open socket to server %s %s!", configParams->server, SDLNet_GetError());
            } else if (retry == 4) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Supressing message 'Try to open socket to ...' for now");
            }
            if (!configParams->runNet)
                break;
            SDL_Delay(10000);
        }

        // Create the socket set with enough space to store our desired number of connections (i.e. sockets)
        socketSet = SDLNet_AllocSocketSet(1);
        if (socketSet == NULL) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to allocate the socket set!");
            break;
        }

        if (!configParams->runNet)
            break;

        // Add our socket to the socket set for polling
        SDLNet_TCP_AddSocket(socketSet, clientSocket);
        activeSockets = SDLNet_CheckSockets(socketSet, 5000);

        if (activeSockets <1) {
            SDLNet_TCP_DelSocket(socketSet, clientSocket);
            SDLNet_TCP_Close(clientSocket);
            SDLNet_FreeSocketSet(socketSet);
            if (sretry ++ < 3) {
                SDL_Log("There is no socket with data at the moment");
            } else if (sretry == 4) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Supressing message 'There is no socket ...' for now");
            } 
            SDL_Delay(5000);
            continue;
        }

        SDL_Log("There are %d socket(s) with data at the moment", activeSockets);

        retry = sretry = rretry = 0;

        // Now start collect data
        while (configParams->runNet)
        {
            static char nmeastr_p1[2048];
            static char nmeastr_p2[2048];
            
            time_t ts;
            int cnt = 0;
            float hdm;

            if (++retry > 10) break;
               
            ts = time(NULL);        // Get a timestamp for this turn

            // Check if we got an NMEA response from the server
            if (SDLNet_CheckSockets(socketSet, 3000) > 0)
            {
                memset(nmeastr_p1, 0, sizeof(nmeastr_p1));

                if ((cnt=strnlen(nmeastr_p2, sizeof(nmeastr_p2)))) { // There is p2 sentence(s) to take care of.
                    memcpy(nmeastr_p1, nmeastr_p2, sizeof(nmeastr_p1));
                    memset(nmeastr_p2, 0, sizeof(nmeastr_p2));
                } else {
                    if ((cnt = SDLNet_TCP_Recv(clientSocket, nmeastr_p1, sizeof(nmeastr_p1))) <= 0) {
                        SDL_Delay(30);
                        continue;
                    }
                }

                retry = 0;
                configParams->netStat = 1;

                if (nmeaChecksum(nmeastr_p1, nmeastr_p2, cnt)) continue;

                // RMC - Recommended minimum specific GPS/Transit data
                if (NMPARSE(nmeastr_p1, "RMC")) {
                    cnmea.rmc_gps_ts = ts;
                    if ((cnmea.rmc=atof(getf(7, nmeastr_p1))) >= TRGPS) {   // SOG
                        cnmea.rmc_ts = ts;
                        if ((hdm=atof(getf(8, nmeastr_p1))) != 0)  {   // Track made good
                            cnmea.hdm=hdm;
                            cnmea.hdm_ts = ts;
                        }
                    }
                    strcpy(cnmea.gll, getf(3, nmeastr_p1));
                    strcpy(cnmea.glo, getf(5, nmeastr_p1));
                    strcpy(cnmea.glns, getf(4, nmeastr_p1));
                    strcpy(cnmea.glne, getf(6, nmeastr_p1));
                    if (cnmea.rmc_tm_set == 0) {
                        strcpy(cnmea.time, getf(1, nmeastr_p1));
                        strcpy(cnmea.date, getf(9, nmeastr_p1));
                        cnmea.rmc_tm_set = 1;
                    }
                    cnmea.net_ts = cnmea.gll_ts = ts;
                    continue;
                }

                // GLL - Geographic Position, Latitude / Longitude
                if (ts - cnmea.gll_ts > S_TIMEOUT/2) { // If not from RMC
                    if (NMPARSE(nmeastr_p1, "GLL")) {
                        strcpy(cnmea.gll, getf(1, nmeastr_p1));
                        strcpy(cnmea.glo, getf(3, nmeastr_p1));
                        strcpy(cnmea.glns, getf(2, nmeastr_p1));
                        strcpy(cnmea.glne, getf(4, nmeastr_p1));
                        cnmea.net_ts = cnmea.gll_ts = ts;
                        continue;
                    }
                }

                // VTG - Track made good and ground speed
                if (ts - cnmea.rmc_ts > S_TIMEOUT/2) { // If not from RMC
                    if (NMPARSE(nmeastr_p1, "VTG")) {
                        if ((cnmea.rmc=atof(getf(5, nmeastr_p1))) >= TRGPS) {   // SOG
                            cnmea.net_ts = cnmea.rmc_ts = ts;
                            if ((hdm=atof(getf(1, nmeastr_p1))) != 0) { // Track made good
                                cnmea.hdm=hdm;
                                cnmea.hdm_ts = ts;
                            }
                        }
                        continue;
                    }
                }

                if (ts - cnmea.rmc_ts > S_TIMEOUT/2) { // If not from RMC
                    // HDG - Heading - Deviation and Variation 
                    if (NMPARSE(nmeastr_p1, "HDG")) {
                        cnmea.hdm=atof(getf(1, nmeastr_p1));
                        cnmea.hdm += atof(getf(4, nmeastr_p1));
                        cnmea.hdm_ts = ts;
                        continue;
                    }
                    // HDT - Heading - True (obsoleted)
                    if (NMPARSE(nmeastr_p1, "HDT")) {
                        cnmea.hdm=atof(getf(1, nmeastr_p1));
                        cnmea.hdm_ts = ts;
                        continue;
                    }

                    // HDM Heading - Heading Magnetic (obsoleted)
                    if (NMPARSE(nmeastr_p1, "HDM")) {
                        cnmea.hdm=atof(getf(1, nmeastr_p1));
                        cnmea.hdm_ts = ts;
                        continue;
                    }
                }
 
                // VHW - Water speed
                if(NMPARSE(nmeastr_p1, "VHW")) {
                    if ((cnmea.stw=atof(getf(5, nmeastr_p1))) != 0)
                        cnmea.stw_ts = ts;
                    continue;
                }

                // DPT - Depth (Depth of transponder added)
                if (NMPARSE(nmeastr_p1, "DPT")) {
                    cnmea.dbt=atof(getf(1, nmeastr_p1))+atof(getf(2, nmeastr_p1));
                    cnmea.dbt_ts = ts;
                    continue;
                }

                // DBT - Depth Below Transponder
                if (ts - cnmea.dbt_ts > S_TIMEOUT/2) { // If not from DPT
                    if (NMPARSE(nmeastr_p1, "DBT")) {
                        cnmea.dbt=atof(getf(3, nmeastr_p1));
                        cnmea.dbt_ts = ts;
                        continue;
                    }
                }

                // MTW - Water temperature in C
                if (NMPARSE(nmeastr_p1, "MTW")) {
                    cnmea.mtw=atof(getf(1, nmeastr_p1));
                    cnmea.mtw_ts = ts;
                    continue;
                }

                // MWV - Wind Speed and Angle (report VWR style)
                if (NMPARSE(nmeastr_p1, "MWV")) {
                    if (strncmp(getf(2, nmeastr_p1),"R",1) + strncmp(getf(4, nmeastr_p1),"N",1) == 0) {
                        cnmea.vwra=atof(getf(1, nmeastr_p1));
                        cnmea.vwrs=atof(getf(3, nmeastr_p1))/1.94; // kn 2 m/s;
                        if (cnmea.vwra > 180) {
                            cnmea.vwrd = 1;
                            cnmea.vwra = 360 - cnmea.vwra;
                        } else cnmea.vwrd = 0;
                        cnmea.vwr_ts = ts;
                    }
                    if (strncmp(getf(2, nmeastr_p1),"T",1) + strncmp(getf(4, nmeastr_p1),"N",1) == 0) {
                        cnmea.vwta=atof(getf(1, nmeastr_p1));
                        cnmea.vwts=atof(getf(3, nmeastr_p1))/1.94; // kn 2 m/s;
                        cnmea.vwt_ts = ts;
                    } else if (ts - cnmea.stw_ts < S_TIMEOUT && cnmea.stw > 0.9) {
                            cnmea.vwta=trueWindDirection(cnmea.stw, cnmea.vwrs,  cnmea.vwra);
                            cnmea.vwts=trueWindSpeed(cnmea.stw, cnmea.vwrs, cnmea.vwra);
                            cnmea.vwt_ts = ts;
                    }
                    continue;
                }

                // VWR - Relative Wind Speed and Angle (obsolete)
                if (ts - cnmea.vwr_ts > S_TIMEOUT/2) { // If not from MWV
                    if (NMPARSE(nmeastr_p1, "VWR")) {
                        cnmea.vwra=atof(getf(1, nmeastr_p1));
                        cnmea.vwrs=atof(getf(3, nmeastr_p1))/1.94; // kn 2 m/s
                        cnmea.vwrd=strncmp(getf(2, nmeastr_p1),"R",1)==0? 0:1;
                        cnmea.vwr_ts = ts;
                        if (ts - cnmea.stw_ts < S_TIMEOUT && cnmea.stw > 0.9) {
                            cnmea.vwta=trueWindDirection(cnmea.stw, cnmea.vwrs,  cnmea.vwra);
                            cnmea.vwts=trueWindSpeed(cnmea.stw, cnmea.vwrs, cnmea.vwra);
                            cnmea.vwt_ts = ts;
                        }
                        continue;
                    }
                }

                // Format: GPENV,volt,bank,current,bank,temp,where,kWhp,kWhn,startTime*cs
                // "$P". These extended messages are not standardized. 
                if (NMPARSE(nmeastr_p1, "ENV")) {
                    cnmea.volt=         atof(getf(1, nmeastr_p1));
                    cnmea.volt_bank=    atoi(getf(2, nmeastr_p1));
                    if(cnmea.volt >=8)  cnmea.volt_ts = ts;

                    cnmea.curr=         atof(getf(3, nmeastr_p1));
                    cnmea.curr_bank=    atoi(getf(4, nmeastr_p1));
                    cnmea.curr_ts = ts;

                    cnmea.temp=         atof(getf(5, nmeastr_p1));
                    cnmea.temp_loc=     atoi(getf(6, nmeastr_p1));
                    if(cnmea.temp != 100) cnmea.temp_ts = ts;

                    cnmea.kWhp=         atof(getf(7, nmeastr_p1));
                    cnmea.kWhn=         atof(getf(8, nmeastr_p1));
                    cnmea.startTime=    atol(getf(9, nmeastr_p1));

                    continue;
                }

            } else {
                configParams->netStat = 0;
                if (rretry++ > 10)
                    break;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Retry to read socket from server %s %s!", configParams->server, SDLNet_GetError());
                SDL_Delay(1000);
            }
        }
        // Server possibly gone, try to redo all this
        SDLNet_TCP_DelSocket(socketSet, clientSocket);
        SDLNet_TCP_Close(clientSocket);
        SDLNet_FreeSocketSet(socketSet);
        configParams->netStat = 0;

        if (configParams->runNet != 0)
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Server %s possibly gone, awaiting its return", configParams->server);
    }

    if (clientSocket && socketSet) {
        SDLNet_TCP_DelSocket(socketSet, clientSocket);
        SDLNet_TCP_Close(clientSocket);
        SDLNet_FreeSocketSet(socketSet);
    }

    configParams->netStat = 0;

    SDL_Log("nmeaNetCollector stopped");
    configParams->numThreads--;

    return 0;
}


// RFB Null log
static void nullLog(void) {}

// RFB Touch events
static void vncClientTouch(int buttonMask, int x, int y, rfbClientPtr cl)
{
    SDL_Event touchEvent;
    sdl2_app *sdlApp = cl->screen->screenData;

    if (buttonMask & 1) {

        if (sdlApp->conf->subTaskPID != 0) {
            /* If a subtask is running, the VNC client will appear frozen.
             * Most likely the user will tap the screen, 
             * so let's kill the process group to re-activate SDL mode.
             */
            kill(-(sdlApp->conf->subTaskPID), SIGTERM);
            return;
        }

        touchEvent.type = SDL_FINGERDOWN;
        touchEvent.tfinger.x = (float)x/(float)sdlApp->conf->window_w;
        touchEvent.tfinger.y = (float)y/(float)sdlApp->conf->window_h;
        touchEvent.tfinger.dx = 0;
        touchEvent.tfinger.dy = 0;
        touchEvent.tfinger.pressure = 1;
        touchEvent.tfinger.timestamp = time(NULL);
        touchEvent.tfinger.touchId = 0;
        touchEvent.tfinger.fingerId = 6;
        touchEvent.user.code = 1;

        SDL_PushEvent(&touchEvent); // Add this event to the event queue. 
    }    
}

// RFB client gone
static void vncClientGone(rfbClientPtr cl)
{
    sdl2_app *sdlApp = cl->screen->screenData;
    SDL_Log("rfbProcessClient: disconnect: Client #%d", sdlApp->conf->vncClients);
    sdlApp->conf->vncClients--; 
}

// New RFB Client
static enum rfbNewClientAction vncNewclient(rfbClientPtr cl)
{
    sdl2_app *sdlApp = cl->screen->screenData;
    cl->clientData = NULL;
    cl->clientGoneHook = vncClientGone;
    sdlApp->conf->vncClients++;
    SDL_Log("rfbProcessClient: connect: Client #%d", sdlApp->conf->vncClients);
    return RFB_CLIENT_ACCEPT;
}

// RFB (VNC) Server thread
static int threadVnc(void *conf) 
{
    sdl2_app *sdlApp = conf;
    long usec;
    sdlApp->conf->vncServer->desktopName = "Live Video sdlSpeedometer";
    sdlApp->conf->vncServer->alwaysShared=(1==1);
 //   sdlApp->conf->vncServer->cursor = NULL;
    sdlApp->conf->vncServer->newClientHook = vncNewclient;
    sdlApp->conf->vncServer->screenData = sdlApp;
    sdlApp->conf->vncServer->ptrAddEvent = vncClientTouch;
    sdlApp->conf->vncServer->port = sdlApp->conf->vncPort;
    sdlApp->conf->vncServer->deferUpdateTime = 20;

    rfbInitServer(sdlApp->conf->vncServer);           

    // Loop, processing clients
    while (rfbIsActive(sdlApp->conf->vncServer))
    {
        usec = sdlApp->conf->vncServer->deferUpdateTime*1000;
        rfbProcessEvents(sdlApp->conf->vncServer, usec);
    }

    SDL_Log("RFB serivice stopped");

    return 0;
}

/*
- x, y: upper left corner.
- texture, rect: outputs.
*/
inline static void get_text_and_rect(SDL_Renderer *renderer, int x, int y, int l, char *text,
        TTF_Font *font, SDL_Texture **texture, SDL_Rect *rect, int color) 
{
    int text_width = 0;
    int text_height = 0;
    int f_width = 0;
    int f_height = 0;
    SDL_Surface *surface;
    SDL_Color textColor;

    if (text == NULL || !strlen(text))
        return;

    textColor.a = 255;

    switch (color)
    {
        case BLACK: textColor.r = textColor.g = textColor.b = 0; break;
        case WHITE: textColor.r = textColor.g = textColor.b = 255; break;
        case RED:   textColor.r = 255; textColor.g = textColor.b = 0; break;
    }

    if (color == WHITE || color == RED) {
        SDL_Color textColorB;
        textColorB.r = 0; textColorB.g = 0;  textColorB.b = 0;  textColorB.a = 0;
        if ((surface = TTF_RenderUTF8_LCD(font,text, textColor, textColorB)) != NULL) {
            Uint32 colorkey = SDL_MapRGB(surface->format, 0, 0, 0);
            SDL_SetColorKey(surface, SDL_TRUE, colorkey);
        }
    } else {
      surface = TTF_RenderText_Solid(font, text, textColor);
    }
    if (surface == NULL) return;

    *texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (*texture == NULL) {
        SDL_FreeSurface(surface);
        return;
    }

    text_width = surface->w;
    text_height = surface->h;
    SDL_FreeSurface(surface);

    // Get the width of one ch of the font used
    TTF_SizeText(font,"0", &f_width, &f_height);

    if (l >1)
        rect->x = x + abs((strlen(text)-l))*f_width/2;  // Align towards (l)
    else
       rect->x = x;

    rect->y = y;
    rect->w = text_width;
    rect->h = text_height;
}

inline static int pageSelect(sdl2_app *sdlApp, SDL_Event *event)
{
    // A simple event handler for touch screen buttons at fixed menu bar localtions
    
    int x, y;
    int win_w, win_h;
    SDL_GetWindowSize(sdlApp->window, &win_w, &win_h);

    // Upside down screen
    //x = WINDOW_W -(event->tfinger.x* win_w);
    //y = WINDOW_H -(event->tfinger.y* win_h);

    if (event->type == SDL_FINGERDOWN) {
        x = event->tfinger.x* win_w;
        y = event->tfinger.y* win_h; 
    } else if (event->type == SDL_MOUSEBUTTONDOWN) {
        x = event->button.x;
        y = event->button.y;
    } else return 0;

    x /= sdlApp->conf->scale;
    y /= sdlApp->conf->scale;

    SDL_Point p = (SDL_Point){x, y};

    SDL_Rect MutedR = {65,15,35,35};
    if (sdlApp->conf->runWrn && SDL_PointInRect(&p, &MutedR))
    {
        if (event->type == SDL_FINGERDOWN) {
            sdlApp->conf->muted = !sdlApp->conf->muted;
        }
        return 0;
    }

    SDL_Rect CalibR = {20,60,30,30};
    if (event->user.code != 1 /* not for RFB */) {
        if (sdlApp->curPage == COGPAGE && sdlApp->conf->i2cFile != 0 && SDL_PointInRect(&p, &CalibR)) {
            return CALPAGE;
        }
    }

    if (sdlApp->curPage != DPTPAGE)
        sdlApp->plotMode = 1;

    x = 403;    // Start x here and go left to right
    SDL_Rect MenuItemR = {x,400,50,50};
    if (SDL_PointInRect(&p, &MenuItemR)) { // COG
        return COGPAGE;
    }
    
    MenuItemR.x +=57;
    if (SDL_PointInRect(&p, &MenuItemR)) {  // SOG
        return SOGPAGE;
    }

    MenuItemR.x +=57;
    if (SDL_PointInRect(&p, &MenuItemR)) { // DPT
        sdlApp->plotMode = !sdlApp->plotMode;
        return DPTPAGE;
    }

    MenuItemR.x +=57;
    if (SDL_PointInRect(&p, &MenuItemR)) { // WND
        return WNDPAGE;
    }

    MenuItemR.x +=57;
    if (SDL_PointInRect(&p, &MenuItemR)) { // GPS
        return GPSPAGE;
    }

    MenuItemR.x +=57;
    if (SDL_PointInRect(&p, &MenuItemR)) { // PWR
#ifdef DIGIFLOW
        if (sdlApp->curPage == PWRPAGE)
            return WTRPAGE;
#endif
         return PWRPAGE;
    }

    MenuItemR.x +=57;
    if (SDL_PointInRect(&p, &MenuItemR)) {
        return VIDPAGE;
    }

    SDL_Rect TskPageR = {30,400,50,50};
    if (sdlApp->subAppsCmd[sdlApp->curPage][0] != NULL && event->user.code != 1 ) {
        /* not for RFB or no subtask declarations */
        if (SDL_PointInRect(&p, &TskPageR)) {
            return TSKPAGE;
        }
    }
    
    return 0;
}

inline static void addMenuItems(sdl2_app *sdlApp, TTF_Font *font)
{  
    // Add text on top of a simple menu bar

    SDL_Rect M1_rect;
    
    int x = 414;    // Start y here and go left to right

    get_text_and_rect(sdlApp->renderer, x, 416, 0, "COG", font, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &M1_rect, BLACK);
    SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &M1_rect);

    x+=57;
    get_text_and_rect(sdlApp->renderer, x, 416, 0, "SOG", font, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &M1_rect, BLACK);
    SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &M1_rect);

    x+=57;
    get_text_and_rect(sdlApp->renderer, x, 416, 0, "DPT", font, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &M1_rect, BLACK);
    SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &M1_rect);

    x+=57;
    get_text_and_rect(sdlApp->renderer, x, 416, 0, "WND", font, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &M1_rect, BLACK);
    SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &M1_rect);

    x+=57;
    get_text_and_rect(sdlApp->renderer, x, 416, 0, "GPS", font, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &M1_rect, BLACK);
    SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &M1_rect);

    x+=57;
#ifdef DIGIFLOW
    if (sdlApp->curPage == PWRPAGE) {
        get_text_and_rect(sdlApp->renderer, x, 416, 0, "WTR", font, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &M1_rect, BLACK);
        SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &M1_rect); 
    } else {
        get_text_and_rect(sdlApp->renderer, x, 416, 0, "PWR", font, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &M1_rect, BLACK);
        SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &M1_rect);
    }
#else
    get_text_and_rect(sdlApp->renderer, x, 416, 0, "PWR", font, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &M1_rect, BLACK);
    SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &M1_rect);
#endif

    x+=57;
    get_text_and_rect(sdlApp->renderer, x, 416, 0, "CAM", font, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &M1_rect, BLACK);
    SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &M1_rect);
}

static void setUTCtime(void)
{
    struct tm *settm = NULL;
    struct timeval tv;
    time_t rawtime, sys_rawtime;
    char buf[40];
    static int fails;

    tv.tv_sec = 0;
    tv.tv_usec = 0;

    if (++fails > 20) {
        // No luck - give up this
        cnmea.rmc_tm_set = 2;
        return;
    }
    // UTC of position in hhmmss.sss format + UTC date of position fix, ddmmyy format 
    if (strlen(cnmea.time) + strlen(cnmea.date) < 15) {
        cnmea.rmc_tm_set = 0;  // Try again
        return;
    }

    cnmea.rmc_tm_set = 2;      // One time only

    if (getuid()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Only root can set System UTC time from GPS. Time and date left unchanged!");
        return;
    }

    sys_rawtime = time(&rawtime);
    settm = gmtime(&rawtime);

    setenv("TZ","UTC",1);       // Temporarily set UTC

    buf[2] = '\0';

    strncpy(buf,    cnmea.time,2);
    settm->tm_hour  = atoi(buf);
    strncpy(buf,    &cnmea.time[2],2);
    settm->tm_min   = atoi(buf);
    strncpy(buf,    &cnmea.time[4],2);
    settm->tm_sec   = atoi(buf);

    strncpy(buf,    cnmea.date,2);
    settm->tm_mday  = atoi(buf);
    strncpy(buf,    &cnmea.date[2],2);
    settm->tm_mon   = atoi(buf)-1;
    strncpy(buf,    &cnmea.date[4],2);   
    settm->tm_year  = atoi(buf) +100;
    settm->tm_isdst = 0;

    // This make sense only if net (ntp) time is disabled
    if ((rawtime = mktime(settm)) != (time_t) -1) {
        if (rawtime >= sys_rawtime-10) {    // sys_rawtime-x skew tolerance
            tv.tv_sec = rawtime;
            if (settimeofday(&tv, NULL) < 0) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to set UTC system time from GPS: %s", strerror(errno));
            } else {             
                    rawtime = time(NULL);
                    strftime(buf, sizeof(buf),TIMEDATFMT, localtime(&rawtime));
                    SDL_Log("Got system time from GPS: %s", buf);
            }
        } else { // Could happen if historical data is replayed in the system
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to set UTC system time from GPS as time is moving backwards %jd seconds!", sys_rawtime-rawtime);
        }
    }
    unsetenv("TZ"); // Restore whatever zone we are in
}

// Make sure instruments take shortest route over the 0 - 360 scale
inline static float rotate(float angle, int res)
{
    float nR = angle;
    static float rot;
    float aR;
    if (res) rot = 0;

    aR = fmod(rot, 360);
    if ( aR < 0 ) { aR += 360; }
    if ( aR < 180 && (nR > (aR + 180)) ) { rot -= 360; }
    if ( aR >= 180 && (nR <= (aR - 180)) ) { rot += 360; }
    rot += (nR - aR);

    return(rot);
}

inline static float rotate_a(float angle, int res)
{
    float nR = angle;
    static float rot;
    float aR;
    if (res) rot = 0;

    aR = fmod(rot, 360);
    if ( aR < 0 ) { aR += 360; }
    if ( aR < 180 && (nR > (aR + 180)) ) { rot -= 360; }
    if ( aR >= 180 && (nR <= (aR - 180)) ) { rot += 360; }
    rot += (nR - aR);

    return(rot);
}

// Play audible warning message
static void playWarnSound(char *snd_card_name, char *wavFile)
{

    SDL_AudioSpec wavSpec;
    Uint32 wavLength;
    Uint8 *wavBuffer;
    SDL_AudioDeviceID deviceId;
    char soundPath[4096];
    strcpy(soundPath, SOUND_PATH);
    strncat(soundPath, wavFile, sizeof(soundPath)-1);

    // load WAV file
    if (SDL_LoadWAV(soundPath, &wavSpec, &wavBuffer, &wavLength) == NULL)
        return;

    if ((deviceId = SDL_OpenAudioDevice(snd_card_name, 0, &wavSpec, NULL, 0)) == 0) { 
        if ((deviceId = SDL_OpenAudioDevice(NULL, 0, &wavSpec, NULL, 0)) == 0) { // Open default
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to SDL_OpenAudioDevice %s: %s", snd_card_name, SDL_GetError()); 
            return;
       }
    }

    // play audio
    if (SDL_QueueAudio(deviceId, wavBuffer, wavLength) == 0) {
        SDL_PauseAudioDevice(deviceId, 0);
        SDL_Delay(2200);
    }

    // Clean up
    SDL_CloseAudioDevice(deviceId);
    SDL_FreeWAV(wavBuffer);
}

// Check for warnings and play sound files periodically
static int threadWarn(void *conf)
{
    configuration *configParams = conf;
    time_t ct;

    configParams->numThreads++; 

    while(configParams->runWrn) {

        if (configParams->muted == 1) {
            SDL_Delay(1000);
            continue;
        }

        ct = time(NULL);    // Get a timestamp for this turn

        if (!(ct - cnmea.dbt_ts > S_TIMEOUT) && cnmea.dbt <= warn.depthw) {
            playWarnSound(configParams->snd_card_name, "shallow-water.wav");
            SDL_Delay(3000);
        }

        if (!(ct - cnmea.curr_ts > S_TIMEOUT) && cnmea.curr <= -warn.highcurrw && configParams->runWrn) { 
            playWarnSound(configParams->snd_card_name, "current-high.wav");
            SDL_Delay(2000);
        }

        if (!(ct - cnmea.volt_ts > S_TIMEOUT) && cnmea.volt <= warn.lowvoltw && configParams->runWrn) { 
            playWarnSound(configParams->snd_card_name, "low-voltage.wav");
            SDL_Delay(2000);
        }

        if (configParams->runWrn) SDL_Delay(2000);

    }

    SDL_Log("Sound Server stopped");

    configParams->numThreads--;

    return 0;
}

inline static void doVnc(sdl2_app *sdlApp)
{
    if (sdlApp->conf->runVnc && sdlApp->conf->vncClients && sdlApp->conf->vncPixelBuffer)
    {
        SDL_RenderReadPixels(
            sdlApp->renderer,
            NULL,
            SDL_PIXELFORMAT_BGR888,
            sdlApp->conf->vncPixelBuffer->pixels,
            sdlApp->conf->vncPixelBuffer->pitch
        );

        rfbMarkRectAsModified(
            sdlApp->conf->vncServer,
            0, 0,
            sdlApp->conf->window_w,
            sdlApp->conf->window_h
        );
    }
}

// Present the compass with heading ant roll
static int doCompass(sdl2_app *sdlApp)
{
    SDL_Event e;
    SDL_Rect compassR, outerRingR, clinoMeterR, windDirR, menuBarR, subTaskbarR, netStatbarR, mutebarR, calbarR, textBoxR;
    TTF_Font* fontCog = TTF_OpenFont(sdlApp->fontPath, 42);
    TTF_Font* fontRoll = TTF_OpenFont(sdlApp->fontPath, 22);
    TTF_Font* fontSrc = TTF_OpenFont(sdlApp->fontPath, 14);
    TTF_Font* fontTod = TTF_OpenFont(sdlApp->fontPath, 16);

    SDL_Texture* compassRose = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "compassRose.png");
    SDL_Texture* outerRing = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "outerRing.png");
    SDL_Texture* windDir = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "windDir.png");
    SDL_Texture* menuBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "menuBar.png");
    SDL_Texture* netStatBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "netStat.png");
    SDL_Texture* noNetStatbar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "noNetStat.png");
    SDL_Texture* muteBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "mute.png");
    SDL_Texture* unmuteBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "unmute.png");
    SDL_Texture* textBox = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "textBox.png");
    SDL_Texture* clinoMeter = NULL;
    SDL_Texture* calBar = NULL;
    SDL_Texture* subTaskbar = NULL;

    if (sdlApp->conf->i2cFile != 0) {
        calBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "cal.png");
        clinoMeter = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "clinometer.png");
    }

    sdlApp->curPage = COGPAGE;

    if (sdlApp->subAppsCmd[sdlApp->curPage][0] != NULL) {
        char icon[PATH_MAX];
        sprintf(icon , "%s/%s.png", IMAGE_PATH, sdlApp->subAppsIco[sdlApp->curPage][0]);
        if ((subTaskbar = IMG_LoadTexture(sdlApp->renderer, icon)) == NULL)
            subTaskbar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "tool.png");           
    }

    compassR.w = 372;
    compassR.h = 372;
    compassR.x = 54;
    compassR.y = 52;

    outerRingR.w = 440;
    outerRingR.h = 440;
    outerRingR.x = 19;
    outerRingR.y = 18;

    clinoMeterR.w = 136;
    clinoMeterR.h = 136;
    clinoMeterR.x = 171;
    clinoMeterR.y = 178;

    menuBarR.w = 393;
    menuBarR.h = 50;
    menuBarR.x = 400;
    menuBarR.y = 400;

    subTaskbarR.w = 50;
    subTaskbarR.h = 50;
    subTaskbarR.x = 30;
    subTaskbarR.y = 400;

    netStatbarR.w = 25;
    netStatbarR.h = 25;
    netStatbarR.x = 20;
    netStatbarR.y = 20;

    mutebarR.w = 25;
    mutebarR.h = 25;
    mutebarR.x = 70;
    mutebarR.y = 20;

    calbarR.w = 25;
    calbarR.h = 25;
    calbarR.x = 20;
    calbarR.y = 60;

    textBoxR.w = 290;
    textBoxR.h = 42;
    textBoxR.x = 470;
    textBoxR.y = 106;

    windDirR.w = 240;
    windDirR.h = 240;
    windDirR.x = 120;
    windDirR.y = 122;

    SDL_Rect textField_rect = {0,0,0,0};

    float t_angle = 0;
    float angle = 0;
    float t_angle_a = 0;
    float angle_a = 0;
    float t_roll = 0;
    float roll = 0;
    int res = 1;
    int res_a = 1;
    int boxItems[] = {120,170,220,270};
    float dynUpd;

    const float offset = 131; // For scale

    while (1) {
        int boxItem = 0;
        sdlApp->textFieldArrIndx = 0;
        char msg_hdm[40] = { "-" };
        char msg_rll[40] = { "" };
        char msg_sog[40] = { "" };
        char msg_stw[40] = { "" };
        char msg_dbt[40] = { "" };
        char msg_mtw[40] = { "" };
        char msg_src[40] = { "" };
        char msg_tod[40];
        time_t ct;

        int doBreak = 0;
        
        while (SDL_PollEvent(&e)) {

            if (e.type == SDL_QUIT) {
                doBreak = 1;
                break;
            }

            if (e.type == SDL_FINGERDOWN || e.type == SDL_MOUSEBUTTONDOWN)
            {
                if ((e.type=pageSelect(sdlApp, &e))) {
                    doBreak = 1;
                    if (e.type == TSKPAGE && subTaskbar != NULL) {
                        SDL_SetTextureColorMod(subTaskbar, 128, 128, 128);
                        SDL_RenderCopyEx(sdlApp->renderer, subTaskbar, NULL, &subTaskbarR, 0, NULL, SDL_FLIP_NONE);
                        SDL_RenderPresent(sdlApp->renderer);
                    }
                    break;
                }
            }
        }
        if (doBreak == 1) break;

        ct = time(NULL);    // Get a timestamp for this turn 

        if (!(ct - cnmea.rmc_gps_ts > S_TIMEOUT)) {
            // Set system UTC time
            if (cnmea.rmc_tm_set == 1)
                setUTCtime();
        }

        strftime(msg_tod, sizeof(msg_tod),TIMEDATFMT, localtime(&ct));

        // Magnetic/Net or GPS HDM
        if (!(ct - cnmea.hdm_i2cts > S_TIMEOUT)) {
            sprintf(msg_hdm, "%.0f", cnmea.hdm);
            sprintf(msg_src, "mag");
        } else {
            sprintf(msg_hdm, "%.0f", cnmea.hdm);
            if (!( ct - cnmea.net_ts > S_TIMEOUT))
                sprintf(msg_src, "net");
            else
                sprintf(msg_src, "gps");
        }

        // VHW - Water speed
        if (!(ct - cnmea.stw_ts > S_TIMEOUT))
            sprintf(msg_stw, "STW: %.1f", cnmea.stw);

        // Magnetic Roll
        if (!(ct - cnmea.roll_i2cts > S_TIMEOUT)) {
            sprintf(msg_rll, "%.0f", fabs(roll=cnmea.roll));
            if (roll > t_roll) t_roll += 0.8 * (fabsf(roll -t_roll) / 10);
            else if (roll < t_roll) t_roll -= 0.8 * (fabsf(roll -t_roll) / 10);
        }

        // RMC - Recommended minimum specific GPS/Transit data
        if (!(ct - cnmea.rmc_ts > S_TIMEOUT))
            sprintf(msg_sog, "SOG: %.1f", cnmea.rmc);

        // DBT - Depth Below Transponder
        if (!(ct - cnmea.dbt_ts > S_TIMEOUT))
            sprintf(msg_dbt, cnmea.dbt > 70.0? "DBT: %.0f" : "DBT: %.1f", cnmea.dbt);

        // WND - Relative wind speed in m/s
        if (!(ct - cnmea.vwr_ts > S_TIMEOUT))
            sprintf(msg_mtw, "WND: %.1f", cnmea.vwrs);

        angle = rotate(roundf(cnmea.hdm), res); res=0;

        // Run needle and roll with smooth acceleration
        if (angle > t_angle) t_angle += 0.8 * (fabsf(angle -t_angle) / 24);
        else if (angle < t_angle) t_angle -= 0.8 * (fabsf(angle -t_angle) / 24);
     
        angle_a = cnmea.vwra; // 0-180

        if (cnmea.vwrd == 1) angle_a = 360 - angle_a; // Mirror the needle motion
        angle_a += offset;

        t_angle_a = round(rotate_a(angle_a, res_a)); res_a=0;

        SDL_SetRenderDrawColor(sdlApp->renderer, 0, 0, 0, 255);
        SDL_RenderClear(sdlApp->renderer);

        SDL_RenderCopy(sdlApp->renderer, Background_Tx, NULL, NULL);
        SDL_RenderCopyEx(sdlApp->renderer, outerRing, NULL, &outerRingR, 0, NULL, SDL_FLIP_NONE);
        SDL_RenderCopyEx(sdlApp->renderer, compassRose, NULL, &compassR, 360-t_angle, NULL, SDL_FLIP_NONE);

        if (!(ct - cnmea.roll_i2cts > S_TIMEOUT))
            SDL_RenderCopyEx(sdlApp->renderer, clinoMeter, NULL, &clinoMeterR, t_roll, NULL, SDL_FLIP_NONE);

        if (!(ct - cnmea.vwr_ts > S_TIMEOUT || cnmea.vwra == 0))
            SDL_RenderCopyEx(sdlApp->renderer, windDir, NULL, &windDirR, t_angle_a, NULL, SDL_FLIP_NONE);

        get_text_and_rect(sdlApp->renderer, 226, 180, 3, msg_src, fontSrc, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);
        SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
    
        get_text_and_rect(sdlApp->renderer, 200, 200, 3, msg_hdm, fontCog, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);
        SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);

        if (!(ct - cnmea.roll_i2cts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 224, 248, 2, msg_rll, fontRoll, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
        }

        if (!(ct - cnmea.stw_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_stw, fontCog, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
        }

        if (!(ct - cnmea.rmc_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_sog, fontCog, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
        }

        if (!(ct - cnmea.dbt_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_dbt, fontCog, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, cnmea.dbt <= warn.depthw? RED : WHITE);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
        }

        if (!(ct - cnmea.vwr_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_mtw, fontCog, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
        }

        get_text_and_rect(sdlApp->renderer, 580, 10, 0, msg_tod, fontTod, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
        SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);

        if (subTaskbar != NULL) {
            SDL_RenderCopyEx(sdlApp->renderer, subTaskbar, NULL, &subTaskbarR, 0, NULL, SDL_FLIP_NONE);
        }

        if (sdlApp->conf->netStat == 1) {
            SDL_RenderCopyEx(sdlApp->renderer, netStatBar, NULL, &netStatbarR, 0, NULL, SDL_FLIP_NONE);
        } else {
            SDL_RenderCopyEx(sdlApp->renderer, noNetStatbar, NULL, &netStatbarR, 0, NULL, SDL_FLIP_NONE);
        }

        if (sdlApp->conf->runWrn) {
            if (sdlApp->conf->muted == 0) {
                SDL_RenderCopyEx(sdlApp->renderer, muteBar, NULL, &mutebarR, 0, NULL, SDL_FLIP_NONE);
            } else {
                SDL_RenderCopyEx(sdlApp->renderer, unmuteBar, NULL, &mutebarR, 0, NULL, SDL_FLIP_NONE);
            }
        }

        if (sdlApp->conf->i2cFile != 0) {
            SDL_RenderCopyEx(sdlApp->renderer, calBar, NULL, &calbarR, 0, NULL, SDL_FLIP_NONE);
        }

        SDL_RenderCopyEx(sdlApp->renderer, menuBar, NULL, &menuBarR, 0, NULL, SDL_FLIP_NONE);
        addMenuItems(sdlApp, fontSrc);

        if (boxItem) {
            textBoxR.h = boxItem*50 +30;
            SDL_RenderCopyEx(sdlApp->renderer, textBox, NULL, &textBoxR, 0, NULL, SDL_FLIP_NONE);
        }

        SDL_RenderPresent(sdlApp->renderer);

        doVnc(sdlApp);

        // Reduce CPU load if only short scale movements
        dynUpd = (1/fabsf(angle -t_angle))*200;
        dynUpd = dynUpd > 200? 200:dynUpd;

        SDL_Delay(30+(int)dynUpd);

        sdlApp->textFieldArrIndx--;
        do {
            SDL_DestroyTexture(sdlApp->textFieldArr[sdlApp->textFieldArrIndx]);
        } while (sdlApp->textFieldArrIndx-- >0);

    }

    if (subTaskbar != NULL)
        SDL_DestroyTexture(subTaskbar);
    if (clinoMeter != NULL)
        SDL_DestroyTexture(clinoMeter);
    if (calBar != NULL)
        SDL_DestroyTexture(calBar);

    SDL_DestroyTexture(compassRose);
    SDL_DestroyTexture(outerRing);
    SDL_DestroyTexture(windDir);
    SDL_DestroyTexture(menuBar);
    SDL_DestroyTexture(netStatBar);
    SDL_DestroyTexture(noNetStatbar);
    SDL_DestroyTexture(muteBar);
    SDL_DestroyTexture(unmuteBar);  
    SDL_DestroyTexture(textBox);
    TTF_CloseFont(fontCog);
    TTF_CloseFont(fontRoll);
    TTF_CloseFont(fontSrc);
    TTF_CloseFont(fontTod);

    return e.type;
}

// Present the Sumlog (NMEA net only)
static int doSumlog(sdl2_app *sdlApp)
{
    SDL_Event e;
    SDL_Rect gaugeR, needleR, menuBarR, subTaskbarR, netStatbarR, mutebarR, textBoxR;
    TTF_Font* fontLarge =  TTF_OpenFont(sdlApp->fontPath, 46);
    TTF_Font* fontSmall =  TTF_OpenFont(sdlApp->fontPath, 20);
    TTF_Font* fontCog = TTF_OpenFont(sdlApp->fontPath, 42);
    TTF_Font* fontSrc = TTF_OpenFont(sdlApp->fontPath, 14);
    TTF_Font* fontTod = TTF_OpenFont(sdlApp->fontPath, 16);

    SDL_Texture* gaugeSumlog = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "sumlog.png");
    SDL_Texture* gaugeNeedleApp = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "needle.png");
    SDL_Texture* menuBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "menuBar.png");
    SDL_Texture* netStatBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "netStat.png");
    SDL_Texture* noNetStatbar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "noNetStat.png");
    SDL_Texture* textBox = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "textBox.png");
    SDL_Texture* muteBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "mute.png");
    SDL_Texture* unmuteBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "unmute.png");

    gaugeR.w = 440;
    gaugeR.h = 440;
    gaugeR.x = 19;
    gaugeR.y = 18;

    needleR.w = 240;
    needleR.h = 240;
    needleR.x = 120;
    needleR.y = 122;

    menuBarR.w = 393;
    menuBarR.h = 50;
    menuBarR.x = 400;
    menuBarR.y = 400;

    subTaskbarR.w = 50;
    subTaskbarR.h = 50;
    subTaskbarR.x = 30;
    subTaskbarR.y = 400;

    netStatbarR.w = 25;
    netStatbarR.h = 25;
    netStatbarR.x = 20;
    netStatbarR.y = 20;

    mutebarR.w = 25;
    mutebarR.h = 25;
    mutebarR.x = 70;
    mutebarR.y = 20;

    textBoxR.w = 290;
    textBoxR.h = 42;
    textBoxR.x = 470;
    textBoxR.y = 106;

    SDL_Texture* subTaskbar = NULL;

    sdlApp->curPage = SOGPAGE;

    if (sdlApp->subAppsCmd[sdlApp->curPage][0] != NULL) {
        char icon[PATH_MAX];
        sprintf(icon , "%s/%s.png", IMAGE_PATH, sdlApp->subAppsIco[sdlApp->curPage][0]);
        if ((subTaskbar = IMG_LoadTexture(sdlApp->renderer, icon)) == NULL)
            subTaskbar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "tool.png");           
    }

    SDL_Rect textField_rect;

    float t_angle = 0;
    float angle = 0;
    int boxItems[] = {120,170,220};
    float dynUpd;

    while (1) {
        int boxItem = 0;
        sdlApp->textFieldArrIndx = 0;
        char msg_stw[40];
        char msg_sog[40];
        char msg_dbt[40] = { "" };
        char msg_mtw[40] = { "" };
        char msg_hdm[40] = { "" };
        float speed, wspeed;
        int stw;
        char msg_tod[40];
        time_t ct;

        // Constants for instrument
        const float minangle = 13;  // Scale start
        const float maxangle = 237; // Scale end
        const float maxspeed = 10;

        int doBreak = 0;
        
        while (SDL_PollEvent(&e)) {

            if (e.type == SDL_QUIT) {
                doBreak = 1;
                break;
            }

            if (e.type == SDL_FINGERDOWN || e.type == SDL_MOUSEBUTTONDOWN)
            {
                if ((e.type=pageSelect(sdlApp, &e))) {
                    doBreak = 1;
                    if (e.type == TSKPAGE && subTaskbar != NULL) {
                        SDL_SetTextureColorMod(subTaskbar, 128, 128, 128);
                        SDL_RenderCopyEx(sdlApp->renderer, subTaskbar, NULL, &subTaskbarR, 0, NULL, SDL_FLIP_NONE);
                        SDL_RenderPresent(sdlApp->renderer);
                    }
                    break;
                }
            }
        }
        if (doBreak == 1) break;

        SDL_RenderClear(sdlApp->renderer);

        ct = time(NULL);    // Get a timestamp for this turn
        strftime(msg_tod, sizeof(msg_tod), TIMEDATFMT, localtime(&ct));

        // VHW - Water speed and Heading
         if (ct - cnmea.stw_ts > S_TIMEOUT) {
            sprintf(msg_stw, "----");
            wspeed = 0.0;
            stw = 0;
        } else {
            sprintf(msg_stw, "%.2f", cnmea.stw);
            wspeed = cnmea.stw;
            stw = 1;
        }

        // RMC - Recommended minimum specific GPS/Transit data
        if (ct - cnmea.rmc_ts > S_TIMEOUT)
            sprintf(msg_sog, "----");
        else {
            sprintf(msg_sog, "SOG:%.2f", cnmea.rmc);
            if (wspeed == 0.0) {
                wspeed = cnmea.rmc;
                sprintf(msg_stw, "%.2f", cnmea.rmc);
            }
        }
       
        // Heading
        if (!(ct - cnmea.hdm_ts > S_TIMEOUT))
            sprintf(msg_hdm, "COG: %.0f", cnmea.hdm);
        
        // DBT - Depth Below Transponder
        if (!(ct - cnmea.dbt_ts > S_TIMEOUT))
            sprintf(msg_dbt, cnmea.dbt > 70.0? "DBT: %.0f" : "DBT: %.1f", cnmea.dbt);

        // WND - Relative wind speed in m/s
        if (!(ct - cnmea.vwr_ts > S_TIMEOUT))
            sprintf(msg_mtw, "WND: %.1f", cnmea.vwrs);
                         
        speed = wspeed * (maxangle/maxspeed);
        angle = roundf(speed+minangle);

        // Run needle with smooth acceleration
        if (angle > t_angle) t_angle += 3.2 * (fabsf(angle -t_angle) / 24) ;
        else if (angle < t_angle) t_angle -= 3.2 * (fabsf(angle -t_angle) / 24);

        SDL_RenderCopy(sdlApp->renderer, Background_Tx, NULL, NULL);
       
        SDL_RenderCopyEx(sdlApp->renderer, gaugeSumlog, NULL, &gaugeR, 0, NULL, SDL_FLIP_NONE);

        if (wspeed)
            SDL_RenderCopyEx(sdlApp->renderer, gaugeNeedleApp, NULL, &needleR, t_angle, NULL, SDL_FLIP_NONE);

        get_text_and_rect(sdlApp->renderer, 182, 300, 4, msg_stw, fontLarge, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);
        SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);

        if (!(ct - cnmea.hdm_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_hdm, fontCog, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
        }

        if (!(ct - cnmea.dbt_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_dbt, fontCog, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, cnmea.dbt <= warn.depthw? RED : WHITE);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
        }

        if (!(ct - cnmea.vwr_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_mtw, fontCog, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
        }

        SDL_RenderCopyEx(sdlApp->renderer, menuBar, NULL, &menuBarR, 0, NULL, SDL_FLIP_NONE);
        addMenuItems(sdlApp, fontSrc);

        get_text_and_rect(sdlApp->renderer, 580, 10, 0, msg_tod, fontTod, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
        SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);

         if (stw) {
            get_text_and_rect(sdlApp->renderer, 186, 366, 8, msg_sog, fontSmall, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);       
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
        }

        if (subTaskbar != NULL) {
            SDL_RenderCopyEx(sdlApp->renderer, subTaskbar, NULL, &subTaskbarR, 0, NULL, SDL_FLIP_NONE);
        }

        if (sdlApp->conf->netStat == 1) {
           SDL_RenderCopyEx(sdlApp->renderer, netStatBar, NULL, &netStatbarR, 0, NULL, SDL_FLIP_NONE);
        } else {
            SDL_RenderCopyEx(sdlApp->renderer, noNetStatbar, NULL, &netStatbarR, 0, NULL, SDL_FLIP_NONE);
        }

        if (sdlApp->conf->runWrn) {
            if (sdlApp->conf->muted == 0) {
                SDL_RenderCopyEx(sdlApp->renderer, muteBar, NULL, &mutebarR, 0, NULL, SDL_FLIP_NONE);
            } else {
                SDL_RenderCopyEx(sdlApp->renderer, unmuteBar, NULL, &mutebarR, 0, NULL, SDL_FLIP_NONE);
            }
        }

        if (boxItem) {
            textBoxR.h = boxItem*50 +30;
            SDL_RenderCopyEx(sdlApp->renderer, textBox, NULL, &textBoxR, 0, NULL, SDL_FLIP_NONE);
        }

        SDL_RenderPresent(sdlApp->renderer); 

        doVnc(sdlApp);

        // Reduce CPU load if only short scale movements
        dynUpd = (1/fabsf(angle -t_angle))*200;
        dynUpd = dynUpd > 200? 200:dynUpd;

        SDL_Delay(30+(int)dynUpd);

        sdlApp->textFieldArrIndx--;
        do {
            SDL_DestroyTexture(sdlApp->textFieldArr[sdlApp->textFieldArrIndx]);
        } while (sdlApp->textFieldArrIndx-- >0);
    }

    if (subTaskbar != NULL) {
        SDL_DestroyTexture(subTaskbar);
    }

    SDL_DestroyTexture(gaugeSumlog);
    SDL_DestroyTexture(gaugeNeedleApp);
    SDL_DestroyTexture(menuBar);
    SDL_DestroyTexture(netStatBar);
    SDL_DestroyTexture(noNetStatbar);
    SDL_DestroyTexture(muteBar);
    SDL_DestroyTexture(unmuteBar);
    SDL_DestroyTexture(textBox);
    TTF_CloseFont(fontLarge);
    TTF_CloseFont(fontSmall); 
    TTF_CloseFont(fontCog);
    TTF_CloseFont(fontSrc);
    TTF_CloseFont(fontTod);
  
    return e.type;
}

// Present GPS data
static int doGps(sdl2_app *sdlApp)
{
    SDL_Event e;
    SDL_Rect gaugeR, menuBarR, subTaskbarR, netStatbarR, mutebarR, textBoxR;

    TTF_Font* fontHD =  TTF_OpenFont(sdlApp->fontPath, 40);
    TTF_Font* fontLA =  TTF_OpenFont(sdlApp->fontPath, 30);
    TTF_Font* fontLO =  TTF_OpenFont(sdlApp->fontPath, 30);
    TTF_Font* fontMG =  TTF_OpenFont(sdlApp->fontPath, 14);
    TTF_Font* fontCog = TTF_OpenFont(sdlApp->fontPath, 42);
    TTF_Font* fontSrc = TTF_OpenFont(sdlApp->fontPath, 14);
    TTF_Font* fontTod = TTF_OpenFont(sdlApp->fontPath, 16);

    SDL_Texture* gaugeGps = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "gps.png");
    SDL_Texture* menuBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "menuBar.png");
    SDL_Texture* netStatBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "netStat.png");
    SDL_Texture* noNetStatbar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "noNetStat.png");
    SDL_Texture* muteBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "mute.png");
    SDL_Texture* unmuteBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "unmute.png");
    SDL_Texture* textBox = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "textBox.png");
        
    SDL_Texture* subTaskbar = NULL;

    sdlApp->curPage = GPSPAGE;

    if (sdlApp->subAppsCmd[sdlApp->curPage][0] != NULL) {
        char icon[PATH_MAX];
        sprintf(icon , "%s/%s.png", IMAGE_PATH, sdlApp->subAppsIco[sdlApp->curPage][0]);
        if ((subTaskbar = IMG_LoadTexture(sdlApp->renderer, icon)) == NULL)
            subTaskbar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "tool.png");           
    }

    SDL_Rect textField_rect;

    gaugeR.w = 440;
    gaugeR.h = 440;
    gaugeR.x = 19;
    gaugeR.y = 18;

    menuBarR.w = 393;
    menuBarR.h = 50;
    menuBarR.x = 400;
    menuBarR.y = 400;

    subTaskbarR.w = 50;
    subTaskbarR.h = 50;
    subTaskbarR.x = 30;
    subTaskbarR.y = 400;

    netStatbarR.w = 25;
    netStatbarR.h = 25;
    netStatbarR.x = 20;
    netStatbarR.y = 20;

    mutebarR.w = 25;
    mutebarR.h = 25;
    mutebarR.x = 70;
    mutebarR.y = 20;

    textBoxR.w = 290;
    textBoxR.h = 42;
    textBoxR.x = 470;
    textBoxR.y = 106;

    int boxItems[] = {120,170,220,270};

    while (1) {
        int boxItem = 0;
        sdlApp->textFieldArrIndx = 0;
        char msg_hdm[40];
        char msg_lat[40];
        char msg_lot[40];
        char msg_src[40];
        char msg_dbt[40] = { "" };
        char msg_mtw[40] = { "" };
        char msg_sog[40] = { "" };
        char msg_stw[40] = { "" };
        char msg_tod[40];
        time_t ct;

        int doBreak = 0;
        
        while (SDL_PollEvent(&e)) {

            if (e.type == SDL_QUIT) {
                doBreak = 1;
                break;
            }

            if (e.type == SDL_FINGERDOWN || e.type == SDL_MOUSEBUTTONDOWN)
            {
                if ((e.type=pageSelect(sdlApp, &e))) {
                    doBreak = 1;
                    if (e.type == TSKPAGE && subTaskbar != NULL) {
                        SDL_SetTextureColorMod(subTaskbar, 128, 128, 128);
                        SDL_RenderCopyEx(sdlApp->renderer, subTaskbar, NULL, &subTaskbarR, 0, NULL, SDL_FLIP_NONE);
                        SDL_RenderPresent(sdlApp->renderer);
                    }
                    break;
                }
            }
        }
        if (doBreak == 1) break;

        SDL_RenderClear(sdlApp->renderer);

        ct = time(NULL);    // Get a timestamp for this turn 
        strftime(msg_tod, sizeof(msg_tod),TIMEDATFMT, gmtime(&ct)); // Here we expose GMT/UTC time

        sprintf(msg_src, "  ");

        // RMC - Recommended minimum specific GPS/Transit data
         if (ct - cnmea.gll_ts > S_TIMEOUT) {
            sprintf(msg_hdm, "----");
            sprintf(msg_lat, "----");
            sprintf(msg_lot, "----");
        } else {
            sprintf(msg_hdm, "%.0f",  cnmea.hdm);
            sprintf(msg_lat, "%.4f%s", dms2dd(atof(cnmea.gll),"m"), cnmea.glns);
            sprintf(msg_lot, "%.4f%s", dms2dd(atof(cnmea.glo),"m"), cnmea.glne);
            if (!(ct - cnmea.hdm_i2cts > S_TIMEOUT)) {
                sprintf(msg_src, "mag");
            } else if (!( ct - cnmea.net_ts > S_TIMEOUT))
                sprintf(msg_src, "net");
            else
                sprintf(msg_src, "gps");
         }

        // RMC - Recommended minimum specific GPS/Transit data
        if (!(ct - cnmea.rmc_ts > S_TIMEOUT))
            sprintf(msg_sog, "SOG: %.1f", cnmea.rmc);

        // VHW - Water speed and Heading
         if (!(ct - cnmea.stw_ts > S_TIMEOUT))
            sprintf(msg_stw, "STW: %.1f", cnmea.stw);
        
        // WND - Relative wind speed in m/s
        if (!(ct - cnmea.vwr_ts > S_TIMEOUT))
            sprintf(msg_mtw, "WND: %.1f", cnmea.vwrs);

        // DBT - Depth Below Transponder
        if (!(ct - cnmea.dbt_ts > S_TIMEOUT))
            sprintf(msg_dbt, cnmea.dbt > 70.0? "DBT: %.0f" : "DBT: %.1f", cnmea.dbt);
        
        SDL_RenderCopy(sdlApp->renderer, Background_Tx, NULL, NULL);
       
        SDL_RenderCopyEx(sdlApp->renderer, gaugeGps, NULL, &gaugeR, 0, NULL, SDL_FLIP_NONE);

        get_text_and_rect(sdlApp->renderer, 190, 142, 3, msg_hdm, fontHD, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);
        SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);

        get_text_and_rect(sdlApp->renderer, 290, 168, 1, msg_src, fontMG, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);
        SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);

        get_text_and_rect(sdlApp->renderer, 148, 222, 9, msg_lat, fontLA, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);
        SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);

        get_text_and_rect(sdlApp->renderer, 148, 292, 9, msg_lot, fontLO, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);
        SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
       
        if (!(ct - cnmea.stw_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_stw, fontCog, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
        }

         if (!(ct - cnmea.rmc_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_sog, fontCog, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
        }

         if (!(ct - cnmea.dbt_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_dbt, fontCog, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, cnmea.dbt <= warn.depthw? RED : WHITE);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
        }

         if (!(ct - cnmea.vwr_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_mtw, fontCog, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
        }

        SDL_RenderCopyEx(sdlApp->renderer, menuBar, NULL, &menuBarR, 0, NULL, SDL_FLIP_NONE);
        addMenuItems(sdlApp, fontSrc);

        get_text_and_rect(sdlApp->renderer, 580, 10, 0, msg_tod, fontTod, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
        SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);

        if (subTaskbar != NULL) {
            SDL_RenderCopyEx(sdlApp->renderer, subTaskbar, NULL, &subTaskbarR, 0, NULL, SDL_FLIP_NONE);
        }

        if (sdlApp->conf->netStat == 1) {
           SDL_RenderCopyEx(sdlApp->renderer, netStatBar, NULL, &netStatbarR, 0, NULL, SDL_FLIP_NONE);
        } else {
            SDL_RenderCopyEx(sdlApp->renderer, noNetStatbar, NULL, &netStatbarR, 0, NULL, SDL_FLIP_NONE);
        }

        if (sdlApp->conf->runWrn) {
            if (sdlApp->conf->muted == 0) {
                SDL_RenderCopyEx(sdlApp->renderer, muteBar, NULL, &mutebarR, 0, NULL, SDL_FLIP_NONE);
            } else {
                SDL_RenderCopyEx(sdlApp->renderer, unmuteBar, NULL, &mutebarR, 0, NULL, SDL_FLIP_NONE);
            }
        }

        if (boxItem) {
            textBoxR.h = boxItem*50 +30;
            SDL_RenderCopyEx(sdlApp->renderer, textBox, NULL, &textBoxR, 0, NULL, SDL_FLIP_NONE);
        }

        SDL_RenderPresent(sdlApp->renderer); 

        doVnc(sdlApp);

        sdlApp->textFieldArrIndx--;
        do {
            SDL_DestroyTexture(sdlApp->textFieldArr[sdlApp->textFieldArrIndx]);
        } while (sdlApp->textFieldArrIndx-- >0);

        SDL_Delay(200);
    }

    if (subTaskbar != NULL) {
        SDL_DestroyTexture(subTaskbar);
    }
    SDL_DestroyTexture(gaugeGps);
    SDL_DestroyTexture(menuBar);
    SDL_DestroyTexture(netStatBar);
    SDL_DestroyTexture(noNetStatbar);
    SDL_DestroyTexture(muteBar);
    SDL_DestroyTexture(unmuteBar);
    SDL_DestroyTexture(textBox);
    TTF_CloseFont(fontHD);
    TTF_CloseFont(fontLA);
    TTF_CloseFont(fontLO);
    TTF_CloseFont(fontMG);
    TTF_CloseFont(fontCog);
    TTF_CloseFont(fontSrc);
    TTF_CloseFont(fontTod);
 
    return e.type;
}

// Present Depth data (NMEA net only)
static int doDepth(sdl2_app *sdlApp)
{
    SDL_Event e;
    SDL_Rect gaugeR, needleR, menuBarR, subTaskbarR, netStatbarR, mutebarR, textBoxR;
    TTF_Font* fontLarge =  TTF_OpenFont(sdlApp->fontPath, 46);
    TTF_Font* fontSmall =  TTF_OpenFont(sdlApp->fontPath, 18);
    TTF_Font* fontMedium =  TTF_OpenFont(sdlApp->fontPath, 24);
    TTF_Font* fontCog = TTF_OpenFont(sdlApp->fontPath, 42);
    TTF_Font* fontSrc = TTF_OpenFont(sdlApp->fontPath, 14);
    TTF_Font* fontTod = TTF_OpenFont(sdlApp->fontPath, 16);

    SDL_Texture* gaugeDepthW = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "depthw.png");
    SDL_Texture* gaugeDepth = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "depth.png");
    SDL_Texture* gaugeDepthx10 = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "depthx10.png");
    SDL_Texture* menuBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "menuBar.png");
    SDL_Texture* netStatBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "netStat.png");
    SDL_Texture* noNetStatbar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "noNetStat.png");
    SDL_Texture* gaugeNeedleApp = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "needle.png");
    SDL_Texture* muteBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "mute.png");
    SDL_Texture* unmuteBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "unmute.png");
    SDL_Texture* textBox = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "textBox.png");

    SDL_Texture* gauge;
    SDL_Texture* subTaskbar = NULL;
    
    SDL_Rect textField_rect;

    gaugeR.w = 440;
    gaugeR.h = 440;
    gaugeR.x = 19;
    gaugeR.y = 18;

    needleR.w = 240;
    needleR.h = 240;
    needleR.x = 120;
    needleR.y = 122;

    menuBarR.w = 393;
    menuBarR.h = 50;
    menuBarR.x = 400;
    menuBarR.y = 400;

    subTaskbarR.w = 50;
    subTaskbarR.h = 50;
    subTaskbarR.x = 30;
    subTaskbarR.y = 400;

    netStatbarR.w = 25;
    netStatbarR.w = 25;
    netStatbarR.h = 25;
    netStatbarR.x = 20;
    netStatbarR.y = 20;

    mutebarR.w = 25;
    mutebarR.h = 25;
    mutebarR.x = 70;
    mutebarR.y = 20;

    textBoxR.w = 290;
    textBoxR.h = 42;
    textBoxR.x = 470;
    textBoxR.y = 106;

    int boxItems[] = {120,170,220,270};

    sdlApp->curPage = DPTPAGE;

    if (sdlApp->subAppsCmd[sdlApp->curPage][0] != NULL) {
        char icon[PATH_MAX];
        sprintf(icon , "%s/%s.png", IMAGE_PATH, sdlApp->subAppsIco[sdlApp->curPage][0]);
        if ((subTaskbar = IMG_LoadTexture(sdlApp->renderer, icon)) == NULL)
            subTaskbar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "tool.png");           
    }

    float t_angle = 0;
    float angle = 0;
    float dynUpd;

    while (1) {
        int boxItem = 0;
        sdlApp->textFieldArrIndx = 0;
        float depth;
        float scale; 
        char msg_dbt[40];
        char msg_mtw[40] = { "" };
        char msg_dtw[40] = { "" };
        char msg_hdm[40] = { "" };
        char msg_stw[40] = { "" };
        char msg_rmc[40] = { "" };
        char msg_vwt[40] = { "" };
        char msg_tod[40];
        time_t ct;

        // Constants for instrument
        const float minangle = 12;  // Scale start
        const float maxangle = 236; // Scale end
        const float maxsdepth = 10;
        int doBreak = 0;
        int doPlot = 0;
        
        while (SDL_PollEvent(&e)) {

            if (e.type == SDL_QUIT) {
                doBreak = 1;
                break;
            }

            if (e.type == SDL_FINGERDOWN || e.type == SDL_MOUSEBUTTONDOWN)
            {
                if ((e.type=pageSelect(sdlApp, &e))) {
                    doBreak = 1;
                    if (e.type == TSKPAGE && subTaskbar != NULL) {
                        SDL_SetTextureColorMod(subTaskbar, 128, 128, 128);
                        SDL_RenderCopyEx(sdlApp->renderer, subTaskbar, NULL, &subTaskbarR, 0, NULL, SDL_FLIP_NONE);
                        SDL_RenderPresent(sdlApp->renderer);
                    }
                    break;
                }
            }
        }
        if (doBreak == 1) break;

        SDL_RenderClear(sdlApp->renderer);

        ct = time(NULL);    // Get a timestamp for this turn 
        strftime(msg_tod, sizeof(msg_tod),TIMEDATFMT, localtime(&ct));

        // DPT - Depth
         if (ct - cnmea.dbt_ts > S_TIMEOUT || cnmea.dbt == 0)
            sprintf(msg_dbt, "----");
        else {
            sprintf(msg_dbt, cnmea.dbt >= 100.0? "%.0f" : "%.1f", cnmea.dbt);
            doPlot++;
        }

        if (sdlApp->conf->runWrn) {
            sprintf(msg_dtw, "@%.1f", warn.depthw);
        }

        // Heading
        if (!(ct - cnmea.hdm_ts > S_TIMEOUT))
            sprintf(msg_hdm, "COG: %.0f", cnmea.hdm);

        // RMC - Recommended minimum specific GPS/Transit data
        if (!(ct - cnmea.rmc_ts > S_TIMEOUT))
             sprintf(msg_rmc, "SOG: %.1f", cnmea.rmc);
        
        // VHW - Water speed and Heading
        if (!(ct - cnmea.stw_ts > S_TIMEOUT))
            sprintf(msg_stw, "STW: %.1f", cnmea.stw);

        // MTW - Water temperature in C
        if (ct - cnmea.mtw_ts > S_TIMEOUT || cnmea.mtw == 0)
            sprintf(msg_vwt, "----");
        else
            sprintf(msg_vwt, "Temp :%.1f", cnmea.mtw);

        // WND - Relative wind speed in m/s
        if (!(ct - cnmea.vwr_ts > S_TIMEOUT))
            sprintf(msg_mtw, "WND: %.1f", cnmea.vwrs);

        gauge = gaugeDepth;
        if (cnmea.dbt <=5 || (cnmea.dbt <= 10 && cnmea.dbt <= warn.depthw)) {
            gauge = gaugeDepthW;
        }
        if (cnmea.dbt > 10) gauge = gaugeDepthx10;

        depth = cnmea.dbt;
        if (depth > 10.0) depth /=10;


        scale = depth * (maxangle/maxsdepth);
        angle = roundf(scale+minangle);

        // Run needle with smooth acceleration
        if (angle > t_angle) t_angle += 3.2 * (fabsf(angle -t_angle) / 24) ;
        else if (angle < t_angle) t_angle -= 3.2 * (fabsf(angle -t_angle) / 24);

        SDL_RenderCopy(sdlApp->renderer, Background_Tx, NULL, NULL);
    
        if (!sdlApp->plotMode) {
            SDL_RenderCopyEx(sdlApp->renderer, gauge, NULL, &gaugeR, 0, NULL, SDL_FLIP_NONE);

            if (!(ct - cnmea.dbt_ts > S_TIMEOUT || cnmea.dbt == 0) && cnmea.dbt < 110)
            SDL_RenderCopyEx(sdlApp->renderer, gaugeNeedleApp, NULL, &needleR, t_angle, NULL, SDL_FLIP_NONE);
        }

        if (!sdlApp->plotMode) {
            get_text_and_rect(sdlApp->renderer, 182, 300, 4, msg_dbt, fontLarge, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);
        } else {
            get_text_and_rect(sdlApp->renderer, 182, 390, 4, msg_dbt, fontLarge, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, cnmea.dbt <= warn.depthw? RED: BLACK);
        }
        SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);

        if (!sdlApp->plotMode) {
            get_text_and_rect(sdlApp->renderer, 180, 370, 1, msg_vwt, fontSmall, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);

            if (!(ct - cnmea.hdm_ts > S_TIMEOUT)) {
                get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_hdm, fontCog, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
                SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
            }
            if (!(ct - cnmea.rmc_ts > S_TIMEOUT)) {
                get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_rmc, fontCog, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
                SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
            }

            if (!(ct - cnmea.stw_ts > S_TIMEOUT)) {
                get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_stw, fontCog, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
                SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
            }

            if (!(ct - cnmea.vwr_ts > S_TIMEOUT)) {
                get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_mtw, fontCog, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
                SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
            }
        }

        SDL_RenderCopyEx(sdlApp->renderer, menuBar, NULL, &menuBarR, 0, NULL, SDL_FLIP_NONE);
        addMenuItems(sdlApp, fontSrc);

        get_text_and_rect(sdlApp->renderer, 580, 10, 0, msg_tod, fontTod, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
        SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);

        if (subTaskbar != NULL) {
            SDL_RenderCopyEx(sdlApp->renderer, subTaskbar, NULL, &subTaskbarR, 0, NULL, SDL_FLIP_NONE);
        }

        if (sdlApp->conf->netStat == 1) {
           SDL_RenderCopyEx(sdlApp->renderer, netStatBar, NULL, &netStatbarR, 0, NULL, SDL_FLIP_NONE);
        } else {
            SDL_RenderCopyEx(sdlApp->renderer, noNetStatbar, NULL, &netStatbarR, 0, NULL, SDL_FLIP_NONE);
        }

        if (sdlApp->conf->runWrn) {
            if (!sdlApp->plotMode) {
                get_text_and_rect(sdlApp->renderer, 264, 158, 1, msg_dtw, fontMedium, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);
                SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
            }

            if (sdlApp->conf->muted == 0) {
                SDL_RenderCopyEx(sdlApp->renderer, muteBar, NULL, &mutebarR, 0, NULL, SDL_FLIP_NONE);
            } else {
                SDL_RenderCopyEx(sdlApp->renderer, unmuteBar, NULL, &mutebarR, 0, NULL, SDL_FLIP_NONE);
            }
        }

        if (boxItem) {
            textBoxR.h = boxItem*50 +30;
            SDL_RenderCopyEx(sdlApp->renderer, textBox, NULL, &textBoxR, 0, NULL, SDL_FLIP_NONE);
        }

        static double depthHist[DHISTORY];
        static int histHead;
        static int histCount;

        inline double auto_scale(double maxDepth)
        {
            int scale = 10;

            while (scale < maxDepth)
                scale += 10;

            return scale;
        }

        inline void draw_text(SDL_Renderer *r, TTF_Font *font, const char *txt, int x, int y)
        {
            SDL_Color c = {220,220,220,255};

            SDL_Surface *s = TTF_RenderUTF8_Blended(font, txt, c);
            SDL_Texture *t = SDL_CreateTextureFromSurface(r, s);

            SDL_Rect d = {x,y,s->w,s->h};

            SDL_RenderCopy(r,t,NULL,&d);

            SDL_FreeSurface(s);
            SDL_DestroyTexture(t);
        }

        if (sdlApp->plotMode)
        {
            int w=DWINDOW_W;
            int h=DWINDOW_H;
            char buf[32];

            SDL_Rect plot = {80,60,w-120,h-175};
            depthHist[histHead] = cnmea.dbt;

            histHead = (histHead+1)%DHISTORY;

            if(histCount<DHISTORY)
                histCount++;

            double maxDepth=0;

            for(int i=0;i<histCount;i++)
                if(depthHist[i]>maxDepth)
                    maxDepth=depthHist[i];

            double yMax = auto_scale(maxDepth);

            SDL_SetRenderDrawColor(sdlApp->renderer,12,18,28,255);

            SDL_SetRenderDrawColor(sdlApp->renderer,25,35,55,255);
            SDL_RenderFillRect(sdlApp->renderer,&plot);

            SDL_SetRenderDrawColor(sdlApp->renderer,150,150,150,255);
            SDL_RenderDrawRect(sdlApp->renderer,&plot);

            /* ---------- DEPTH ZONES ---------- */

            // Beräkna var den röda zonen slutar (toppen av det röda)
            int redY = plot.y + plot.h - (warn.depthw / yMax) * plot.h;
            
            // Rita den röda zonen (botten)
            SDL_Rect red = {plot.x, redY, plot.w, plot.y + plot.h - redY};
            SDL_SetRenderDrawColor(sdlApp->renderer, 70, 0, 0, 255);
            SDL_RenderFillRect(sdlApp->renderer, &red);

            // Rita den tonade zonen i 6 steg från redY upp till plot.y
            int steps = 30;
            float stepHeight = (float)(redY - plot.y) / steps;

            for (int i = 0; i < steps; i++) {
                SDL_Rect section;
                section.x = plot.x;
                section.w = plot.w;
                // Vi ritar nerifrån och upp
                section.y = redY - (int)((i + 1) * stepHeight);
                section.h = (int)stepHeight + 1; // +1 för att undvika glipor vid avrundning

                // Linjär interpolering av färg:
                // i=0 (nära rött): r=70, g=0
                // i=5 (toppen):    r=0,  g=60
                int r = 20 - (i * 20 / (steps - 1));
                int g = (i * 160 / (steps - 1));

                SDL_SetRenderDrawColor(sdlApp->renderer, r, g, 0, 255);
                SDL_RenderFillRect(sdlApp->renderer, &section);
            }

            SDL_SetRenderDrawColor(sdlApp->renderer,70,0,0,255);
            SDL_RenderFillRect(sdlApp->renderer,&red);

            int thresholdY =
                plot.y + plot.h -
                (warn.depthw/yMax)*plot.h;

            SDL_SetRenderDrawColor(sdlApp->renderer,255,70,70,255);

            SDL_RenderDrawLine(
                sdlApp->renderer,
                plot.x,
                thresholdY,
                plot.x+plot.w,
                thresholdY);

            if (cnmea.dbt < 35) {
                sprintf(buf,"%.1f m",warn.depthw);
                draw_text(sdlApp->renderer,fontSrc,buf,plot.x,thresholdY);
            }

            int thresholdW =
                plot.y + plot.h -
                (warn.draftw/yMax)*plot.h;

            SDL_SetRenderDrawColor(sdlApp->renderer,255,255,0,255);

            SDL_RenderDrawLine(
                sdlApp->renderer,
                plot.x,
                thresholdW,
                plot.x+plot.w,
                thresholdW);

            if (cnmea.dbt < 35) {
                sprintf(buf,"%.1f m",warn.draftw);
                draw_text(sdlApp->renderer,fontSrc,buf,plot.x,thresholdW);
            }

            size_t newest = (histHead+DHISTORY-1)%DHISTORY;

            double stepX = (double)plot.w/(DHISTORY-1);

            int prevX=0,prevY=0;

            for(size_t age=histCount; age>0; age--)
            {
                size_t a = age-1;

                size_t idx = (newest+DHISTORY-a)%DHISTORY;

                double v = depthHist[idx];

                if(v>yMax) v=yMax;

                int x = plot.x + plot.w - a*stepX;

                int y = plot.y + plot.h -
                        (v/yMax)*plot.h;

                if((int)age<histCount)
                {
                    SDL_SetRenderDrawColor(sdlApp->renderer,50,180,255,255);
                    SDL_RenderDrawLine(sdlApp->renderer,prevX,prevY,x,y);
                }

                prevX=x;
                prevY=y;
            }

            sprintf(buf,"%.0f m",yMax);
            draw_text(sdlApp->renderer,fontSrc,buf,20,plot.y-6);

            sprintf(buf,"%.0f m",yMax/2);
            draw_text(sdlApp->renderer,fontSrc,buf,28,plot.y+plot.h/2-6);

            draw_text(sdlApp->renderer,fontSrc,"0 m",40,plot.y+plot.h-10);

            // ----- TIME MARKERS -----

            const char *labels[] = {"-20s","-15s","-10s","-5s","Now"};
            int marks = 5;

            for(int i=0;i<marks;i++)
            {
                int x = plot.x + (plot.w * i)/(marks-1);

                SDL_SetRenderDrawColor(sdlApp->renderer,180,180,180,255);

                SDL_RenderDrawLine(
                    sdlApp->renderer,
                    x,
                    plot.y + plot.h,
                    x,
                    plot.y + plot.h + 6);

                draw_text(
                    sdlApp->renderer,
                    fontSrc,
                    labels[i],
                    x - 15,
                    plot.y + plot.h + 10);
            }
        }

        SDL_RenderPresent(sdlApp->renderer);

        doVnc(sdlApp);

        if (!sdlApp->plotMode) {
            // Reduce CPU load if only short scale movements
            dynUpd = (1/fabsf(angle -t_angle))*200;
            dynUpd = dynUpd > 200? 200:dynUpd;
            SDL_Delay(30+(int)dynUpd);
        }   else {
            SDL_Delay(70);

        }

        sdlApp->textFieldArrIndx--;
        do {
            SDL_DestroyTexture(sdlApp->textFieldArr[sdlApp->textFieldArrIndx]);
        } while (sdlApp->textFieldArrIndx-- >0);

    }

    if (subTaskbar != NULL) {
        SDL_DestroyTexture(subTaskbar);
    }

    SDL_DestroyTexture(gaugeDepth);
    SDL_DestroyTexture(gaugeDepthW);
    SDL_DestroyTexture(gaugeDepthx10);
    SDL_DestroyTexture(gaugeNeedleApp);
    SDL_DestroyTexture(menuBar);
    SDL_DestroyTexture(netStatBar);
    SDL_DestroyTexture(noNetStatbar);
    SDL_DestroyTexture(muteBar);
    SDL_DestroyTexture(unmuteBar);
    SDL_DestroyTexture(textBox);
    TTF_CloseFont(fontLarge);
    TTF_CloseFont(fontMedium);
    TTF_CloseFont(fontSmall);
    TTF_CloseFont(fontCog);
    TTF_CloseFont(fontSrc);
    TTF_CloseFont(fontTod);

    return e.type;
}

// Present Wind data (NMEA net only)
static int doWind(sdl2_app *sdlApp)
{
    SDL_Event e;
    SDL_Rect gaugeR, needleR, menuBarR, subTaskbarR, netStatbarR, mutebarR, textBoxR;;
    TTF_Font* fontLarge =  TTF_OpenFont(sdlApp->fontPath, 46);
    TTF_Font* fontSmall =  TTF_OpenFont(sdlApp->fontPath, 20);
    TTF_Font* fontCog = TTF_OpenFont(sdlApp->fontPath, 42);
    TTF_Font* fontSrc = TTF_OpenFont(sdlApp->fontPath, 14);
    TTF_Font* fontTod = TTF_OpenFont(sdlApp->fontPath, 16);

    SDL_Texture* gaugeSumlog = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "wind.png");
    SDL_Texture* gaugeNeedleApp = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "needle.png");
    SDL_Texture* gaugeNeedleTrue = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "needle-black.png");
    SDL_Texture* menuBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "menuBar.png");
    SDL_Texture* netStatBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "netStat.png");
    SDL_Texture* noNetStatbar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "noNetStat.png");
    SDL_Texture* muteBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "mute.png");
    SDL_Texture* unmuteBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "unmute.png");
    SDL_Texture* textBox = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "textBox.png");

    SDL_Texture* subTaskbar = NULL;

    gaugeR.w = 440;
    gaugeR.h = 440;
    gaugeR.x = 19;
    gaugeR.y = 18;

    needleR.w = 240;
    needleR.h = 240;
    needleR.x = 120;
    needleR.y = 122;

    menuBarR.w = 393;
    menuBarR.h = 50;
    menuBarR.x = 400;
    menuBarR.y = 400;

    subTaskbarR.w = 50;
    subTaskbarR.h = 50;
    subTaskbarR.x = 30;
    subTaskbarR.y = 400;

    netStatbarR.w = 25;
    netStatbarR.h = 25;
    netStatbarR.x = 20;
    netStatbarR.y = 20;

    mutebarR.w = 25;
    mutebarR.h = 25;
    mutebarR.x = 70;
    mutebarR.y = 20;

    textBoxR.w = 290;
    textBoxR.h = 42;
    textBoxR.x = 470;
    textBoxR.y = 106;

    int boxItems[] = {120,170,220,270,320};

    sdlApp->curPage = WNDPAGE;

    if (sdlApp->subAppsCmd[sdlApp->curPage][0] != NULL) {
        char icon[PATH_MAX];
        sprintf(icon , "%s/%s.png", IMAGE_PATH, sdlApp->subAppsIco[sdlApp->curPage][0]);
        if ((subTaskbar = IMG_LoadTexture(sdlApp->renderer, icon)) == NULL)
            subTaskbar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "tool.png");           
    }

    SDL_Rect textField_rect;

    float t_angle_a = 0;
    float t_angle_t = 0;
    float angle_a = 0;
    float angle_t = 0;
    int res = 1;
    int res_a = 1;
    float dynUpd;

    const float offset = 131; // For scale

    while (1) {
        int boxItem = 0;
        sdlApp->textFieldArrIndx = 0;
        char msg_vwrs[40];
        char msg_vwts[40];
        char msg_vwra[40];
        char msg_dbt[40] = { "" };
        char msg_stw[40] = { "" };
        char msg_hdm[40] = { "" };
        char msg_rmc[40] = { "" };
        char msg_tod[40];
        time_t ct;
        int doBreak = 0;
        
        while (SDL_PollEvent(&e)) {

            if (e.type == SDL_QUIT) {
                doBreak = 1;
                break;
            }

            if (e.type == SDL_FINGERDOWN || e.type == SDL_MOUSEBUTTONDOWN)
            {
                if ((e.type=pageSelect(sdlApp, &e))) {
                    doBreak = 1;
                    if (e.type == TSKPAGE && subTaskbar != NULL) {
                        SDL_SetTextureColorMod(subTaskbar, 128, 128, 128);
                        SDL_RenderCopyEx(sdlApp->renderer, subTaskbar, NULL, &subTaskbarR, 0, NULL, SDL_FLIP_NONE);
                        SDL_RenderPresent(sdlApp->renderer);
                    }
                    break;
                }
            }
        }
        if (doBreak == 1) break;

        SDL_RenderClear(sdlApp->renderer);

        ct = time(NULL);    // Get a timestamp for this turn
        strftime(msg_tod, sizeof(msg_tod),TIMEDATFMT, localtime(&ct));

        // Wind speed and angle (relative)
         if (ct -  cnmea.vwr_ts > S_TIMEOUT || cnmea.vwrs == 0)
            sprintf(msg_vwrs, "----");
        else
            sprintf(msg_vwrs, "%.1f", cnmea.vwrs);

        if (ct - cnmea.vwr_ts > S_TIMEOUT)
            sprintf(msg_vwra, "----");
        else {
            sprintf(msg_vwra, " %.0f%c", cnmea.vwra, 0xb0);
        }

        // True wind speed
         if (ct -  cnmea.vwt_ts > S_TIMEOUT || cnmea.vwts == 0)
            sprintf(msg_vwts, "----");
        else
            sprintf(msg_vwts, "TRUE: %.1f", cnmea.vwts);

        // DPT - Depth
        if (!(ct - cnmea.dbt_ts > S_TIMEOUT || cnmea.dbt == 0))
            sprintf(msg_dbt, "DBT: %.1f", cnmea.dbt);
        
        // Heading
        if (!(ct - cnmea.hdm_ts > S_TIMEOUT))
            sprintf(msg_hdm, "COG: %.0f", cnmea.hdm);

        // RMC - Recommended minimum specific GPS/Transit data
        if (!(ct - cnmea.rmc_ts > S_TIMEOUT))
             sprintf(msg_rmc, "SOG: %.1f", cnmea.rmc);
        
        // VHW - Water speed and Heading
         if (!(ct - cnmea.stw_ts > S_TIMEOUT))
            sprintf(msg_stw, "STW: %.1f", cnmea.stw);
   
        angle_a = cnmea.vwra; // 0-180

        if (cnmea.vwrd == 1) angle_a = 360 - angle_a; // Mirror the needle motion

        angle_a += offset;  

        angle_a = rotate(angle_a, res); res=0;
        
        // Run needle with smooth acceleration
        if (angle_a > t_angle_a) t_angle_a += 3.2 * (fabsf(angle_a -t_angle_a) / 24) ;
        else if (angle_a < t_angle_a) t_angle_a -= 3.2 * (fabsf(angle_a -t_angle_a) / 24);

        angle_t = cnmea.vwta; // 0-180

        if (cnmea.vwrd == 1) angle_t = 360 - angle_t; // Mirror the needle motion

        angle_t += offset;  

        angle_t = rotate_a(angle_t, res_a); res_a=0;
        
        // Run needle with smooth acceleration
        if (angle_t > t_angle_t) t_angle_t += 3.2 * (fabsf(angle_t -t_angle_t) / 24) ;
        else if (angle_t < t_angle_t) t_angle_t -= 3.2 * (fabsf(angle_t -t_angle_t) / 24);

        SDL_RenderCopy(sdlApp->renderer, Background_Tx, NULL, NULL);
       
        SDL_RenderCopyEx(sdlApp->renderer, gaugeSumlog, NULL, &gaugeR, 0, NULL, SDL_FLIP_NONE);

        if (!(ct - cnmea.vwr_ts > S_TIMEOUT || cnmea.vwra == 0))
            SDL_RenderCopyEx(sdlApp->renderer, gaugeNeedleApp, NULL, &needleR, t_angle_a, NULL, SDL_FLIP_NONE);

        if (!(ct - cnmea.stw_ts > S_TIMEOUT) && cnmea.stw > 0.9) 
            SDL_RenderCopyEx(sdlApp->renderer, gaugeNeedleTrue, NULL, &needleR, t_angle_t, NULL, SDL_FLIP_NONE);

        get_text_and_rect(sdlApp->renderer, 216, 100, 4, msg_vwra, fontSmall, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);
        SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);

        get_text_and_rect(sdlApp->renderer, 182, 300, 4, msg_vwrs, fontLarge, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);    
        SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);

        if (!(ct - cnmea.stw_ts > S_TIMEOUT) && cnmea.stw > 0.9) {
            get_text_and_rect(sdlApp->renderer, 150, 356, 4, msg_vwts, fontSmall,&sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);    
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
        }
        
        if (!(ct - cnmea.hdm_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_hdm, fontCog, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
        }
        
        if (!(ct - cnmea.stw_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_stw, fontCog, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
        }

        if (!(ct - cnmea.rmc_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_rmc, fontCog, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
        }

        if (!(ct - cnmea.dbt_ts > S_TIMEOUT || cnmea.dbt == 0)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_dbt, fontCog, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, cnmea.dbt <= warn.depthw? RED : WHITE);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
        }

        SDL_RenderCopyEx(sdlApp->renderer, menuBar, NULL, &menuBarR, 0, NULL, SDL_FLIP_NONE);
        addMenuItems(sdlApp, fontSrc);

        get_text_and_rect(sdlApp->renderer, 580, 10, 0, msg_tod, fontTod, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
        SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect); 

        if (subTaskbar != NULL) {
            SDL_RenderCopyEx(sdlApp->renderer, subTaskbar, NULL, &subTaskbarR, 0, NULL, SDL_FLIP_NONE);
        }

        if (sdlApp->conf->netStat == 1) {
           SDL_RenderCopyEx(sdlApp->renderer, netStatBar, NULL, &netStatbarR, 0, NULL, SDL_FLIP_NONE);
        } else {
            SDL_RenderCopyEx(sdlApp->renderer, noNetStatbar, NULL, &netStatbarR, 0, NULL, SDL_FLIP_NONE);
        }

        if (sdlApp->conf->runWrn) {
            if (sdlApp->conf->muted == 0) {
                SDL_RenderCopyEx(sdlApp->renderer, muteBar, NULL, &mutebarR, 0, NULL, SDL_FLIP_NONE);
            } else {
                SDL_RenderCopyEx(sdlApp->renderer, unmuteBar, NULL, &mutebarR, 0, NULL, SDL_FLIP_NONE);
            }
        }

        if (boxItem) {
            textBoxR.h = boxItem*50 +30;
            SDL_RenderCopyEx(sdlApp->renderer, textBox, NULL, &textBoxR, 0, NULL, SDL_FLIP_NONE);
        }

        SDL_RenderPresent(sdlApp->renderer);
 
        doVnc(sdlApp);
        
        // Reduce CPU load if only short scale movements
        dynUpd = (1/fabsf(angle_a -t_angle_a))*200;
        dynUpd = dynUpd > 200? 200:dynUpd;

        SDL_Delay(30+(int)dynUpd);

        sdlApp->textFieldArrIndx--;
        do {
            SDL_DestroyTexture(sdlApp->textFieldArr[sdlApp->textFieldArrIndx]);
        } while (sdlApp->textFieldArrIndx-- >0);
    }

    if (subTaskbar != NULL) {
        SDL_DestroyTexture(subTaskbar);
    }
    SDL_DestroyTexture(gaugeSumlog);
    SDL_DestroyTexture(gaugeNeedleApp);
    SDL_DestroyTexture(gaugeNeedleTrue);
    SDL_DestroyTexture(menuBar);
    SDL_DestroyTexture(netStatBar);
    SDL_DestroyTexture(noNetStatbar);
    SDL_DestroyTexture(muteBar);
    SDL_DestroyTexture(unmuteBar);
    SDL_DestroyTexture(textBox);
    TTF_CloseFont(fontLarge);
    TTF_CloseFont(fontSmall);
    TTF_CloseFont(fontCog);
    TTF_CloseFont(fontSrc);
    TTF_CloseFont(fontTod);

    return e.type;
}


static void set_alsa_volume(sdl2_app *sdlApp, float percent)
{   
    long volume = sdlApp->conf->snd_minv + (long)((sdlApp->conf->snd_maxv - sdlApp->conf->snd_minv) * percent);

    snd_mixer_selem_set_playback_volume_all(sdlApp->conf->elem, volume);
}

static float get_current_volume(sdl2_app *sdlApp)
{
    long volume;
    snd_mixer_selem_get_playback_volume(sdlApp->conf->elem, SND_MIXER_SCHN_FRONT_LEFT, &volume);

    return (float)(volume - sdlApp->conf->snd_minv) / (sdlApp->conf->snd_maxv - sdlApp->conf->snd_minv);
}


// Present Environmant page (Non standard NMEA)
static int doEnvironment(sdl2_app *sdlApp)
{
    SDL_Event e;
    SDL_Rect gaugeVoltR, gaugeCurrR, gaugeTempR, voltNeedleR, currNeedleR;
    SDL_Rect tempNeedleR, menuBarR, netStatbarR, mutebarR, subTaskbarR;
    TTF_Font* fontSmall = TTF_OpenFont(sdlApp->fontPath, 14);
    TTF_Font* fontLarge = TTF_OpenFont(sdlApp->fontPath, 18);
    TTF_Font* fontTod = TTF_OpenFont(sdlApp->fontPath, 16);

    SDL_Texture* menuBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "menuBar.png");
    SDL_Texture* netStatBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "netStat.png");
    SDL_Texture* noNetStatbar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "noNetStat.png");
    SDL_Texture* muteBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "mute.png");
    SDL_Texture* unmuteBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "unmute.png");

    SDL_Texture* gaugeVolt = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "volt.png");
    SDL_Texture* gaugeVolt24 = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "volt-24.png");
    SDL_Texture* gaugeCurr = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "curr.png");
    SDL_Texture* gaugeTemp = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "temp.png");
    SDL_Texture* needleVolt = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "sneedle.png");
    SDL_Texture* needleCurr = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "sneedle.png");
    SDL_Texture* needleTemp = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "sneedle.png");

    sdlApp->curPage = PWRPAGE;

    SDL_Texture* subTaskbar = NULL;

    if (sdlApp->subAppsCmd[sdlApp->curPage][0] != NULL) {
        char icon[PATH_MAX];
        sprintf(icon , "%s/%s.png", IMAGE_PATH, sdlApp->subAppsIco[sdlApp->curPage][0]);
        if ((subTaskbar = IMG_LoadTexture(sdlApp->renderer, icon)) == NULL)
            subTaskbar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "tool.png");           
    }

    gaugeVoltR.w = 200;
    gaugeVoltR.h = 200;
    gaugeVoltR.x = 80;
    gaugeVoltR.y = 33;

    gaugeCurrR.w = 200;
    gaugeCurrR.h = 200;
    gaugeCurrR.x = 300;
    gaugeCurrR.y = 33;

    gaugeTempR.w = 200;
    gaugeTempR.h = 200;
    gaugeTempR.x = 520;
    gaugeTempR.y = 33;

    voltNeedleR.w = 100;
    voltNeedleR.h = 62;
    voltNeedleR.x = 131;
    voltNeedleR.y = 110;

    currNeedleR.w = 100;
    currNeedleR.h = 62;
    currNeedleR.x = 349;
    currNeedleR.y = 110;

    tempNeedleR.w = 100;
    tempNeedleR.h = 62;
    tempNeedleR.x = 572;
    tempNeedleR.y = 110;

    menuBarR.w = 393;
    menuBarR.h = 50;
    menuBarR.x = 400;
    menuBarR.y = 400;

    netStatbarR.w = 25;
    netStatbarR.h = 25;
    netStatbarR.x = 20;
    netStatbarR.y = 20;

    mutebarR.w = 25;
    mutebarR.h = 25;
    mutebarR.x = 70;
    mutebarR.y = 20;

    subTaskbarR.w = 50;
    subTaskbarR.h = 50;
    subTaskbarR.x = 30;
    subTaskbarR.y = 400;

    SDL_Rect textField_rect;

    SDL_Rect slider = {
        (SWINDOW_WIDTH - SLIDER_WIDTH) / 2,
        (SWINDOW_HEIGHT - SLIDER_HEIGHT) / 2,
        SLIDER_WIDTH,
        SLIDER_HEIGHT
    };

    // Scale adjustments
    float v_maxangle = 102;
    float v_offset = 6;
    float v_max = 16;
    float v_min = 8;
    float v_scaleoffset = 7.9;

    float c_maxangle = 120;
    float c_offset = 58;
    float c_max = 30;
    float c_scaleoffset = 0;

    float t_maxangle = 136;
    float t_offset = 33;
    float t_max = 50;
    float t_scaleoffset = 5;

    char msg_volt[20] = {"0.0"};
    char msg_curr[20] = {"0.0"};
    char msg_temp[20] = {"0.0"};

    char msg_volt_bank[20] = {"Bank -"};
    char msg_curr_bank[20] = {"Bank -"};
    char msg_temp_loca[20] = {"--"};

    // Volume adjustments
    float volume_percent = 0.0;
    int dragging = 0;
    int w, h;

    if (sdlApp->conf->snd_useMixer) {
        volume_percent = get_current_volume(sdlApp);
        SDL_GetWindowSize(sdlApp->window, &w, &h);
    }

    while (1) {
        sdlApp->textFieldArrIndx = 0;
        char msg_tod[40];
        char msg_stm[40];
        char msg_kWhp[80];
        char msg_kWhn[80];
        float v_angle, c_angle, t_angle;
        float volt_value = 0;
        float curr_value = 0;
        float temp_value = 0;
        int doPlot = 0;
        time_t ct;
        int doBreak = 0;
        
        while (SDL_PollEvent(&e)) {

            if (e.type == SDL_QUIT) {
                doBreak = 1;
                break;
            }

            if (sdlApp->conf->snd_useMixer && !sdlApp->conf->muted) {
                if (e.type == SDL_MOUSEMOTION && dragging) {

                    SDL_Point p = (SDL_Point){e.motion.x/sdlApp->conf->scale, e.motion.y/sdlApp->conf->scale};

                    if (SDL_PointInRect(&p, &slider)) {
                        int mouseY = p.y;

                        float rel = (float)(slider.y + slider.h - mouseY) / slider.h;

                        if (rel < 0.0f) rel = 0.0f;
                        if (rel > 1.0f) rel = 1.0f;

                        volume_percent = rel;
                        set_alsa_volume(sdlApp, volume_percent);
                    }
                }
            }

            if (e.type == SDL_FINGERDOWN || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEMOTION) 
            {
                if ((e.type=pageSelect(sdlApp, &e))) {
                    doBreak = 1;
                    if (e.type == TSKPAGE && subTaskbar != NULL) {
                        SDL_SetTextureColorMod(subTaskbar, 128, 128, 128);
                        SDL_RenderCopyEx(sdlApp->renderer, subTaskbar, NULL, &subTaskbarR, 0, NULL, SDL_FLIP_NONE);
                        SDL_RenderPresent(sdlApp->renderer);
                    }
                    break;
                }

                SDL_Point p = (SDL_Point){e.button.x/sdlApp->conf->scale, e.button.y/sdlApp->conf->scale};

                if (SDL_PointInRect(&p, &slider)) {
                    dragging = 1;
                }
            }

            if (e.type == SDL_MOUSEBUTTONUP)
                dragging = 0;
            }

        if (doBreak == 1) break;

        ct = time(NULL);    // Get a timestamp for this turn
        strftime(msg_tod, sizeof(msg_tod),TIMEDATFMT, localtime(&ct));

        if (!(ct - cnmea.volt_ts > S_TIMEOUT)) {
            sprintf(msg_volt, "%.1f", cnmea.volt);
            volt_value = cnmea.volt > v_max? cnmea.volt/2: cnmea.volt;
            sprintf(msg_volt_bank, "Bank %d", cnmea.volt_bank);
            doPlot++;
        }

        if (!(ct - cnmea.curr_ts > S_TIMEOUT)) {
            sprintf(msg_curr, "%.1f", cnmea.curr);
            curr_value = cnmea.curr;
            sprintf(msg_curr_bank, "Bank %d", cnmea.curr_bank);
            doPlot++;
        }

        if (!(ct - cnmea.temp_ts > S_TIMEOUT)) {
            sprintf(msg_temp, "%.1f", cnmea.temp);
            temp_value = cnmea.temp;
            if (cnmea.temp_loc == 1)
                sprintf(msg_temp_loca, "Indoor");
        }

        if (cnmea.startTime) {
            strftime(msg_stm, sizeof(msg_stm),"%x:%H:%M", localtime(&cnmea.startTime));
            if (cnmea.kWhn < 1.0)
                sprintf(msg_kWhn, "%.3f kWh spent since %s", cnmea.kWhn, msg_stm);
            else
                sprintf(msg_kWhn, "%.1f kWh spent since %s", cnmea.kWhn, msg_stm);
            
            if (cnmea.kWhp < 1.0)
                sprintf(msg_kWhp, "%.3f kWh charged. Net : %.3f kWh", cnmea.kWhp, cnmea.kWhp - cnmea.kWhn);
            else
                sprintf(msg_kWhp, "%.1f kWh charged. Net : %.3f kWh", cnmea.kWhp, cnmea.kWhp - cnmea.kWhn);
        }

        SDL_RenderClear(sdlApp->renderer);

        SDL_RenderCopy(sdlApp->renderer, Background_Tx, NULL, NULL);

        SDL_RenderCopyEx(sdlApp->renderer, cnmea.volt < v_max? gaugeVolt:gaugeVolt24, NULL, &gaugeVoltR, 0, NULL, SDL_FLIP_NONE);
        SDL_RenderCopyEx(sdlApp->renderer, gaugeCurr, NULL, &gaugeCurrR, 0, NULL, SDL_FLIP_NONE);
        SDL_RenderCopyEx(sdlApp->renderer, gaugeTemp, NULL, &gaugeTempR, 0, NULL, SDL_FLIP_NONE);

        if (!(ct - cnmea.volt_ts > S_TIMEOUT || volt_value < v_min || volt_value > v_max )) {
            v_angle = ((volt_value-v_scaleoffset) * (v_maxangle/v_max) *2)+v_offset;
            SDL_RenderCopyEx(sdlApp->renderer, needleVolt, NULL, &voltNeedleR, v_angle, NULL, SDL_FLIP_NONE);

            get_text_and_rect(sdlApp->renderer, 164, 170, 0, msg_volt, fontSmall, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);

            get_text_and_rect(sdlApp->renderer, 156, 187, 0, msg_volt_bank, fontSmall,&sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
        }

        if (!(ct -  cnmea.curr_ts > S_TIMEOUT)) {
            if (fabs(curr_value) < 33 ) {
                c_angle = (((curr_value*0.5)-c_scaleoffset) * (c_maxangle/c_max)*2)+c_offset;
                SDL_RenderCopyEx(sdlApp->renderer, needleCurr, NULL, &currNeedleR, c_angle, NULL, SDL_FLIP_NONE);
            }

            get_text_and_rect(sdlApp->renderer, 386, 170, 0, msg_curr, fontSmall, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);

            get_text_and_rect(sdlApp->renderer, 380, 187, 0, msg_curr_bank, fontSmall, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
        }

        if (!(ct -  cnmea.temp_ts > S_TIMEOUT)) {
            t_angle = ((temp_value-t_scaleoffset) * (t_maxangle/t_max)*1.2)+t_offset;
            SDL_RenderCopyEx(sdlApp->renderer, needleTemp, NULL, &tempNeedleR, t_angle, NULL, SDL_FLIP_NONE);

            get_text_and_rect(sdlApp->renderer, 605, 170, 0, msg_temp, fontSmall, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);

            get_text_and_rect(sdlApp->renderer, 598, 187, 0, msg_temp_loca, fontSmall, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
        }

        if (sdlApp->conf->netStat == 1) {
           SDL_RenderCopyEx(sdlApp->renderer, netStatBar, NULL, &netStatbarR, 0, NULL, SDL_FLIP_NONE);
        } else {
            SDL_RenderCopyEx(sdlApp->renderer, noNetStatbar, NULL, &netStatbarR, 0, NULL, SDL_FLIP_NONE);
        }

        if (sdlApp->conf->runWrn) {
            if (sdlApp->conf->muted == 0) {
                SDL_RenderCopyEx(sdlApp->renderer, muteBar, NULL, &mutebarR, 0, NULL, SDL_FLIP_NONE);
            } else {
                SDL_RenderCopyEx(sdlApp->renderer, unmuteBar, NULL, &mutebarR, 0, NULL, SDL_FLIP_NONE);
            }
        }

        SDL_RenderCopyEx(sdlApp->renderer, menuBar, NULL, &menuBarR, 0, NULL, SDL_FLIP_NONE);
        addMenuItems(sdlApp, fontSmall);

        get_text_and_rect(sdlApp->renderer, 580, 10, 0, msg_tod, fontTod, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
        SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);

        if (cnmea.startTime)
        {
            get_text_and_rect(sdlApp->renderer, 104, 416, 0, msg_kWhn, fontSmall,&sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
            get_text_and_rect(sdlApp->renderer, 104, 432, 0, msg_kWhp, fontSmall, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
        }

        if (subTaskbar != NULL) {
            SDL_RenderCopyEx(sdlApp->renderer, subTaskbar, NULL, &subTaskbarR, 0, NULL, SDL_FLIP_NONE);
        }

        static double powerHist[PHISTORY];
        static double voltHist[PHISTORY];

        static int histHead;
        static int histCount;

        inline double auto_scale(double maxVal)
        {
            if(maxVal <= 50) return 50;
            if(maxVal <= 500) return 500;
            return 1000;
        }

        inline void draw_text(SDL_Renderer *r,TTF_Font *font,const char *txt,int x,int y)
        {
            SDL_Color c={230,230,230,255};

            SDL_Surface *s=TTF_RenderUTF8_Blended(font,txt,c);
            SDL_Texture *t=SDL_CreateTextureFromSurface(r,s);

            SDL_Rect d={x,y,s->w,s->h};

            SDL_RenderCopy(r,t,NULL,&d);

            SDL_FreeSurface(s);
            SDL_DestroyTexture(t);
        }

        inline void draw_textc(SDL_Renderer *r,TTF_Font *font,const char *txt,int x,int y, SDL_Color c)
        {
            SDL_Surface *s=TTF_RenderUTF8_Blended(font,txt,c);
            SDL_Texture *t=SDL_CreateTextureFromSurface(r,s);

            SDL_Rect d={x,y,s->w,s->h};

            SDL_RenderCopy(r,t,NULL,&d);

            SDL_FreeSurface(s);
            SDL_DestroyTexture(t);
        }

        // Do plotting
        if (cnmea.startTime && !dragging)
        {
            SDL_Rect plot={100,240,PWINDOW_W-100,PWINDOW_H-120};

            double power = fabs(cnmea.volt * cnmea.curr);
            double volt = fabs(cnmea.volt);

            powerHist[histHead]=power;
            voltHist[histHead]=volt;

            histHead=(histHead+1)%PHISTORY;

            if(histCount<PHISTORY)
                histCount++;

            double maxPower=0;

            for(int i=0;i<SCALE_WINDOW;i++)
            {
                int idx=(histHead-i-1+PHISTORY)%PHISTORY;

                if(powerHist[idx]>maxPower)
                    maxPower=powerHist[idx];
            }

            double yMax=auto_scale(maxPower);

            SDL_SetRenderDrawColor(sdlApp->renderer,30,40,60,255);
            SDL_RenderFillRect(sdlApp->renderer,&plot);

            SDL_SetRenderDrawColor(sdlApp->renderer,160,160,160,255);
            SDL_RenderDrawRect(sdlApp->renderer,&plot);

            for(int i=1;i<5;i++)
            {
                int y=plot.y+i*plot.h/5;

                SDL_SetRenderDrawColor(sdlApp->renderer,70,70,80,255);
                SDL_RenderDrawLine(sdlApp->renderer,plot.x,y,plot.x+plot.w,y);
            }

            int newest=(histHead+PHISTORY-1)%PHISTORY;

            double stepX=(double)plot.w/(PHISTORY-1);

            int prevPX=0,prevPY=0;
            int prevVX=0,prevVY=0;

            for(int age=histCount;age>0;age--)
            {
                int a=age-1;
                int idx=(newest+PHISTORY-a)%PHISTORY;

                int x=plot.x+plot.w-a*stepX;

                double p=powerHist[idx];
                if(p>yMax) p=yMax;

                int py=plot.y+plot.h-(p/yMax)*plot.h;

                double v=voltHist[idx];

                int vy=plot.y+plot.h-((v-8.0)/8.0)*plot.h;

                if(age<histCount)
                {
                    if (cnmea.curr >=0 )
                        SDL_SetRenderDrawColor(sdlApp->renderer,60,255,140,255);
                    else
                        SDL_SetRenderDrawColor(sdlApp->renderer,255,100,100,255);

                    SDL_RenderDrawLine(sdlApp->renderer,prevPX,prevPY,x,py);

                    SDL_SetRenderDrawColor(sdlApp->renderer,220,140,50,255);
                    SDL_RenderDrawLine(sdlApp->renderer,prevVX,prevVY,x,vy);
                }

                prevPX=x;
                prevPY=py;

                prevVX=x;
                prevVY=vy;
            }

            char buf[32];
            SDL_Color c;

            if (cnmea.curr >=0 )
                c = (SDL_Color){100,255,100,255};
            else
                c = (SDL_Color){255,100,100,255};

            sprintf(buf,"%.1f W",power);
            draw_textc(sdlApp->renderer,fontLarge,buf, plot.x-75 ,plot.y-30, c);

            sprintf(buf,"%.0f W",yMax);
            draw_text(sdlApp->renderer,fontSmall,buf,25,plot.y-6);

            sprintf(buf,"%.0f W",yMax/2);
            draw_text(sdlApp->renderer,fontSmall,buf,30,plot.y+plot.h/2-6);

            draw_text(sdlApp->renderer,fontSmall,"0 W",45,plot.y+plot.h-10);

            for(int v=8;v<=16;v++)
            {
                int y=plot.y+plot.h-((v-8.0)/8.0)*plot.h;

                SDL_SetRenderDrawColor(sdlApp->renderer,120,120,120,255);

                SDL_RenderDrawLine(
                    sdlApp->renderer,
                    plot.x+plot.w,
                    y,
                    plot.x+plot.w+6,
                    y);

                char txt[8];
                sprintf(txt,"%d",v);

                draw_text(sdlApp->renderer,fontSmall,txt,plot.x+plot.w+10,y-7);
            }

            draw_text(sdlApp->renderer,fontSmall,"Volts",plot.x+plot.w+10,plot.y-20);

            int ticks=25;

            for(int i=0;i<=ticks;i++)
            {
                int x=plot.x+(plot.w*i)/ticks;

                SDL_SetRenderDrawColor(sdlApp->renderer,170,170,170,255);

                SDL_RenderDrawLine(
                    sdlApp->renderer,
                    x,
                    plot.y+plot.h,
                    x,
                    plot.y+plot.h+6);

                char label[6];
                sprintf(label,"%d",25-i);

                draw_text(sdlApp->renderer,fontSmall,label,x-6,plot.y+plot.h+10);
            }
        }
    
        if (sdlApp->conf->snd_useMixer && sdlApp->conf->snd_showMixer && !sdlApp->conf->muted) {

            slider.x = w - SLIDER_WIDTH - RIGHT_MARGIN;
            slider.w = SLIDER_WIDTH;
            slider.h = SLIDER_HEIGHT;

            slider.y = h - SLIDER_HEIGHT - (int)(h * 0.25);

            /* Slider background */
            SDL_SetRenderDrawColor(sdlApp->renderer, 80, 80, 80, 255);
            SDL_RenderFillRect(sdlApp->renderer, &slider);

            /* Filled portion */
            SDL_Rect fill = slider;
            fill.h = (int)(slider.h * volume_percent);
            fill.y = slider.y + (slider.h - fill.h);

            /* Slider foreground green to red */
            SDL_SetRenderDrawColor(sdlApp->renderer, (Uint8)(volume_percent * 200.0f), 200-(Uint8)(volume_percent * 150.0f), 0, 255);
            SDL_RenderFillRect(sdlApp->renderer, &fill);

            /* Render volume text */
            char text[16];
            int percent_display = (int)(volume_percent * 100.0f);
            snprintf(text, sizeof(text), "%d%%", percent_display);

            SDL_Color black = {255, 255, 255, 0};
            SDL_Surface *surface = TTF_RenderText_Blended(fontSmall, text, black);

            SDL_Texture *texture = SDL_CreateTextureFromSurface(sdlApp->renderer, surface);

            SDL_Rect textRect;
            textRect.w = surface->w;
            textRect.h = surface->h;
            textRect.x = slider.x -4;
            textRect.y = slider.y - 20;
            SDL_RenderCopy(sdlApp->renderer, texture, NULL, &textRect);

            SDL_FreeSurface(surface);
            SDL_DestroyTexture(texture);

        }

        SDL_RenderPresent(sdlApp->renderer);
 
        doVnc(sdlApp);

        if (!dragging) {
            SDL_Delay(90);
        }

        sdlApp->textFieldArrIndx--;
        do {
            SDL_DestroyTexture(sdlApp->textFieldArr[sdlApp->textFieldArrIndx]);
        } while (sdlApp->textFieldArrIndx-- >0);
    }

    if (subTaskbar != NULL) {
        SDL_DestroyTexture(subTaskbar);
    }

    SDL_DestroyTexture(gaugeVolt);
    SDL_DestroyTexture(gaugeVolt24);
    SDL_DestroyTexture(gaugeCurr);
    SDL_DestroyTexture(gaugeTemp);
    SDL_DestroyTexture(needleVolt);
    SDL_DestroyTexture(needleCurr);
    SDL_DestroyTexture(needleTemp);
    SDL_DestroyTexture(menuBar);
    SDL_DestroyTexture(netStatBar); 
    SDL_DestroyTexture(noNetStatbar);
    SDL_DestroyTexture(muteBar);
    SDL_DestroyTexture(unmuteBar);  
    TTF_CloseFont(fontTod);
    TTF_CloseFont(fontSmall);
    TTF_CloseFont(fontLarge);
 
    return e.type;
}

static int doCamera(sdl2_app *sdlApp)
{
    AVFormatContext *fmt = NULL;
    AVCodecContext *vctx = NULL;
    AVCodecContext *actx = NULL;
    struct SwrContext *swr = NULL;

    // Volume adjustments
    float volume_percent = 0.0;
    int dragging = 0;

    SDL_Rect slider = {
        (SWINDOW_WIDTH - SLIDER_WIDTH) / 2,
        (SWINDOW_HEIGHT - SLIDER_HEIGHT) / 2,
        SLIDER_WIDTH,
        SLIDER_HEIGHT
    };

    if (sdlApp->conf->snd_useMixer) {
        volume_percent = get_current_volume(sdlApp);
    }

    SDL_Rect mutebarR, pWaitR, exitbuttR, bgR;
  
    int vstream = -1;
    int astream = -1;
    int audio_buf_size = 192000;
    int wasMuted;
    int running = 0;

    AVPacket pkt;
    AVFrame *frame = av_frame_alloc();

    uint8_t *audio_buf = NULL;
    audio_buf = (uint8_t*)malloc(audio_buf_size);

    SDL_Texture *tex = NULL;
    SDL_AudioDeviceID deviceId = 0;
    SDL_Event e;

    wasMuted = sdlApp->conf->muted;
    sdlApp->conf->muted = 1;    // Prevent warnings
    int muted = 0;

    int got_keyframe = 0;

    avformat_network_init();

    av_log_set_level(AV_LOG_QUIET);

    int win_w, win_h;
    SDL_GetWindowSize(sdlApp->window, &win_w, &win_h);  

    pWaitR.w = win_w;
    pWaitR.h = win_h;
    pWaitR.x = 0;
    pWaitR.y = 0;

    mutebarR.w = 25;
    mutebarR.h = 25;
    mutebarR.x = 70;
    mutebarR.y = 20;
    bgR.w = bgR.h = 30;
    bgR.x = 68;
    bgR.y = 18;

    exitbuttR.w = 40;
    exitbuttR.h = 40;
    exitbuttR.x = 62;
    exitbuttR.y = 60;

    SDL_Texture* muteBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "mute.png");
    SDL_Texture* unmuteBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "unmute.png");
    TTF_Font* fontSmall = TTF_OpenFont(sdlApp->fontPath, 14);

    char pict[PATH_MAX];

    SDL_Texture* pWait = NULL;
    sprintf(pict , "%s/pwait.png", IMAGE_PATH);
    pWait = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "pwait.png");  

    SDL_RenderCopyEx(sdlApp->renderer, pWait, NULL, &pWaitR, 0, NULL, SDL_FLIP_NONE);
    SDL_RenderPresent(sdlApp->renderer); 

    SDL_Texture* pQuit = NULL;
    sprintf(pict , "%s/quitButton.png", IMAGE_PATH);
    pQuit = IMG_LoadTexture(sdlApp->renderer, pict);

    int w, h;
    SDL_GetWindowSize(sdlApp->window, &w, &h);
    
    int toggle = 1;
    running = 4;

    while (running)
    {
        AVDictionary *opts = NULL;

        char *proto[] = {"tcp", "udp"};

        av_dict_set(&opts,"rtsp_transport", proto[(toggle = !toggle)],0);
        av_dict_set(&opts,"fflags","nobuffer",0);
        av_dict_set(&opts,"flags","low_delay",0);
        av_dict_set(&opts,"max_delay","0",0);

        if (avformat_open_input(&fmt, sdlApp->conf->cam_url, NULL, &opts) < 0)
        {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "Failed to open RTSP %s stream %s... retrying %d", sdlApp->conf->cam_url, proto[toggle], running);
            SDL_Delay(1000);
            av_dict_free(&opts);
            running--;
            continue;
        }

        if (avformat_find_stream_info(fmt,NULL) < 0)
        {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Stream info failed... retrying %d", running);
            avformat_close_input(&fmt);
            SDL_Delay(2000);
            running--;
            continue;
        }

        av_dict_free(&opts);

        vstream = -1;
        astream = -1;

        for (int i=0;i<(int)fmt->nb_streams;i++)
        {
            if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && vstream==-1)
                vstream = i;

            if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && astream==-1)
                astream = i;
        }

        got_keyframe = 0;

        /* VIDEO */

        if (vstream >= 0)
        {
            const AVCodec *vcodec = avcodec_find_decoder(fmt->streams[vstream]->codecpar->codec_id);

            vctx = avcodec_alloc_context3(vcodec);
            avcodec_parameters_to_context(vctx, fmt->streams[vstream]->codecpar);
            avcodec_open2(vctx,vcodec,NULL);

            tex = SDL_CreateTexture(
                sdlApp->renderer,
                SDL_PIXELFORMAT_YV12,
                SDL_TEXTUREACCESS_STREAMING,
                vctx->width,
                vctx->height);
        }

        /* AUDIO */

        if (astream >= 0)
        {
            const AVCodec *acodec =
                avcodec_find_decoder(fmt->streams[astream]->codecpar->codec_id);

            actx = avcodec_alloc_context3(acodec);
            avcodec_parameters_to_context(actx, fmt->streams[astream]->codecpar);
            avcodec_open2(actx,acodec,NULL);

            SDL_AudioSpec want;
            SDL_zero(want);
            want.freq = 48000;
            want.format = AUDIO_S16SYS;
            want.channels = 2;
            want.samples = 4096;

            if (sdlApp->conf->snd_useMixer) {
                if ((deviceId = SDL_OpenAudioDevice(sdlApp->conf->snd_card_name, 0, &want, NULL, 0)) == 0) { 
                    if ((deviceId = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0)) == 0) { // Open default
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                            "Failed to SDL_OpenAudioDevice %s: %s", sdlApp->conf->snd_card_name, SDL_GetError()); 
                    }
                }
            }

            if (deviceId)
                SDL_PauseAudioDevice(deviceId,0);

            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;

            if (swr_alloc_set_opts2(
                &swr,
                &stereo,
                AV_SAMPLE_FMT_S16,
                48000,
                &actx->ch_layout,
                actx->sample_fmt,
                actx->sample_rate,
                0,
                NULL) < 0)
            {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "swr_alloc_set_opts2 failed. Giving up");
                running = 0;
                break;
            }

            if (swr_init(swr) < 0)
            {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "swr_init failed. Giving up");
                running = 0;
                break;
            }
        }

        int hideQuit = 0;

        while (running && av_read_frame(fmt, &pkt) >= 0) {

            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) {
                    running = 0;
                    break;
                }

                if (sdlApp->conf->snd_useMixer && !muted) {
                    if (e.type == SDL_MOUSEMOTION && dragging) {

                        SDL_Point p = (SDL_Point){e.motion.x/sdlApp->conf->scale, e.motion.y/sdlApp->conf->scale};

                        if (SDL_PointInRect(&p, &slider)) {
                            int mouseY = p.y;

                            float rel = (float)(slider.y + slider.h - mouseY) / slider.h;

                            if (rel < 0.0f) rel = 0.0f;
                            if (rel > 1.0f) rel = 1.0f;

                            volume_percent = rel;
                            set_alsa_volume(sdlApp, volume_percent);
                        }
                    }
                }

                if (e.type == SDL_FINGERDOWN || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEMOTION)
                {

                    hideQuit=40;

                    SDL_Point p;
                    int x, y;
                    if (e.type == SDL_FINGERDOWN) {
                        x = e.tfinger.x* w;
                        y = e.tfinger.y* h;
                        p = (SDL_Point){ x, y };
                    } else {
                        p = (SDL_Point){e.button.x/sdlApp->conf->scale, e.button.y/sdlApp->conf->scale};
                    }

                    if (SDL_PointInRect( &p, &exitbuttR)) {
                        running = 0;
                        break;
                    }
                    
                    if (e.type == SDL_FINGERDOWN) {
                        if (SDL_PointInRect(&p, &mutebarR)) {
                            muted = !muted;
                        }
                    }

                    p = (SDL_Point){e.button.x/sdlApp->conf->scale, e.button.y/sdlApp->conf->scale};

                    if (SDL_PointInRect(&p, &slider)) {
                        dragging = 1;
                    }
                }

                if (e.type == SDL_MOUSEBUTTONUP)
                    dragging = 0;
            }

            if (running == 0) break;        

            /* VIDEO */

            if (pkt.stream_index == vstream)
            {
                if (!got_keyframe)
                {
                    if (!(pkt.flags & AV_PKT_FLAG_KEY))
                    {
                        av_packet_unref(&pkt);
                        continue;
                    }
                    got_keyframe = 1;
                }

                avcodec_send_packet(vctx,&pkt);

                while (avcodec_receive_frame(vctx,frame) == 0)
                {
                    SDL_UpdateYUVTexture(
                        tex,
                        NULL,
                        frame->data[0],frame->linesize[0],
                        frame->data[1],frame->linesize[1],
                        frame->data[2],frame->linesize[2]);

                    SDL_RenderClear(sdlApp->renderer);

                    SDL_RenderCopy(sdlApp->renderer,tex,NULL,NULL);

                    // Draw Exit button
                    if (pQuit != NULL) {
                        if (hideQuit-- >=0)
                            SDL_RenderCopyEx(sdlApp->renderer, pQuit, NULL, &exitbuttR, 0, NULL, SDL_FLIP_NONE);
                    }

                    if (deviceId) {
                        if (muted == 1 || hideQuit > 0 ) {
                            SDL_SetRenderDrawColor(sdlApp->renderer, 255, 255, 255, 90);
                            SDL_RenderFillRect(sdlApp->renderer, &bgR);
                        }

                        if (hideQuit > 0 && !muted) {
                                SDL_RenderCopyEx(sdlApp->renderer, muteBar, NULL, &mutebarR, 0, NULL, SDL_FLIP_NONE);
                        }
                        if (muted == 1) {
                            SDL_RenderCopyEx(sdlApp->renderer, unmuteBar, NULL, &mutebarR, 0, NULL, SDL_FLIP_NONE);
                        }
                    }

                    if (sdlApp->conf->snd_useMixer && sdlApp->conf->snd_showMixer && hideQuit > 0 && !muted)
                    {

                        slider.x = w - SLIDER_WIDTH - RIGHT_MARGIN;
                        slider.w = SLIDER_WIDTH;
                        slider.h = SLIDER_HEIGHT;

                        slider.y = h - SLIDER_HEIGHT - (int)(h * 0.25);

                        /* Slider background */
                        SDL_SetRenderDrawColor(sdlApp->renderer, 80, 80, 80, 255);
                        SDL_RenderFillRect(sdlApp->renderer, &slider);

                        /* Filled portion */
                        SDL_Rect fill = slider;
                        fill.h = (int)(slider.h * volume_percent);
                        fill.y = slider.y + (slider.h - fill.h);

                        /* Slider foreground green to red */
                        SDL_SetRenderDrawColor(sdlApp->renderer, (Uint8)(volume_percent * 200.0f), 200-(Uint8)(volume_percent * 150.0f), 0, 255);
                        SDL_RenderFillRect(sdlApp->renderer, &fill);

                        /* Render volume text */
                        char text[16];
                        int percent_display = (int)(volume_percent * 100.0f);
                        snprintf(text, sizeof(text), "%d%%", percent_display);

                        SDL_Color black = {125, 125, 125, 0};
                        SDL_Surface *surface = TTF_RenderText_Blended(fontSmall, text, black);

                        SDL_Texture *texture = SDL_CreateTextureFromSurface(sdlApp->renderer, surface);

                        SDL_Rect textRect;
                        textRect.w = surface->w;
                        textRect.h = surface->h;
                        textRect.x = slider.x -4;
                        textRect.y = slider.y - 20;
                        SDL_RenderCopy(sdlApp->renderer, texture, NULL, &textRect);

                        SDL_FreeSurface(surface);
                        SDL_DestroyTexture(texture);

                    }

                    SDL_RenderPresent(sdlApp->renderer);

                    doVnc(sdlApp);
                }
            }

            /* AUDIO */

            if (pkt.stream_index == astream && actx && swr)
            {
                avcodec_send_packet(actx,&pkt);

                while (avcodec_receive_frame(actx,frame) == 0)
                {
                    uint8_t *out_planes[1] = { audio_buf };

                    int out_samples = swr_convert(
                        swr,
                        out_planes,
                        audio_buf_size/4,
                        (const uint8_t**)frame->data,
                        frame->nb_samples);

                    if (out_samples > 0 && deviceId > 0 && !muted)
                    {
                        int out_size = out_samples * 2 * 2;
                        SDL_QueueAudio(deviceId,audio_buf,out_size);
                    }
                }
            }

            av_packet_unref(&pkt);
        }

        avformat_close_input(&fmt);
        SDL_Delay(1000);
    }

    if (vctx) avcodec_free_context(&vctx);
    if (actx) avcodec_free_context(&actx);
    if (frame) av_frame_free(&frame);
    if (swr) swr_free(&swr);
    if (audio_buf) free(audio_buf);

    if (deviceId) {
        SDL_CloseAudioDevice(deviceId);
    }

    if (tex != NULL) {
        SDL_DestroyTexture(tex);
    }

    if (pWait != NULL) {
        SDL_DestroyTexture(pWait);
    }

    if (pQuit != NULL) {
        SDL_DestroyTexture(pQuit);
    }

    TTF_CloseFont(fontSmall);
    SDL_DestroyTexture(muteBar);
    SDL_DestroyTexture(unmuteBar);

    sdlApp->conf->muted = wasMuted;

    if (e.type == SDL_QUIT)
        return e.type;

    return sdlApp->curPage;
}

static int findAudiodevice(char *dname, void *ptr)
{
    audRunner *doRun = ptr;

    int card = -1;
    snd_ctl_t *handle;
    snd_ctl_card_info_t *info;
    int found = 0;

    snd_ctl_card_info_alloca(&info);

    // Loopa through all audio cards
    while (snd_card_next(&card) == 0 && card >= 0) {
        char hw_name[32];
        sprintf(hw_name, "hw:%d", card);

        if (snd_ctl_open(&handle, hw_name, 0) < 0) continue;
        if (snd_ctl_card_info(handle, info) >= 0) {
            const char *id = snd_ctl_card_info_get_id(info);
            const char *longname = snd_ctl_card_info_get_name(info);

            // Check if requested name is there
            if (strstr(id, dname) || strstr(longname, dname)) {
                // Check for a PCM-device on this card
                int dev = -1;
                snd_pcm_info_t *pcminfo;
                snd_pcm_info_alloca(&pcminfo);

                while (snd_ctl_pcm_next_device(handle, &dev) == 0 && dev >= 0) {
                    snd_pcm_info_set_device(pcminfo, dev);
                    snd_pcm_info_set_subdevice(pcminfo, 0);
                    snd_pcm_info_set_stream(pcminfo, SND_PCM_STREAM_CAPTURE);

                    if (snd_ctl_pcm_info(handle, pcminfo) >= 0) {
                        //printf("Found %s Capture: hw:%d,%d\n", doRun->snd_card_dev, card, dev);
                        snprintf(doRun->snd_card_dev, sizeof(doRun->snd_card_dev),"hw:%d,%d", card, dev);
                        found = 1;
                        break;
                    }
                }
            }
        }
        snd_ctl_close(handle);
    }

    return found;
}

static int threadAudio(void *ptr)
{
    audRunner *doRun = ptr;
    int dnm;
    int found=0;
    snd_pcm_t *capture_handle;
    snd_pcm_t *playback_handle;
 
    while ((!doRun->run)) {
        SDL_Delay(1000); 
    }

    // Use arecord -l to insert a new device here
     char *dnames[]={   "MS2109", \
                        "Guermok USB2 Video", \
                        "MS2130", \
                        "MS2131", \
                        "default" \
                    };

    for (dnm = 0; dnm < sizeof(dnames)/sizeof(char*); dnm++) {
        if ((found=findAudiodevice(dnames[dnm], doRun)) == 1)
            break;
    }

    if (!found) {
        strcpy(doRun->snd_card_dev, dnames[--dnm]);
    }

    SDL_Log("A/V PCM Device: %s named %s", doRun->snd_card_dev, dnames[dnm]);

    // 1. Open device
    if (snd_pcm_open(&capture_handle, doRun->snd_card_dev, SND_PCM_STREAM_CAPTURE, 0) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "snd_pcm_open audio source failed on %s: %s", doRun->snd_card_dev, SDL_GetError());
        SDL_Log("Format ~/.asoundrc to define the default capture PCM audio card");
        return 0;
    }

    snprintf(doRun->snd_card_dev, sizeof(doRun->snd_card_dev), "plug%s", doRun->snd_card);

    // 2. Open Speaker (Output) - "default" works usually best
    if (snd_pcm_open(&playback_handle, doRun->snd_card_dev, SND_PCM_STREAM_PLAYBACK, 0) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "snd_pcm_open audio device %s failed. Trying defaut %s", doRun->snd_card_dev, SDL_GetError());
        if (snd_pcm_open(&playback_handle, "default", SND_PCM_STREAM_PLAYBACK, 0) != 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "snd_pcm_open audio device %s failed: %s", "default", SDL_GetError());
            snd_pcm_close(capture_handle);
            return 0;
        }
    }

    // 3. Configure both of them (must have the same settings!)
    unsigned int rate = 48000;
    snd_pcm_set_params(capture_handle,  SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, rate, 1, 200000);
    snd_pcm_set_params(playback_handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, rate, 1, 200000);


    // Properly enable the sound devices
    snd_pcm_prepare(capture_handle);
    snd_pcm_prepare(playback_handle);

    // Explicit start the sound capture
    snd_pcm_start(capture_handle);

    short buffer[FRAME_SIZE * 2]; // *2 since we have two channels (stereo)

    while (doRun->run) {
        long r = snd_pcm_readi(capture_handle, buffer, FRAME_SIZE);
        
        if (r < 0) {
            // recover handles both -EPIPE and -ESTRPIPE
            r = snd_pcm_recover(capture_handle, r, 0);
            if (r < 0) continue; // retry
        }

        if (r > 0 && !doRun->mute) {
            long w = snd_pcm_writei(playback_handle, buffer, r);
            if (w < 0) {
                w = snd_pcm_recover(playback_handle, w, 0);
            }
        }
    }
    snd_pcm_close(capture_handle);
    snd_pcm_close(playback_handle);

    return 0;
}

static int doVideoCapture(sdl2_app *sdlApp)
{
    SDL_Rect mutebarR, exitbuttR, bgR;
    TTF_Font* fontSmall = TTF_OpenFont(sdlApp->fontPath, 14);
    SDL_Thread *audioThread = NULL;
    int wasMuted;

    static audRunner doRun;

    doRun.run = 0;
    doRun.astat = 1;
    strncpy(doRun.snd_card, sdlApp->conf->snd_card, sizeof(doRun.snd_card));

    if ((audioThread = SDL_CreateThread(threadAudio, "VideoAudio", &doRun)) == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateThread VideoAudio failed: %s", SDL_GetError());
        doRun.astat = 0;
    } else SDL_DetachThread(audioThread);

    // Volume adjustments
    float volume_percent = 0.0;
    int dragging = 0;

    SDL_Rect slider = {
        (SWINDOW_WIDTH - SLIDER_WIDTH) / 2,
        (SWINDOW_HEIGHT - SLIDER_HEIGHT) / 2,
        SLIDER_WIDTH,
        SLIDER_HEIGHT
    };

    if (sdlApp->conf->snd_useMixer) {
        volume_percent = get_current_volume(sdlApp);
    }

    struct buffer {
        void *start;
        size_t length;
    };

    int w, h;
    SDL_GetWindowSize(sdlApp->window, &w, &h);

    exitbuttR.w = 40;
    exitbuttR.h = 40;
    exitbuttR.x = 62;
    exitbuttR.y = 60;

    mutebarR.w = 25;
    mutebarR.h = 25;
    mutebarR.x = 70;
    mutebarR.y = 20;
    bgR.w = bgR.h = 30;
    bgR.x = 68;
    bgR.y = 18;

    if (!strlen(sdlApp->conf->vid_device))
        return sdlApp->curPage;

    int vfd = open(sdlApp->conf->vid_device, O_RDWR);
    if (vfd < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Open video device %s failed: %s", sdlApp->conf->vid_device, SDL_GetError());
        return sdlApp->curPage;
    }

    char pict[PATH_MAX];
    SDL_Texture* pQuit = NULL;
    sprintf(pict , "%s/quitButton.png", IMAGE_PATH);
    pQuit = IMG_LoadTexture(sdlApp->renderer, pict);

    // Use "v4l2-ctl --list-formats-ex" to figure out supported resolutions
    // 640x480 is good enough for a 8" display. Higher resolutions increase stuttering.
    int win_w=640;
    int win_h=480;

    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = win_w;
    fmt.fmt.pix.height = win_h;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(vfd, VIDIOC_S_FMT, &fmt) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ioctl VIDIOC_S_FMT on %s failed: %s", sdlApp->conf->vid_device, SDL_GetError());
        close(vfd);
        return sdlApp->curPage;
    }

    struct v4l2_requestbuffers req = {0};
    req.count =  VIDEO_BUFFERS;;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if(ioctl(vfd, VIDIOC_REQBUFS, &req) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ioctl VIDIOC_REQBUFS on %s failed: %s", sdlApp->conf->vid_device, SDL_GetError());
        close(vfd);
        return sdlApp->curPage;
    }

    struct buffer buffers[VIDEO_BUFFERS];

    for (int i=0;i<VIDEO_BUFFERS;i++)
    {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if(ioctl(vfd, VIDIOC_QUERYBUF, &buf) < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ioctl VIDIOC_QUERYBUF on %s failed: %s", sdlApp->conf->vid_device, SDL_GetError());
            close(vfd);
            return sdlApp->curPage;
        }
        buffers[i].length = buf.length;
        buffers[i].start = mmap(NULL, buf.length, PROT_READ|PROT_WRITE, MAP_SHARED, vfd, buf.m.offset);
        if(!buffers[i].start) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ioctl VIDIOC_QUERYBUF on %s failed: %s", sdlApp->conf->vid_device, SDL_GetError());
            close(vfd);
            return sdlApp->curPage;
        }
        if(ioctl(vfd, VIDIOC_QBUF, &buf) < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ioctl VIDIOC_QBUF on %s failed: %s", sdlApp->conf->vid_device, SDL_GetError());
            close(vfd);
            return sdlApp->curPage;
        }
    }

    wasMuted = sdlApp->conf->muted;
    sdlApp->conf->muted = 1;    // Prevent warnings

    SDL_Texture* muteBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "mute.png");
    SDL_Texture* unmuteBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "unmute.png");

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(vfd, VIDIOC_STREAMON, &type);

    SDL_Texture *tex = SDL_CreateTexture(
        sdlApp->renderer,
        SDL_PIXELFORMAT_YUY2,
        SDL_TEXTUREACCESS_STREAMING,
        win_w,
        win_h);

    SDL_Event e;
    int running = 1;

    doRun.run = 1;
    doRun.mute = 0;

    int hideQuit = 0;
 
    while (running)
    {
        while (SDL_PollEvent(&e)) 
        {
            if (e.type == SDL_QUIT) {
                running = 0;
                break;
            }

            if (sdlApp->conf->snd_useMixer && !doRun.mute) {
                if (e.type == SDL_MOUSEMOTION && dragging) {

                    SDL_Point p = (SDL_Point){e.motion.x/sdlApp->conf->scale, e.motion.y/sdlApp->conf->scale};

                    if (SDL_PointInRect(&p, &slider)) {
                        int mouseY = p.y;

                        float rel = (float)(slider.y + slider.h - mouseY) / slider.h;

                        if (rel < 0.0f) rel = 0.0f;
                        if (rel > 1.0f) rel = 1.0f;

                        volume_percent = rel;
                        set_alsa_volume(sdlApp, volume_percent);
                    }
                }
            }

            if (e.type == SDL_FINGERDOWN || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEMOTION)
            {
                hideQuit=130;

                SDL_Point p;
                int x, y;
                if (e.type == SDL_FINGERDOWN) {
                    x = e.tfinger.x* w;
                    y = e.tfinger.y* h;
                    p = (SDL_Point){ x, y };
                } else {
                    p = (SDL_Point){e.button.x/sdlApp->conf->scale, e.button.y/sdlApp->conf->scale};
                }

                if (SDL_PointInRect(&p, &exitbuttR)) {
                    running = 0;
                    break;
                }
                
                if (e.type == SDL_FINGERDOWN) {
                    if (SDL_PointInRect(&p, &mutebarR)) {
                        doRun.mute = !doRun.mute;
                    }
                }

                p = (SDL_Point){e.button.x/sdlApp->conf->scale, e.button.y/sdlApp->conf->scale};

                if (SDL_PointInRect(&p, &slider)) {
                    dragging = 1;
                }
            }

            if (e.type == SDL_MOUSEBUTTONUP)
                dragging = 0;
        }

        if (running == 0) break;

        // --- Video ---
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if(ioctl(vfd, VIDIOC_DQBUF, &buf) < 0) continue;

        SDL_UpdateTexture(tex, NULL, buffers[buf.index].start, win_w*2);

        SDL_RenderClear(sdlApp->renderer);
        SDL_RenderCopy(sdlApp->renderer, tex, NULL, NULL);

        SDL_SetRenderDrawBlendMode(sdlApp->renderer, SDL_BLENDMODE_BLEND);

        ioctl(vfd, VIDIOC_QBUF, &buf);

        // Draw Exit button
        if (pQuit != NULL) {
            if (hideQuit-- >=0)
                SDL_RenderCopyEx(sdlApp->renderer, pQuit, NULL, &exitbuttR, 0, NULL, SDL_FLIP_NONE);
        }

        if (doRun.astat) 
        {
            if (doRun.mute == 1 || hideQuit > 0 ) {
                SDL_SetRenderDrawColor(sdlApp->renderer, 255, 255, 255, 90);
                SDL_RenderFillRect(sdlApp->renderer, &bgR);
            }

            if (hideQuit > 0 && !doRun.mute) {
                    SDL_RenderCopyEx(sdlApp->renderer, muteBar, NULL, &mutebarR, 0, NULL, SDL_FLIP_NONE);
            }
            if (doRun.mute == 1) {
                SDL_RenderCopyEx(sdlApp->renderer, unmuteBar, NULL, &mutebarR, 0, NULL, SDL_FLIP_NONE);
            }
        }

        if (sdlApp->conf->snd_useMixer && sdlApp->conf->snd_showMixer && hideQuit > 0 && !doRun.mute) {

            slider.x = w - SLIDER_WIDTH - RIGHT_MARGIN;
            slider.w = SLIDER_WIDTH;
            slider.h = SLIDER_HEIGHT;

            slider.y = h - SLIDER_HEIGHT - (int)(h * 0.25);

            /* Slider background */
            SDL_SetRenderDrawColor(sdlApp->renderer, 80, 80, 80, 255);
            SDL_RenderFillRect(sdlApp->renderer, &slider);

            /* Filled portion */
            SDL_Rect fill = slider;
            fill.h = (int)(slider.h * volume_percent);
            fill.y = slider.y + (slider.h - fill.h);

            /* Slider foreground green to red */
            SDL_SetRenderDrawColor(sdlApp->renderer, (Uint8)(volume_percent * 200.0f), 200-(Uint8)(volume_percent * 150.0f), 0, 255);
            SDL_RenderFillRect(sdlApp->renderer, &fill);

            /* Render volume text */
            char text[16];
            int percent_display = (int)(volume_percent * 100.0f);
            snprintf(text, sizeof(text), "%d%%", percent_display);

            SDL_Color black = {125, 125, 125, 0};
            SDL_Surface *surface = TTF_RenderText_Blended(fontSmall, text, black);

            SDL_Texture *texture = SDL_CreateTextureFromSurface(sdlApp->renderer, surface);

            SDL_Rect textRect;
            textRect.w = surface->w;
            textRect.h = surface->h;
            textRect.x = slider.x -4;
            textRect.y = slider.y - 20;
            SDL_RenderCopy(sdlApp->renderer, texture, NULL, &textRect);

            SDL_FreeSurface(surface);
            SDL_DestroyTexture(texture);
        }

        SDL_RenderPresent(sdlApp->renderer);

        doVnc(sdlApp);
    }

    doRun.run = 0;  // Take down audio thread

    ioctl(vfd, VIDIOC_STREAMOFF, &type);

    for(int i=0;i<VIDEO_BUFFERS;i++)
        munmap(buffers[i].start, buffers[i].length);

    close(vfd);

    if (pQuit != NULL) {
        SDL_DestroyTexture(pQuit);
    }

    TTF_CloseFont(fontSmall);
    SDL_DestroyTexture(muteBar);
    SDL_DestroyTexture(unmuteBar);
    SDL_DestroyTexture(tex);

    sdlApp->conf->muted = wasMuted;

    if (e.type == SDL_QUIT)
        return e.type;

    return sdlApp->curPage;
}

static int doVideo(sdl2_app *sdlApp)
{
    TTF_Font *font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 28);

    typedef struct {
        SDL_Rect circle;
        SDL_Rect text_rect;
        SDL_Texture *text;
    } RadioButton;

    SDL_Texture* renderText(SDL_Renderer *r, TTF_Font *font, const char *msg,
                            SDL_Color color, SDL_Rect *rect)
    {
        SDL_Surface *surf = TTF_RenderUTF8_Blended(font, msg, color);
        SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);

        rect->w = surf->w;
        rect->h = surf->h;

        SDL_FreeSurface(surf);
        return tex;
    }

    void drawRadio(SDL_Renderer *r, RadioButton *b, int selected)
    {
        SDL_RenderDrawRect(r, &b->circle);

        if(selected)
        {
            SDL_Rect dot = {
                b->circle.x + 5,
                b->circle.y + 5,
                b->circle.w - 10,
                b->circle.h - 10
            };
            SDL_RenderFillRect(r, &dot);
        }

        SDL_RenderCopy(r, b->text, NULL, &b->text_rect);
    }

    SDL_Color black = {0,0,0,255};

    RadioButton r1, r2, r3;
    SDL_Rect commit;

    int win_w, win_h;
    SDL_GetWindowSize(sdlApp->window, &win_w, &win_h); 

    int centerX = win_w/2;
    int centerY = win_h/2;

    int spacing = 60;

    r1.circle = (SDL_Rect){centerX - 120, centerY - spacing*2, 20, 20};
    r2.circle = (SDL_Rect){centerX - 120, centerY - spacing, 20, 20};
    r3.circle = (SDL_Rect){centerX - 120, centerY, 20, 20};

    r1.text = renderText(sdlApp->renderer, font, "CAM capture", black, &r1.text_rect);
    r2.text = renderText(sdlApp->renderer, font, "HDMI capture", black, &r2.text_rect);
    r3.text = renderText(sdlApp->renderer, font, "Leave", black, &r3.text_rect);

    r1.text_rect.x = r1.circle.x + 35;
    r1.text_rect.y = r1.circle.y - 5;

    r2.text_rect.x = r2.circle.x + 35;
    r2.text_rect.y = r2.circle.y - 5;

    r3.text_rect.x = r3.circle.x + 35;
    r3.text_rect.y = r3.circle.y - 5;

    commit = (SDL_Rect){centerX-70, centerY + 90, 140, 50};

    SDL_Rect commit_text_rect;
    SDL_Texture *commit_text = renderText(sdlApp->renderer, font, "Commit", black, &commit_text_rect);

    commit_text_rect.x = commit.x + (commit.w - commit_text_rect.w)/2;
    commit_text_rect.y = commit.y + (commit.h - commit_text_rect.h)/2;

    int selected = 0;
    int running = 1;

    SDL_Event e;

    while(running)
    {
        while(SDL_PollEvent(&e))
        {        
            if (e.type == SDL_QUIT) {
                running = 0;
                break;
            }

            if (e.type == SDL_FINGERDOWN || e.type == SDL_MOUSEBUTTONDOWN)
            {
                SDL_Point p;
                int x, y;
                if (e.type == SDL_FINGERDOWN) {
                    x = e.tfinger.x* win_w;
                    y = e.tfinger.y* win_h;
                    p = (SDL_Point){ x, y };
                } else {
                    p = (SDL_Point){e.button.x/sdlApp->conf->scale, e.button.y/sdlApp->conf->scale};
                }

                if (SDL_PointInRect(&p, &r1.circle) || SDL_PointInRect(&p, &r1.text_rect))
                    selected = 1;

                if (strlen(sdlApp->conf->vid_device)) {
                    if (SDL_PointInRect(&p, &r2.circle) || SDL_PointInRect(&p, &r2.text_rect))
                        selected = 2;
                }

                if (SDL_PointInRect(&p, &r3.circle) || SDL_PointInRect(&p, &r3.text_rect))
                    selected = 3;

                if (SDL_PointInRect( &p, &commit))
                {
                    running = 0;
                    break;
                }
            }
        }

        if (running == 0) break;

        SDL_RenderCopy(sdlApp->renderer, Background_Tx, NULL, NULL);

        SDL_SetRenderDrawColor(sdlApp->renderer,0,0,0,255);

        drawRadio(sdlApp->renderer,&r1,selected==1);
        if (!strlen(sdlApp->conf->vid_device)) {
            SDL_SetRenderDrawColor(sdlApp->renderer,255,100,0,255);
            SDL_RenderDrawLine(sdlApp->renderer,commit.x-60,commit.y-140,commit.x+186,commit.y-140);
        }

        SDL_SetRenderDrawColor(sdlApp->renderer,0,0,0,255);

        drawRadio(sdlApp->renderer,&r2,selected==2);
        drawRadio(sdlApp->renderer,&r3,selected==3);

        if (selected) {
            SDL_RenderDrawRect(sdlApp->renderer,&commit);
            SDL_RenderCopy(sdlApp->renderer,commit_text,NULL,&commit_text_rect);
        }

        SDL_RenderPresent(sdlApp->renderer);

        doVnc(sdlApp);
    }

    SDL_DestroyTexture(r1.text);
    SDL_DestroyTexture(r2.text);
    SDL_DestroyTexture(r3.text);
    SDL_DestroyTexture(commit_text);

    TTF_CloseFont(font);

    if (selected == 1)
         return doCamera(sdlApp);

    if (selected == 2)
         return doVideoCapture(sdlApp);

    if (selected == 3)
        return sdlApp->curPage;

    return e.type;
}

#ifdef DIGIFLOW
// Present Fresh Water data. Dendent on project  https://github.com/ehedman/flowSensor
static int doWater(sdl2_app *sdlApp)
{
    SDL_Event e;
    SDL_Rect gaugeR, menuBarR, netStatbarR, mutebarR, textBoxR;
  
    FILE *tankFd;
    int tankIndx = 0;
    int rval = 1;
    char tBuff[40];
    struct stat statbuf;
    struct tm *info_t;
    static char buffer_t[60];

    sdlApp->curPage = WTRPAGE;

    TTF_Font* fontHD =  TTF_OpenFont(sdlApp->fontPath, 40);
    TTF_Font* fontLA =  TTF_OpenFont(sdlApp->fontPath, 40);
    TTF_Font* fontLO =  TTF_OpenFont(sdlApp->fontPath, 30);
    TTF_Font* fontMG =  TTF_OpenFont(sdlApp->fontPath, 14);
    TTF_Font* fontCog = TTF_OpenFont(sdlApp->fontPath, 42);
    TTF_Font* fontSrc = TTF_OpenFont(sdlApp->fontPath, 14);
    TTF_Font* fontTod = TTF_OpenFont(sdlApp->fontPath, 16);

    SDL_Texture* gaugeWtr = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "dflow.png");
    SDL_Texture* menuBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "menuBar.png");
    SDL_Texture* netStatBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "netStat.png");
    SDL_Texture* noNetStatbar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "noNetStat.png");
    SDL_Texture* muteBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "mute.png");
    SDL_Texture* unmuteBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "unmute.png");
    SDL_Texture* textBox = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "textBox.png");

    SDL_Rect textField_rect;

    gaugeR.w = 440;
    gaugeR.h = 440;
    gaugeR.x = 19;
    gaugeR.y = 18;

    menuBarR.w = 393;
    menuBarR.h = 50;
    menuBarR.x = 400;
    menuBarR.y = 400;

    netStatbarR.w = 25;
    netStatbarR.h = 25;
    netStatbarR.x = 20;
    netStatbarR.y = 20;

    mutebarR.w = 25;
    mutebarR.h = 25;
    mutebarR.x = 70;
    mutebarR.y = 20;

    textBoxR.w = 290;
    textBoxR.h = 42;
    textBoxR.x = 470;
    textBoxR.y = 106;

    int boxItems[] = {120,170,220,270,320};

    if (!system("digiflow.sh /tmp/digiflow.txt") && (tankFd = fopen("/tmp/digiflow.txt","r")) != NULL) {
        if (fstat(fileno(tankFd), &statbuf) == 0 && statbuf.st_size != 0) {
            while( fgets(tBuff, sizeof(tBuff), tankFd) != NULL) {
                switch(tankIndx++) {
                    case 0: cnmea.fdate = atol(tBuff); break;
                    case 1: cnmea.tvol =  atof(tBuff); break;
                    case 2: cnmea.gvol =  atof(tBuff); break;
                    case 3: cnmea.tank =  atof(tBuff); break;
                    case 4: cnmea.tds  =  atoi(tBuff); break;
                    case 5: cnmea.ttemp = atof(tBuff);
                            rval = 0; 
                            break;
                    default: rval = 1; break;
                }
            }
        }

        fclose(tankFd);
        unlink("/tmp/digiflow.txt");

    } else rval = 1;

    while (1) {
        sdlApp->textFieldArrIndx = 0;
        int boxItem = 0;
        char msg_tnk[40];
        char msg_lft[40];
        char msg_use[40];
        char msg_gtv[40];
        char msg_flr[60];
        char msg_tds[40];
        char msg_tmp[40];
        char msg_cns[40];
        char msg_tod[40];
        time_t ct;

        int doBreak = 0;
        
        while (SDL_PollEvent(&e)) {

            if (e.type == SDL_QUIT) {
                doBreak = 1;
                break;
            }

            if (e.type == SDL_FINGERDOWN || e.type == SDL_MOUSEBUTTONDOWN)
            {
                if ((e.type=pageSelect(sdlApp, &e))) {
                    doBreak = 1;
                    break;
                }
            }
        }
 
        if (doBreak == 1) break;

        SDL_RenderClear(sdlApp->renderer);

        ct = time(NULL);    // Get a timestamp for this turn 
        strftime(msg_tod, sizeof(msg_tod),TIMEDATFMT, localtime(&ct));

         if (rval == 1) {
            sprintf(msg_tnk, "----");
            sprintf(msg_lft, "----");
            sprintf(msg_flr, "----");
        } else {
            sprintf(msg_tnk, "%.0f", cnmea.tank);
            sprintf(msg_lft, "%.1f", cnmea.tank-cnmea.tvol);
            info_t = localtime( &cnmea.fdate);
            strftime(buffer_t, sizeof(buffer_t), "%Y-%m-%d", info_t);
            sprintf(msg_flr, "%s",  buffer_t);
            sprintf(msg_tmp, "TEMP: %.0f", cnmea.ttemp);
            sprintf(msg_tds, "TDS:  %d", cnmea.tds);
            float vleft = floor((((cnmea.tank-cnmea.tvol)/cnmea.tank)*100)+0.5);
            sprintf(msg_cns, "LEFT: %.0f%c", vleft, '%');
            sprintf(msg_gtv, "GTVL: %.0f", cnmea.gvol); 
            sprintf(msg_use, "USED: %.0f", cnmea.tvol);
        }
        
        SDL_RenderCopy(sdlApp->renderer, Background_Tx, NULL, NULL);
       
        SDL_RenderCopyEx(sdlApp->renderer, gaugeWtr, NULL, &gaugeR, 0, NULL, SDL_FLIP_NONE);

        get_text_and_rect(sdlApp->renderer, 196, 142, 3, msg_tnk, fontHD, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);
        SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
 
        get_text_and_rect(sdlApp->renderer, 136, 216, 9, msg_lft, fontLA, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);
        SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
 
        get_text_and_rect(sdlApp->renderer, 148, 292, 9, msg_flr, fontLO, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, cnmea.fdate < ct+604800 ? RED : BLACK); // A week+
        SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);

        if (rval == 0) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_cns, fontCog, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);

            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_use, fontCog, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);

            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_gtv, fontCog, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);

            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_tmp, fontCog, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);

            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_tds, fontCog, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);
        }

        SDL_RenderCopyEx(sdlApp->renderer, menuBar, NULL, &menuBarR, 0, NULL, SDL_FLIP_NONE);

        addMenuItems(sdlApp, fontSrc);

        get_text_and_rect(sdlApp->renderer, 580, 10, 0, msg_tod, fontTod, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, WHITE);
        SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);


        if (sdlApp->conf->netStat == 1) {
           SDL_RenderCopyEx(sdlApp->renderer, netStatBar, NULL, &netStatbarR, 0, NULL, SDL_FLIP_NONE);
        } else {
            SDL_RenderCopyEx(sdlApp->renderer, noNetStatbar, NULL, &netStatbarR, 0, NULL, SDL_FLIP_NONE);
        }

        if (sdlApp->conf->runWrn) {
            if (sdlApp->conf->muted == 0) {
                SDL_RenderCopyEx(sdlApp->renderer, muteBar, NULL, &mutebarR, 0, NULL, SDL_FLIP_NONE);
            } else {
                SDL_RenderCopyEx(sdlApp->renderer, unmuteBar, NULL, &mutebarR, 0, NULL, SDL_FLIP_NONE);
            }
        }

        if (boxItem) {
            textBoxR.h = boxItem*50 +30;
            SDL_RenderCopyEx(sdlApp->renderer, textBox, NULL, &textBoxR, 0, NULL, SDL_FLIP_NONE);
        }

        SDL_RenderPresent(sdlApp->renderer); 

        doVnc(sdlApp);

        SDL_Delay(1000);

        sdlApp->textFieldArrIndx--;
        do {
            SDL_DestroyTexture(sdlApp->textFieldArr[sdlApp->textFieldArrIndx]);
        } while (sdlApp->textFieldArrIndx-- >0);
    }

    SDL_DestroyTexture(gaugeWtr);
    SDL_DestroyTexture(menuBar);
    SDL_DestroyTexture(netStatBar);
    SDL_DestroyTexture(noNetStatbar);
    SDL_DestroyTexture(muteBar);
    SDL_DestroyTexture(unmuteBar);
    SDL_DestroyTexture(textBox);
    TTF_CloseFont(fontHD);
    TTF_CloseFont(fontLA);
    TTF_CloseFont(fontLO);
    TTF_CloseFont(fontMG);
    TTF_CloseFont(fontCog);
    TTF_CloseFont(fontSrc);
    TTF_CloseFont(fontTod);
 
    return e.type;
}
#endif /* DIGIFLOW */

static int threadCalibrator(void *ptr)
{

    calRunner *doRun = ptr;
    int magRaw[3];
    int fd;
    char buf[250];
    char dbuf[40];

    int magXmax = -32767;
    int magYmax = -32767;
    int magZmax = -32767;
    int magXmin = 32767;
    int magYmin = 32767;
    int magZmin = 32767;

    if (doRun->declination != 0.0)
        sprintf(dbuf, ", declval = %.2f;\n", doRun->declination); 
    else
        sprintf(dbuf, ";\n");

    while (doRun->run)
    {
        i2creadMAG(magRaw, doRun->i2cFile);
		sprintf(doRun->progress, "magXmax %4i magYmax %4i magZmax %4i magXmin %4i magYmin %4i magZmin %4i declination %.2f", \
                                  magXmax,magYmax,magZmax,magXmin,magYmin,magZmin,doRun->declination);

		if (magRaw[0] > magXmax) magXmax = magRaw[0];
		if (magRaw[1] > magYmax) magYmax = magRaw[1];
		if (magRaw[2] > magZmax) magZmax = magRaw[2];

		if (magRaw[0] < magXmin) magXmin = magRaw[0];
		if (magRaw[1] < magYmin) magYmin = magRaw[1];
		if (magRaw[2] < magZmin) magZmin = magRaw[2];

		//Sleep for 0.25ms
		usleep(25000);
    }   

    // To be picked up by i2cCollector thread
    sprintf(buf, "UPDATE calib SET magXmax = %i, magYmax = %i, magZmax = %i, magXmin = %i, magYmin = %i, magZmin = %i", \
                  magXmax,magYmax,magZmax,magXmin,magYmin,magZmin);

    strcat(buf, dbuf);
 
    if ((fd = open(SQLCONFIG, O_WRONLY | O_CREAT | O_TRUNC, (mode_t)0644)) >0) {
        write(fd, buf, strlen(buf));
        close(fd);
    }

    return 0;
}

// Do calibration
static int doCalibration(sdl2_app *sdlApp, configuration *configParams)
{
    SDL_Event e;
    SDL_Rect menuBarR;

    TTF_Font* fontCAL =  TTF_OpenFont(sdlApp->fontPath, 28);
    TTF_Font* fontPRG =  TTF_OpenFont(sdlApp->fontPath, 11);
    TTF_Font* fontSrc = TTF_OpenFont(sdlApp->fontPath, 14);

    SDL_Texture* menuBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "menuBar.png");

    CURL *curl = NULL;

    calRunner doRun;

    if (!configParams->i2cFile)
        return COGPAGE;

    doRun.run = 1;
    doRun.i2cFile = configParams->i2cFile;

    SDL_Rect textField_rect;

    menuBarR.w = 393;
    menuBarR.h = 50;
    menuBarR.x = 400;
    menuBarR.y = 400;

    int progress = 10;
    int seconds = 0;
    const int cperiod = 60;
    char msg_cal[100];

    doRun.latitude = doRun.longitude = doRun.declination = 0;

    // If the GPS is running OK, prepare for NOAA declination fetch
    if (!(time(NULL) - cnmea.gll_ts > S_TIMEOUT)) {
        doRun.latitude = dms2dd(atof(cnmea.gll),"m");
        doRun.longitude = dms2dd(atof(cnmea.glo),"m");
    }

    // If we are on-line, try to update the declination from NOAA
    if (doRun.latitude + doRun.longitude)
        curl = curl_easy_init();

    if (curl)
    {
        char buf[250];
        FILE *fd = NULL;        
        CURLcode res = 1;
        float declination = 0.0; 
        char *dcfile = "/tmp/declination.csv";
        char month[10];
        char year[10];
        time_t timer;
        struct tm* tm_info;

        time(&timer);
        tm_info = localtime(&timer);
        strftime(month, 3, "%m", tm_info);
        strftime(year, 5, "%Y", tm_info);

        char *url = "https://www.ngdc.noaa.gov/geomag-web/calculators/calculateDeclination?";
        sprintf(buf,"%slat1=%f&lon1=%f&resultFormat=csv&startMonth=%s&startYear=%s", url, doRun.latitude, doRun.longitude, month, year); 
        if ((fd = fopen(dcfile,"w+")) != NULL) {
            curl_easy_setopt(curl, CURLOPT_URL, buf);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1); 
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 6);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, fd);
            res = curl_easy_perform(curl);
            curl_easy_cleanup(curl);
            fclose(fd);
        }

        if (!res) {
            char cmd[80];
            memset(buf, 0, sizeof(buf));
            sprintf(cmd, "tail -1 %s | cut -d, -f5", dcfile);
            if (fread(buf, 1, 40, (fd=popen(cmd, "r"))) > 0) {
                declination = (M_PI/180)*atof(buf);
                if (declination != 0.0)
                        doRun.declination = declination;
            }
            pclose(fd);
        }
        unlink(dcfile);
    }  

    sprintf(doRun.progress, "Progress..");

    SDL_Thread *threadCalib = NULL;

    sprintf(msg_cal, "Calibration about to begin in %d seconds", progress);
    SDL_Log("%s", msg_cal);

    while (1) {

        sdlApp->textFieldArrIndx = 0;
        int doBreak = 0;
        
        while (SDL_PollEvent(&e)) {

            if (e.type == SDL_QUIT) {
                doBreak = 1;
                break;
            }

            if (e.type == SDL_FINGERDOWN || e.type == SDL_MOUSEBUTTONDOWN)
            {
                if ((e.type=pageSelect(sdlApp, &e))) {
                    doBreak = 1;
                    break;
                }
            }
        }
        if (doBreak == 1) break;

        SDL_RenderClear(sdlApp->renderer);

        SDL_RenderCopy(sdlApp->renderer, Background_Tx, NULL, NULL);


        if (seconds ++ > 10) {
            sprintf(msg_cal, "Calibration about to begin in %d seconds", progress--);
            seconds = 0;
            if (progress < 0) break;
        }  

        get_text_and_rect(sdlApp->renderer, 10, 250, 1, msg_cal, fontCAL, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);
        SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);

        SDL_RenderCopyEx(sdlApp->renderer, menuBar, NULL, &menuBarR, 0, NULL, SDL_FLIP_NONE);
        addMenuItems(sdlApp, fontSrc);

        SDL_RenderPresent(sdlApp->renderer); 
        
        SDL_Delay(100);

        sdlApp->textFieldArrIndx--;
        do {
            SDL_DestroyTexture(sdlApp->textFieldArr[sdlApp->textFieldArrIndx]);
        } while (sdlApp->textFieldArrIndx-- >0);
    }

    if (progress < 0) {

        progress = cperiod;   // Will unconditionally last for 'cperiod' seconds

        while (1) {

            sdlApp->textFieldArrIndx = 0;
            e.type = COGPAGE;

            if (threadCalib == NULL) {

                threadCalib = SDL_CreateThread(threadCalibrator, "commpassCalibrator", &doRun);

                if (NULL == threadCalib) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateThread commpassCalibrator failed: %s", SDL_GetError());
                    break;
                } else SDL_DetachThread(threadCalib);
            }

            SDL_RenderCopy(sdlApp->renderer, Background_Tx, NULL, NULL);

            if (seconds++ > 10) {
                sprintf(msg_cal, "Calibration in progress for %d more seconds", progress--);
                seconds = 0;
                if (progress < 0) {
                    break;
                }
            }

            SDL_RenderClear(sdlApp->renderer);

            get_text_and_rect(sdlApp->renderer, 10, 250, 1, msg_cal, fontCAL, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);

            get_text_and_rect(sdlApp->renderer, 10, 320, 1, doRun.progress, fontPRG, &sdlApp->textFieldArr[sdlApp->textFieldArrIndx], &textField_rect, BLACK);
            SDL_RenderCopy(sdlApp->renderer, sdlApp->textFieldArr[sdlApp->textFieldArrIndx++], NULL, &textField_rect);

            SDL_RenderPresent(sdlApp->renderer); 
            
            SDL_Delay(100); 

            sdlApp->textFieldArrIndx--;
            do {
                SDL_DestroyTexture(sdlApp->textFieldArr[sdlApp->textFieldArrIndx]);
            } while (sdlApp->textFieldArrIndx-- >0);

        } 
    }

    doRun.run = 0;  // Quit the thread

    SDL_Delay(1000); 
    SDL_Log("Calibration completed");

    SDL_DestroyTexture(menuBar);
    TTF_CloseFont(fontCAL);
    TTF_CloseFont(fontPRG);
    TTF_CloseFont(fontSrc);

    return e.type;
}

static void closeSDL2(sdl2_app *sdlApp)
{
    TTF_Quit();
    SDL_DestroyRenderer(sdlApp->renderer);
    SDL_DestroyWindow(sdlApp->window);
    SDL_VideoQuit();
    IMG_Quit();
    SDL_Quit();

}

static int openSDL2(configuration *configParams, sdl2_app *sdlApp, int doInit)
{
    SDL_Surface* Loading_Surf;
    SDL_Thread *threadNmea = NULL;
    SDL_Thread *threadI2C = NULL;
    SDL_Thread *threadGPS = NULL;
    SDL_Thread *threadVNC = NULL;
    SDL_Thread *threadWrn = NULL;
    configParams->conn = NULL; 
    Uint32 flags;

    if (doInit) {
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,  "Couldn't initialize SDL. Video driver %s!", SDL_GetError());
            return SDL_QUIT;
        }
  

        if (sqlite3_open_v2(SQLDBPATH, &configParams->conn, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, 0)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open configuration databas : %s", (char*)sqlite3_errmsg(configParams->conn));
            (void)sqlite3_close(configParams->conn);
            configParams->conn = NULL;
            return SDL_QUIT;
        }
        int flags = IMG_INIT_PNG | IMG_INIT_JPG;
        IMG_Init(flags);
    }

    if (configParams->runNet) {
        if (strncmp(configParams->server, "none", 4)) {
            threadNmea = SDL_CreateThread(nmeaNetCollector, "nmeaNetCollector", configParams);

            if (NULL == threadNmea) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateThread nmeaNetCollector failed: %s", SDL_GetError());
            } else SDL_DetachThread(threadNmea);
        } else
            configParams->runNet = 0;
    }

    if (configParams->runi2c) {
        threadI2C = SDL_CreateThread(i2cCollector, "i2cCollector", configParams);

        if (0 == threadI2C) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateThread threadI2C failed: %s", SDL_GetError());
        } else SDL_DetachThread(threadI2C);
    }

    if (configParams->runGps) {
        if (strncmp(configParams->tty, "none", 4)) {
            threadGPS = SDL_CreateThread(threadSerial, "threadGPS", configParams);

            if (NULL == threadGPS) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateThread threadGPS failed: %s", SDL_GetError());
            } else SDL_DetachThread(threadGPS);
        } else
         configParams->runGps = 0;   
    }

    if (configParams->runVnc == 1) {
        threadVNC = SDL_CreateThread(threadVnc, "threadVNC", sdlApp);
        if (NULL == threadVNC) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateThread threadVNC failed: %s", SDL_GetError());
             configParams->runVnc = 0; 
        } else { SDL_DetachThread(threadVNC);  configParams->runVnc = 2; }
    }

    if (configParams->runWrn) {
        if (!configParams->snd_useMixer) {
            configParams->runWrn = 0;
        } else {
            threadWrn = SDL_CreateThread(threadWarn, "threadWarn", configParams);

            if (NULL == threadWrn) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateThread threadWarn failed: %s", SDL_GetError());
                configParams->runWrn = 0;
            } else SDL_DetachThread(threadWrn);
        }
    }

    if (doInit) {
        flags = configParams->useWm == 1? SDL_WINDOW_BORDERLESS | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_ALWAYS_ON_TOP : 0;

        if ((sdlApp->window = SDL_CreateWindow("sdlSpeedometer",
                0, 0, // Pos x/y
                configParams->window_w, configParams->window_h,
                flags)) == NULL) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateWindow failed: %s", SDL_GetError());
                configParams->runGps = configParams->runi2c = configParams->runNet = configParams->runWrn = 0;
                return SDL_QUIT;
        }

        SDL_ShowCursor(configParams->cursor == 1? SDL_ENABLE : SDL_DISABLE);

        if (configParams->useWm == 1) {
            SDL_SetWindowBordered( sdlApp->window, SDL_FALSE );
        }
    }

    sdlApp->renderer = SDL_CreateRenderer(sdlApp->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    SDL_RenderSetScale(sdlApp->renderer, configParams->scale, configParams->scale);

    TTF_Init();

    Loading_Surf = SDL_LoadBMP(DEFAULT_BACKGROUND);
    Background_Tx = SDL_CreateTextureFromSurface(sdlApp->renderer, Loading_Surf);
    SDL_FreeSurface(Loading_Surf);

    SDL_SetWindowAlwaysOnTop(sdlApp->window, SDL_TRUE);

    return 0;
}

// Check if subtask is within PATH.
static int checkSubtask(sdl2_app *sdlApp, configuration *configParams)
{
    char subtask[PATH_MAX];
    char buff[PATH_MAX];
    int rval;
    FILE *fd;
    sqlite3_stmt *res;
    const char *tail;

    if (sdlApp->subAppsCmd[0][0] == NULL) {
        if (configParams->conn == NULL) {
           return 0;
        }

        int c = 1;
        if ((rval=sqlite3_prepare_v2(configParams->conn, "select task,args,icon from subtasks", -1, &res, &tail)) == SQLITE_OK)
        {
            while (sqlite3_step(res) != SQLITE_DONE) {  
                strncpy((sdlApp->subAppsCmd[c][0]=(char*)malloc(PATH_MAX)), (char*)sqlite3_column_text(res, 0), PATH_MAX);
                strncpy((sdlApp->subAppsCmd[c][1]=(char*)malloc(PATH_MAX)), (char*)sqlite3_column_text(res, 1), PATH_MAX);
                strncpy((sdlApp->subAppsIco[c][0]=(char*)malloc(PATH_MAX)), (char*)sqlite3_column_text(res, 2), PATH_MAX);

                sprintf(subtask, "which %s", sdlApp->subAppsCmd[c][0]);
 
                *buff = '\0';

                if ((fd = popen(subtask, "r")) != NULL) {
                    fgets(buff, sizeof(buff) , fd);
                    pclose(fd);
                }
                if (strlen(buff) < 2) {
                    free(sdlApp->subAppsCmd[c][0]);
                    free(sdlApp->subAppsCmd[c][1]);
                    free(sdlApp->subAppsIco[c][0]);
                    sdlApp->subAppsCmd[c][0] = NULL;
                }

                if (c++ >= VIDPAGE) {
                    break;
                }
            }

            if (c >1) {
                sdlApp->subAppsCmd[0][0] = "1"; // Checked
            }

            sqlite3_finalize(res);
        }
    }

    if (sdlApp->subAppsCmd[sdlApp->curPage][0] == NULL)
        return 0;

    return 1;
}

// Give up all resources in favor of a subtask execution.
static int doSubtask(sdl2_app *sdlApp, configuration *configParams)
{
    int runners[4];
    int t_wmax = 4;
    int status, i=0;
    char *args[20];
    char cmd[1024];

    if (!checkSubtask(sdlApp, configParams))
        return COGPAGE;

    sprintf(cmd, "/bin/bash %s %s %s", SPAWNCMD, sdlApp->subAppsCmd[sdlApp->curPage][0], sdlApp->subAppsCmd[sdlApp->curPage][1]);

    SDL_Log("Launch subcommand: %s", cmd);

    // Break up cmd to an argv array
    args[i] = strtok(cmd, " ");
    while(args[i] != NULL)
        args[++i] = strtok(NULL, " ");

    // Take down threads and all of SDL2
    runners[0] = configParams->runGps;
    runners[1] = configParams->runi2c;
    runners[2] = configParams->runNet;
    runners[3] = configParams->runWrn;
    configParams->runGps = configParams->runi2c = configParams->runNet = configParams->runWrn = 0;

    while(configParams->numThreads && t_wmax--) {
        SDL_Delay(350*configParams->numThreads);
        if (configParams->numThreads) {
            SDL_Log("Taking down threads #%d remains", configParams->numThreads);
        }
    }

    if (configParams->numThreads) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to take down all threads: %d remains.", configParams->numThreads);
    }

    SDL_DestroyRenderer(sdlApp->renderer);
    
    configParams->subTaskPID = fork ();

    if (configParams->subTaskPID == 0) {
        // Child
        setpgid(0, 0);
        prctl(PR_SET_PDEATHSIG, SIGINT);
        execv ("/bin/bash", args);
        _exit(1);  // fallback if exec fails
    }

    // You've picked my bones clean, speak now ...
    waitpid(configParams->subTaskPID, &status, 0);
    // ... before I reclaim the meat. (Solonius)

    configParams->subTaskPID = 0;

    // Regain SDL2 control
    configParams->runGps = runners[0];
    configParams->runi2c = runners[1];
    configParams->runNet = runners[2];
    configParams->runWrn = runners[3];

    status = openSDL2(configParams, sdlApp, 0);

    if (status == 0)    
        status = sdlApp->curPage;

    return status;
}

static void init_av(configuration *configParams)
{
    const char *adev = configParams->snd_card;  // e.g., "hw:2,0" for SDL2
    char card[sizeof(configParams->snd_card)];

    if (adev) {
        // copy up to comma or end of string
        const char *comma = strchr(adev, ',');
        if (comma) {
            size_t len = comma - adev;
            if (len >= sizeof(card)) len = sizeof(card)-1;
            strncpy(card, adev, len);
            card[len] = '\0';
        } else {
            strncpy(card, adev, sizeof(card)-1);
            card[sizeof(card)-1] = '\0';
        }
    } else {
        strcpy(card, "default");
    }

    snd_mixer_open(&configParams->mixer, 0);
    snd_mixer_attach(configParams->mixer, card);
    snd_mixer_selem_register(configParams->mixer, NULL, NULL);
    snd_mixer_load(configParams->mixer);

    snd_mixer_selem_id_t *sid;
    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, "PCM");

    configParams->elem = NULL;

    configParams->elem = snd_mixer_find_selem(configParams->mixer, sid);

    if (!configParams->elem) {
        configParams->snd_useMixer = 0;
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize ALSA sound: Cannot find mixer element for %s", adev);
    } else {
        setenv("SDL_AUDIODRIVER", "alsa", 1);
        SDL_Init(SDL_INIT_AUDIO);
        configParams->snd_useMixer = 1;
        configParams->snd_showMixer = 1;
        snd_mixer_selem_get_playback_volume_range(configParams->elem, &configParams->snd_minv, &configParams->snd_maxv);

        const char *target_card_num = 0;
        const char *colon = strchr(card, ':');
        if (colon && *(colon + 1)) {
            target_card_num = colon + 1;
        }

        int cardn = -1;
        char sdl_compat_name[256] = {'\0'};

        while (snd_card_next(&cardn) == 0 && cardn >= 0) {
            if (cardn == atoi(target_card_num)) {
                char *card_short;
                snd_card_get_name(cardn, &card_short);

                // Find PCM name
                char hw_name[60];
                sprintf(hw_name, "hw:%d", cardn);

                snd_ctl_t *handle;
                if (snd_ctl_open(&handle, hw_name, 0) >= 0) {
                    snd_pcm_info_t *pcminfo;
                    snd_pcm_info_alloca(&pcminfo);

                    // Check unit (0) to get the name
                    snd_pcm_info_set_device(pcminfo, 0);
                    snd_pcm_info_set_subdevice(pcminfo, 0);
                    snd_pcm_info_set_stream(pcminfo, SND_PCM_STREAM_PLAYBACK);

                    if (snd_ctl_pcm_info(handle, pcminfo) >= 0) {
                        const char *device_name = snd_pcm_info_get_name(pcminfo);
                        // Create the the format "ShortName, UnitName"
                        // Example: "USB Audio, USB Audio"
                        snprintf(sdl_compat_name, sizeof(sdl_compat_name), "%s, %s", card_short, device_name);
                    }
                    snd_ctl_close(handle);
                }

                if (strlen(sdl_compat_name)) {
                    if (strstr(sdl_compat_name, "hdmi")) configParams->snd_showMixer = 0; // Goes to decoder/tv mixer
                    strncpy(configParams->snd_card_name, sdl_compat_name, sizeof(((configuration *)0)->snd_card_name));
                    SDL_Log("Using ALSA device %s named \"%s\"", card, configParams->snd_card_name);
                } else {
                    configParams->snd_showMixer = 0;
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Unable to find audio device name for %s", card);
                }

                free(card_short);
                break;
            }
        }

    }

    /** Video **/
    int fd=0;
    int ndev;
    struct v4l2_capability caps;
    struct v4l2_input input;
    char dev_name[100] = {'\0'};

    for (ndev=0; ndev <64; ndev++) {
        sprintf(configParams->vid_device, "/dev/video%d", ndev);
        if ((fd = open(configParams->vid_device, O_RDWR)) <0) {
            continue;
        }

        memset(&input, 0, sizeof(input));
        input.index = 0;

        if (ioctl(fd, VIDIOC_ENUMINPUT, &input) == 0) {
            sprintf(dev_name, "Found Video Capture - %s: ", (char*)input.name);
            if (ioctl(fd, VIDIOC_QUERYCAP, &caps) == 0) {
                if ((caps.device_caps & (V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_VIDEO_OUTPUT_MPLANE))) {
                    close(fd);
                    continue;
                }
                strcat(dev_name, (char*)caps.card);
            }
            close(fd); 
            break;
        }
        close(fd);
    }
    if (ndev >= 63) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No video capture card found");
        *configParams->vid_device = 0;
    } else {
        SDL_Log("%s\n", dev_name);
    }
}

int main(int argc, char *argv[])
{
    int c, t_wmax = 4;
    configuration configParams;
    sdl2_app sdlApp;
    char buf[FILENAME_MAX];
    struct stat stats;

    memset(&cnmea, 0, sizeof(cnmea));
    memset(&sdlApp, 0, sizeof(sdlApp));
    memset(&configParams, 0, sizeof(configParams));
    sdlApp.conf = &configParams;

    sdlApp.fontPath = DEFAULT_FONT;

    SDL_LogSetOutputFunction((void*)logCallBack, argv[0]);   

    if (getenv("DISPLAY") != NULL) {    // Wait for X/Xweston to become ready
        int i;
        for (i = 0; i < 5; i++) {
            Display *display;
            if ((display = XOpenDisplay(NULL)) == NULL) {
                sleep(2);
                continue;
            }
            XWindowAttributes wa;
            XGetWindowAttributes(display, DefaultRootWindow(display), &wa);
            sprintf(configParams.ssize, "%dx%d", wa.width, wa.height);
            XCloseDisplay(display);
            break;
        }
        if (i >= 5) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s: No response from X/Xweston. Terminating now!\n", argv[0]);
            return 1;
        }
    }  else {
        strcpy(configParams.ssize, DEFAULT_SCREEN_SIZE);
    }

    configParams.scale = DEFAULT_SCREEN_SCALE;

    configParams.runGps = configParams.runi2c = configParams.runNet = 1;
        
    sdlApp.nextPage = COGPAGE; // Start-page

    if (configureDb(&configParams) != SQLITE_OK) {  // Fetch configuration
        exit(EXIT_FAILURE);
    }

    while ((c = getopt (argc, argv, "cC:hlvginwVpPs:z:")) != -1)
    {
        switch (c)
            {
            case 'l':
                useSyslog = 1;
                break;
            case 'c':   exit(EXIT_SUCCESS);         // Check/Create databasse only and exit
                break;
            case 'C':   configParams.runTyd = atoi(optarg);    // Remote configuration shell
                break;
            case 'g':   configParams.runGps = 0;    // Diable GPS data collection
                break;
            case 'i':   configParams.runi2c = 0;    // Disable i2c data collection
                break;
            case 'n':   configParams.runNet = 0;    // Disable NMEA net data collection
                break;
            case 'w':   configParams.useWm  = 1;    // Use private WM
                break;
            case 'V':   configParams.runVnc = 1;    // Enable VNC server
                break;
            case 'p':   configParams.runWrn = 1;    // Play warning sounds
                break;
            case 'P':   configParams.cursor = 1;    // Show pointer cursor
                break;
            case 's':   strncpy(configParams.ssize, optarg, sizeof(configParams.ssize));    // Screen size w/h
                break;
            case 'z':   configParams.scale = atof(optarg);    // Scale the screen
                break;
            case 'v':
                fprintf(stderr, "revision: %s\n", SWREV);
                exit(EXIT_SUCCESS);
                break;
            case 'h':
            default:
                fprintf(stderr, "Usage: %s -l -c -C -g -i -n -p -P -V -w -z -s -v (version)\n", basename(argv[0]));
                fprintf(stderr, "       Where: -l use syslog : -c Create database only: -P show cursor : -g Disable GPS : -i Disable i2c : -p Play warnings\n");
                fprintf(stderr, "              -n Disabe NMEA Net : -w use WM : -V Enable VNC Server : -C (port>1024) Remote config : -z Scale factor : -s Window size w/h\n");
                exit(EXIT_FAILURE);
                break;
            }
    }

    {
        // Resolve -s option
        const char s[2] = "x";
        char *token;
        int first = 0;
        /* get the first token */
        token = strtok(configParams.ssize, s);

        /* walk through other tokens */
        while( token != NULL ) {
            if (first++ == 0)
                configParams.window_w = atoi(token);
            else
                configParams.window_h = atoi(token);

          token = strtok(NULL, s);
        }
    }

    init_av(&configParams);

    if (useSyslog) {
        setlogmask (LOG_UPTO (LOG_NOTICE));
        openlog (basename(argv[0]), LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);
        syslog (LOG_NOTICE, "Program started by User %d", getuid ());
    }

    if (configParams.runVnc == 1) {
        // Create an empty RGB surface that will be used to hold the VNC pixel buffer
        configParams.vncPixelBuffer = SDL_CreateRGBSurface(0, configParams.window_w, configParams.window_h, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000); 
        if (configParams.vncPixelBuffer != NULL) {
            rfbErr=SDL_Log; 
            rfbLog=SDL_Log;
            configParams.vncServer=rfbGetScreen(&argc, argv, configParams.window_w, configParams.window_h, 8, 3, 4);           
            configParams.vncServer->frameBuffer = configParams.vncPixelBuffer->pixels;
            configParams.vncServer->ipv6port = 0;
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateRGBSurface failed: %s. VNC Server disabled!", SDL_GetError());
            configParams.runVnc = 0;
        }
    }

    sprintf(buf, "%d", configParams.window_h); SDL_setenv("WINDOW_W", buf, 0);
    sprintf(buf, "%d", configParams.window_w); SDL_setenv("WINDOW_H", buf, 0);

    if (SDL_getenv("DISPLAY") != NULL) {

        SDL_setenv("SDL_VIDEODRIVER", "x11", 0);
        SDL_Log("Using X11/Xweston Videodriver");

        pid_t pid0, pid1=0, pid2;
        int status;

        if (configParams.useWm  == 1 && system("xprop -root|grep -q _NET_CLIENT_LIST >/dev/null 2>&1") != 0) {
            // Deprecated Xorg section

            sprintf(buf, "xrandr -s %dx%d &>/dev/null", configParams.window_w, configParams.window_h);
            system(buf);

            pid0 = fork();

            if (pid0 == 0) {
                // Disabe decorations from wm.
                SDL_Log("Attempt to start devilspie2");
                char *args[] = { "/usr/bin/devilspie2", "-f", "/usr/local/etc/devilspie2",  NULL }; 
                execvp(args[0], args);
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to execute  %s: %s (non fatal)\n", args[0], strerror(errno));
                _exit(0);
            }           

            if (waitpid(pid0,&status,WNOHANG) == 0) {

                pid1 = fork();

                if (pid1 == 0) {

                    pid_t pidWmMgr;

                    for (int i = 1; i < 8; i++) {

                        SDL_Log("Attempt to start a window manager");

                        pidWmMgr = fork();

                        if (pidWmMgr == 0) {

                            int fdnull;

                            // close stdin, stderr, stdout
                            if ((fdnull = open("/dev/null", O_RDWR)) > 0)
                            {
                                dup2 (fdnull, STDIN_FILENO);
                                dup2 (fdnull, STDOUT_FILENO);
                                dup2 (fdnull, STDERR_FILENO);
                                close(fdnull);
                            }

                            char *args[] = { "/usr/bin/xfwm4", "--sm-client-disable", "--compositor=off", NULL };
                            execvp(args[0], args);
                            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to execute  %s: %s (non fatal)\n", args[0], strerror(errno));
                            _exit(1);
                        }

                        for (int i = 0; i < 5; i++) {
                            sleep(1);
                            // Make sure to be on top if wm restarts
                            if (!system("wmctrl -a sdlSpeedometer >/dev/null 2>&1;")) {
                                break;
                            }
                        }

                        if (waitpid(pidWmMgr,&status,WUNTRACED) != 0) {
                            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "The window manager died unexpectedly. Retry  #%d", i);
                            sleep(2);
                        }
                    }
                    _exit(0);
                }
            }

            sleep(2);

            if (pid1>0 && waitpid(pid1,&status,WNOHANG) != 0) {
                kill(pid0, SIGINT);
                sleep(1);
                waitpid(pid0,&status,WNOHANG);
            }

            sleep(1);

            sprintf(buf, "/usr/local/share/images/splash-%dx%d.png", configParams.window_w, configParams.window_h);

            if (stat(buf, &stats) == 0) {

                pid2 = fork();

                if (pid2 == 0 ) {
                    // Start the splashscreen
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Attempt to initiate the splash screen");
                    char *args[] = { "/usr/bin/xloadimage", "-quiet", "-fullscreen", buf, NULL };
                    execvp(args[0], args);
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to execute  %s %s : %s (non fatal)", args[0], buf, strerror(errno));
                    _exit(0);
                }
            } else {
                SDL_Log("Splash file \"%s not found\"\n", buf);
            }

        }  else {
            if (configParams.useWm == 1) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "A window manager is already running. The -w option is disabled.");
                configParams.useWm = 0;
            }
        }

    } else { 
        if (SDL_getenv("WAYLAND_DISPLAY") != NULL) {
                SDL_setenv("SDL_VIDEODRIVER", "wayland", 0);
                SDL_Log("Using Wayland Videodriver");
                configParams.useWm = 0;
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Neither x11 or wayland are available as SDL_VIDEODRIVER");
                exit(1);
            }
    }

    if (configParams.runTyd > 1024) {

        SDL_Log("Attempt to start the ttyd daemon for remote configurtion");

        pid_t pid = fork();

        if (pid == 0 ) {

            // Redirect stdout to /dev/null

            int fd = open("/dev/null", O_RDWR);
            if (fd >= 0) {
               // dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                dup2(fd, STDIN_FILENO);
                close(fd);
            }

			setenv("DISPLAY", ":0", 1);	// May be required by subtasks although not by ttyd itself.

            // Start the ttyd
            sprintf(buf, "%d", configParams.runTyd);
            char *args[] = { "/usr/bin/ttyd", "-p", buf, "--writable", CONFICMD, NULL };
            execvp(args[0], args);
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to execute  %s : %s (non fatal)", args[0], strerror(errno));
            _exit(0);
        }

        configParams.ttydPID = pid;
    }

    if (openSDL2(&configParams, &sdlApp, 1))
        exit(EXIT_FAILURE);

    (void)checkSubtask(&sdlApp, &configParams);

    if (configParams.runVnc) {
        rfbLog=(rfbLogProc)nullLog;
    }

    while(1)
    {
        SDL_Delay(200);
        SDL_FlushEvents(SDL_FINGERDOWN, SDL_FINGERMOTION);

        switch (sdlApp.nextPage)
        {
            case COGPAGE: sdlApp.nextPage = doCompass(&sdlApp);
                break;
            case SOGPAGE: sdlApp.nextPage = doSumlog(&sdlApp);
                break;
            case DPTPAGE: sdlApp.nextPage = doDepth(&sdlApp);
                break;
            case WNDPAGE: sdlApp.nextPage = doWind(&sdlApp);
                break;
            case GPSPAGE: sdlApp.nextPage = doGps(&sdlApp);
                break;
            case PWRPAGE: sdlApp.nextPage = doEnvironment(&sdlApp);
                break;
#ifdef DIGIFLOW
            case WTRPAGE: sdlApp.nextPage = doWater(&sdlApp);
                break;
#endif
            case VIDPAGE: sdlApp.nextPage = doVideo(&sdlApp);
                break;
            case CALPAGE: sdlApp.nextPage = doCalibration(&sdlApp, &configParams);
                break;
            case TSKPAGE: sdlApp.nextPage = doSubtask(&sdlApp, &configParams);
                break;
            default: sdlApp.nextPage = COGPAGE;
                break;
        }
        if (sdlApp.nextPage == SDL_QUIT)
            break;
    }

    // Terminate the threads
    if (configParams.runVnc) {
        rfbShutdownServer(configParams.vncServer, TRUE);
        if (configParams.vncPixelBuffer != NULL)
            SDL_FreeSurface(configParams.vncPixelBuffer);
    }

    configParams.runGps = configParams.runi2c = configParams.runNet = configParams.runWrn = 0;

    // .. and let them close cleanly
    while(configParams.numThreads && t_wmax--) {
        SDL_Delay(1000);
        if (configParams.numThreads) {
            SDL_Log("Taking down threads #%d remains", configParams.numThreads);
        }
    }

    if (configParams.numThreads) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to take down all threads: %d remains.", configParams.numThreads);
    }
    
    closeSDL2(&sdlApp);

    if (configParams.ttydPID) {
        kill(configParams.ttydPID, SIGINT);
        SDL_Log("Taking down remote config process");
    }

    SDL_Log("User terminated");

    exit(EXIT_SUCCESS);
}
