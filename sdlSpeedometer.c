/*
 * sdlSpeedometer.c
 *
 *  Copyright (C) 2018 by Erland Hedman <erland@hedmanshome.se>
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
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_net.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <curl/curl.h>
#include <termios.h>
#include <libgen.h>
#include <sys/prctl.h>
#include <time.h>
#include <syslog.h>
#include <errno.h>

#include "sdlSpeedometer.h"

#define DEF_NMEA_SERVER "rpi3.hedmanshome.se"   // A test site running 24/7
#define DEF_NMEA_PORT   10110
#define DEF_VNC_PORT    5903

#define TIMEDATFMT  "%x - %H:%M %Z"

#define WINDOW_W 800        // Resolution
#define WINDOW_H 480

#define S_TIMEOUT   4       // Invalidate current sentences after # seconds without a refresh from talker.
#define TRGPS       2.5     // Min speed to be trusted as real movement from GPS RMC
#define NMPARSE(str, nsent) !strncmp(nsent, &str[3], strlen(nsent))

#define DEFAULT_FONT        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf";

#define TTY_GPS             "/dev/ttyS0"    // RPI 3

#ifdef REV
#define SWREV REV
#else
#define SWREV __DATE__
#endif

#define SQLCONFIG "/tmp/sqlConfig.txt"  // Internal or External inject into config db.

#ifndef PATH_INSTALL
#define IMAGE_PATH  "./img/"
#define SQLDBPATH   "speedometer.db"
#define SPAWNCMD    "./spawnSubtask"
#else
#define IMAGE_PATH  "/usr/local/share/images/"
#define SQLDBPATH   "/usr/local/etc/speedometer.db"
#define SPAWNCMD    "/usr/local/bin/spawnSubtask"
#endif

#define DEFAULT_BACKGROUND IMAGE_PATH "Default-bg.bmp"

#define BLACK   1
#define WHITE   2
#define RED     3
#define DWRN    10  // Turn RED at depth < 10

static int useSyslog = 0;

static SDL_Texture* Background_Tx;

static collected_nmea cnmea;

void logCallBack(char *userdata, int category, SDL_LogPriority priority, const char *message)
{
    FILE *out = priority == SDL_LOG_PRIORITY_ERROR? stderr : stdout;

    if (useSyslog)
        syslog (LOG_NOTICE, message, getuid ());
    else
        fprintf(out, "[%s] %s\n", basename(userdata), message);
}

// The configuration database
int  configureDb(configuration *configParams)
{
    sqlite3 *conn;
    sqlite3_stmt *res;
    const char *tail;
    char buf[250];
    int rval = 0;
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

                    sqlite3_prepare_v2(conn, "CREATE TABLE config (Id INTEGER PRIMARY KEY, rev TEXT, tty TEXT, baud INTEGER, server TEXT, port INTEGER, vncport INTEGER)", -1, &res, &tail);
                    sqlite3_step(res);

                    sprintf(buf, "INSERT INTO config (rev,tty,baud,server,port,vncport) VALUES ('%s','%s',9600,'%s',%d,%d)", SWREV,TTY_GPS, DEF_NMEA_SERVER, DEF_NMEA_PORT, DEF_VNC_PORT);
                    sqlite3_prepare_v2(conn, buf, -1, &res, &tail);
                    sqlite3_step(res);

                    sqlite3_prepare_v2(conn, "CREATE TABLE calib (Id INTEGER PRIMARY KEY, magXmax INTEGER, magYmax INTEGER, magZmax INTEGER, magXmin INTEGER, magYmin INTEGER, magZmin INTEGER, declval REAL)", -1, &res, &tail);
                    sqlite3_step(res);
                    sprintf(buf, "INSERT INTO calib (magXmax,magYmax,magZmax,magXmin,magYmin,magZmin,declval) VALUES (%d,%d,%d,%d,%d,%d,%.2f)", \
                        dmagXmax,dmagYmax,dmagZmax,dmagXmin,dmagYmin,dmagZmin,ddeclval); 
                    sqlite3_prepare_v2(conn, buf , -1, &res, &tail);
                    sqlite3_step(res);

                    sqlite3_prepare_v2(conn, "CREATE TABLE subtasks (Id INTEGER PRIMARY KEY, task TEXT, args TEXT)", -1, &res, &tail);
                    sqlite3_step(res);
                    sprintf(buf, "INSERT INTO subtasks (task,args) VALUES ('opencpn','-fullscreen')");
                    sqlite3_prepare_v2(conn, buf, -1, &res, &tail);
                    sqlite3_step(res);
                    sprintf(buf, "INSERT INTO subtasks (task,args) VALUES ('notyet','')");
                    sqlite3_prepare_v2(conn, buf, -1, &res, &tail);
                    sqlite3_step(res);
                    sprintf(buf, "INSERT INTO subtasks (task,args) VALUES ('notyet','')");
                    sqlite3_prepare_v2(conn, buf, -1, &res, &tail);
                    sqlite3_step(res);
                    sprintf(buf, "INSERT INTO subtasks (task,args) VALUES ('zyGrib','')");
                    sqlite3_prepare_v2(conn, buf, -1, &res, &tail);
                    sqlite3_step(res);
                    sprintf(buf, "INSERT INTO subtasks (task,args) VALUES ('xterm','-geometry 132x20 -e sdlSpeedometer-config')");
                    sqlite3_prepare_v2(conn, buf, -1, &res, &tail);
                    sqlite3_step(res);
                    sprintf(buf, "INSERT INTO subtasks (task,args) VALUES ('notyet','')");
                    sqlite3_prepare_v2(conn, buf, -1, &res, &tail);
                    sqlite3_step(res);
                    sprintf(buf, "INSERT INTO subtasks (task,args) VALUES ('notyet','')");
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
    rval = sqlite3_prepare_v2(conn, "select tty,baud,server,port,vncport from config", -1, &res, &tail);        
    if (rval == SQLITE_OK && sqlite3_step(res) == SQLITE_ROW) {
        strcpy(configParams->tty,       (char*)sqlite3_column_text(res, 0));
        configParams->baud =            sqlite3_column_int(res, 1);
        strcpy(configParams->server,    (char*)sqlite3_column_text(res, 2));
        configParams->port =            sqlite3_column_int(res, 3);
        configParams->vncPort =         sqlite3_column_int(res, 4);
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to fetch configutation from database: %s", (char*)sqlite3_errmsg(conn));
    }

    rval = SQLITE_BUSY;
    
    while(rval == SQLITE_BUSY) { // make sure ! busy ! db locked 
        rval = SQLITE_OK;
        sqlite3_stmt * res = sqlite3_next_stmt(conn, NULL);
        if (res != NULL) {
            rval = sqlite3_finalize(res);
            if (rval == SQLITE_OK) {
                rval = sqlite3_close(conn);
            }
        }
    }
    (void)sqlite3_close(conn);

    return 0;
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


// Watch the presence of an HDMI display
static int threadMonstat(void *conf)
{
    configuration *configParams = conf;
    char *cmd = {"/opt/vc/bin/tvservice -d /dev/null 2>/dev/null"};
    char buf[40];
    FILE *fd;

    configParams->onHold = 0;
    
    // Use get EDID to determine if the display is present i.e. on/off
    while (configParams->runMon) {
        memset(buf, 0, sizeof(buf));
        if ((fd = popen(cmd, "r")) != NULL) {
            if ((fread(buf, 1, sizeof(buf), fd)) > 7) {
                if (!strncmp("Written", buf, 7)) {
                    configParams->onHold = 0;
                }
                else {
                    configParams->onHold = 1;
                }
            }
            pclose(fd);
        }
        SDL_Delay(5000);
    }

    SDL_Log("threadMonstat stopped");

    return 0;
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

#define MAX_LONGITUDE 180
#define MAX_LATITUDE   90

float dms2dd(float coordinates, const char *val)
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

        if (configParams->onHold) {
            SDL_Delay(4000);
            continue;
        }

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

            // HDT - Heading - True
            if (NMPARSE(buffer, "HDT")) {
                cnmea.hdm=atof(getf(1, buffer));
                cnmea.hdm_ts = ct;
                continue;
            }

            // HDG - Heading - Deviation and Variation 
            if (NMPARSE(buffer, "HDG")) {
                cnmea.hdm=atof(getf(1, buffer));
                cnmea.hdm_ts = ct;
                continue;
            }

            // HDM Heading - Heading Magnetic
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
        return 0;
    }

    configParams->conn = NULL;

    SDL_Log("Starting up i2c collector");

    if (sqlite3_open_v2(SQLDBPATH, &configParams->conn, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, 0)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open configuration databas : %s", (char*)sqlite3_errmsg(configParams->conn));
        (void)sqlite3_close(configParams->conn);
        configParams->conn = NULL;
    }

    configParams->numThreads++;

    while(configParams->runi2c)
    {
        time_t ct;
        float hdm;

        if (configParams->onHold) {
            SDL_Delay(4000);
            continue;
        }

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
                    rval = sqlite3_prepare_v2(configParams->conn, "select magXmax,magYmax,magZmax,magXmin,magYmin,magZmin,declval from calib", -1, &res, &tail);        
                    if (rval == SQLITE_OK && sqlite3_step(res) == SQLITE_ROW) {
                        // See: BerryIMU/compass_tutorial03_calibration
                        calib.magXmax = sqlite3_column_int(res, 0);
                        calib.magYmax = sqlite3_column_int(res, 1);
                        calib.magZmax = sqlite3_column_int(res, 2);
                        calib.magXmin = sqlite3_column_int(res, 3);
                        calib.magYmin = sqlite3_column_int(res, 4);
                        calib.magZmin = sqlite3_column_int(res, 5);
                        calib.declval = sqlite3_column_double(res, 6);
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
        cnmea.roll = i2cReadRoll(configParams->i2cFile, dt);

        // Take over if no NMEA
        if (ct - cnmea.hdm_ts > S_TIMEOUT) {
            //cnmea.hdm=hdm;
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

        if (configParams->onHold) {
            SDL_Delay(4000);
            continue;
        }

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

            if (configParams->onHold)
                break;

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
                    } else if (strncmp(getf(2, nmeastr_p1),"T",1) + strncmp(getf(4, nmeastr_p1),"N",1) == 0) {
                        cnmea.vwta=atof(getf(1, nmeastr_p1));
                        cnmea.vwts=atof(getf(3, nmeastr_p1))/1.94; // kn 2 m/s;
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

        if (configParams->runNet && configParams->onHold == 0)
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
        touchEvent.tfinger.x = (float)x/(float)WINDOW_W;
        touchEvent.tfinger.y = (float)y/(float)WINDOW_H;
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
    sdlApp->conf->vncServer->cursor = NULL;
    sdlApp->conf->vncServer->newClientHook = vncNewclient;
    sdlApp->conf->vncServer->screenData = sdlApp;
    sdlApp->conf->vncServer->ptrAddEvent = vncClientTouch;
    sdlApp->conf->vncServer->port = sdlApp->conf->vncPort;

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
static void get_text_and_rect(SDL_Renderer *renderer, int x, int y, int l, char *text,
        TTF_Font *font, SDL_Texture **texture, SDL_Rect *rect, int color) {
    int text_width;
    int text_height;
    int f_width;
    int f_height;
    SDL_Surface *surface;
    SDL_Color textColor;

    if (text == NULL || !strlen(text))
        return;

    switch (color)
    {
        case BLACK: textColor.r = textColor.g = textColor.b = 0; break;
        case WHITE: textColor.r = textColor.g = textColor.b = 255; break;
        case RED:   textColor.r = 255; textColor.g = textColor.b = 0; break;
    }

    surface = TTF_RenderText_Solid(font, text, textColor);
    *texture = SDL_CreateTextureFromSurface(renderer, surface);
    text_width = surface->w;
    text_height = surface->h;
    SDL_FreeSurface(surface);

    // Get the width of one ch of the font used
    TTF_SizeText(font,"0", &f_width, &f_height);

    if (l >1)
        rect->x = x + abs((strlen(text)-l)*f_width)/2;  // Align towards (l)
    else
       rect->x = x;
    rect->y = y;
    rect->w = text_width;
    rect->h = text_height;
}

static int pageSelect(sdl2_app *sdlApp, SDL_Event *event)
{
    // A simple event handler for touch screen buttons at fixed menu bar localtions

    // Upside down screen
    //int x = WINDOW_W -(event->tfinger.x* WINDOW_W);
    //int y = WINDOW_H -(event->tfinger.y* WINDOW_H);

    // Normal
    int x = event->tfinger.x* WINDOW_W;
    int y = event->tfinger.y* WINDOW_H;

    if (y > 400  && y < 450)
    {
        if (x > 433 && x < 483)
            return COGPAGE;
        if (x > 490 && x < 540)
            return SOGPAGE;
        if (x > 547 && x < 595)
            return DPTPAGE;
        if (x > 605 && x < 652)
            return WNDPAGE;
        if (x > 662 && x < 708)
           return GPSPAGE;
        if (x > 718 && x < 765) {
            if (sdlApp->curPage == COGPAGE && event->user.code != 1 /* not for RFB */)
                return CALPAGE;
            else
                return PWRPAGE;
        }
        if (sdlApp->subAppsCmd[sdlApp->curPage][0] != NULL && event->user.code != 1 ) {
            if (x > 30 && x < 80)
                return TSKPAGE;
        }
    }
    return 0;
}

static void addMenuItems(sdl2_app *sdlApp, TTF_Font *font)
{  
    // Add text on top of a simple menu bar

    SDL_Texture* textM1;
    SDL_Rect M1_rect;

    get_text_and_rect(sdlApp->renderer, 440, 416, 0, "COG", font, &textM1, &M1_rect, BLACK);
    SDL_RenderCopy(sdlApp->renderer, textM1, NULL, &M1_rect); SDL_DestroyTexture(textM1);

    get_text_and_rect(sdlApp->renderer, 498, 416, 0, "SOG", font, &textM1, &M1_rect, BLACK);
    SDL_RenderCopy(sdlApp->renderer, textM1, NULL, &M1_rect); SDL_DestroyTexture(textM1);

    get_text_and_rect(sdlApp->renderer, 556, 416, 0, "DPT", font, &textM1, &M1_rect, BLACK);
    SDL_RenderCopy(sdlApp->renderer, textM1, NULL, &M1_rect); SDL_DestroyTexture(textM1);

    get_text_and_rect(sdlApp->renderer, 610, 416, 0, "WND", font, &textM1, &M1_rect, BLACK);
    SDL_RenderCopy(sdlApp->renderer, textM1, NULL, &M1_rect); SDL_DestroyTexture(textM1);

    get_text_and_rect(sdlApp->renderer, 668, 416, 0, "GPS", font, &textM1, &M1_rect, BLACK);
    SDL_RenderCopy(sdlApp->renderer, textM1, NULL, &M1_rect); SDL_DestroyTexture(textM1);
    
    get_text_and_rect(sdlApp->renderer, 726, 416, 0, sdlApp->curPage == COGPAGE? "CAL" : "PWR", font, &textM1, &M1_rect, BLACK);
    SDL_RenderCopy(sdlApp->renderer, textM1, NULL, &M1_rect); SDL_DestroyTexture(textM1);
}

static void setUTCtime(void)
{
    struct tm *settm = NULL;
    time_t rawtime, sys_rawtime;
    char buf[40];
    static int fails;

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
            if (stime(&rawtime) < 0) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to set UTC system time from GPS: %s", strerror(errno));
            } else {             
                    rawtime = time(NULL);
                    strftime(buf, sizeof(buf),TIMEDATFMT, localtime(&rawtime));
                    SDL_Log("Got system time from GPS: %s", buf);
            }
        } else { // Could happen if historical data is replayed in the system
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to set UTC system time from GPS as time is moving backwards %ld seconds!", sys_rawtime-rawtime);
        }
    }
    unsetenv("TZ"); // Restore whatever zone we are in
}

// Make sure instruments take shortest route over the 0 - 360 scale
static float rotate(float angle, int res)
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

// Present the compass with heading ant roll
static int doCompass(sdl2_app *sdlApp)
{
    SDL_Event event;
    SDL_Rect compassR, outerRingR, clinoMeterR, menuBarR, subTaskbarR, netStatbarR, textBoxR;
    TTF_Font* fontCog = TTF_OpenFont(sdlApp->fontPath, 42);
    TTF_Font* fontRoll = TTF_OpenFont(sdlApp->fontPath, 22);
    TTF_Font* fontSrc = TTF_OpenFont(sdlApp->fontPath, 14);
    TTF_Font* fontTod = TTF_OpenFont(sdlApp->fontPath, 12);

    SDL_Texture* compassRose = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "compassRose.png");
    SDL_Texture* outerRing = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "outerRing.png");
    SDL_Texture* clinoMeter = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "clinometer.png");
    SDL_Texture* menuBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "menuBar.png");
    SDL_Texture* netStatBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "netStat.png");
    SDL_Texture* textBox = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "textBox.png");

    SDL_Texture* subTaskbar = NULL;

    sdlApp->curPage = COGPAGE;

    if (sdlApp->subAppsCmd[sdlApp->curPage][0] != NULL) {
        char icon[PATH_MAX];
        sprintf(icon , "%s/%s.png", IMAGE_PATH, sdlApp->subAppsCmd[sdlApp->curPage][0]);
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

    menuBarR.w = 340;
    menuBarR.h = 50;
    menuBarR.x = 430;
    menuBarR.y = 400;

    subTaskbarR.w = 50;
    subTaskbarR.h = 50;
    subTaskbarR.x = 30;
    subTaskbarR.y = 400;


    netStatbarR.w = 25;
    netStatbarR.h = 25;
    netStatbarR.x = 20;
    netStatbarR.y = 20;

    textBoxR.w = 290;
    textBoxR.h = 42;
    textBoxR.x = 470;
    textBoxR.y = 106;

    SDL_Texture* textField;
    SDL_Rect textField_rect;

    float t_angle = 0;
    float angle = 0;
    float t_roll = 0;
    float roll = 0;
    int res = 1;
    int boxItems[] = {120,170,220,270};

    int toggle = 1;
    float dynUpd;

    while (1) {
        int boxItem = 0;
        char msg_hdm[40] = { "" };
        char msg_rll[40] = { "" };
        char msg_sog[40] = { "" };
        char msg_stw[40] = { "" };
        char msg_dbt[40] = { "" };
        char msg_mtw[40] = { "" };
        char msg_src[40] = { "" };
        char msg_tod[40];
        time_t ct;

        if (sdlApp->conf->onHold) {
            SDL_Delay(4000);
            continue;
        }

        SDL_PollEvent(&event);

        if(event.type == SDL_QUIT || event.type == SDL_MOUSEBUTTONDOWN)
            break;

        if(event.type == SDL_FINGERDOWN)
        {
            if ((event.type=pageSelect(sdlApp, &event)))
                break;
        }

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
        if (!(ct - cnmea.roll_i2cts > S_TIMEOUT))
            sprintf(msg_rll, "%.0f", fabs(roll=cnmea.roll));

        // RMC - Recommended minimum specific GPS/Transit data
        if (!(ct - cnmea.rmc_ts > S_TIMEOUT))
            sprintf(msg_sog, "SOG: %.1f", cnmea.rmc);

        // DBT - Depth Below Transponder
        if (!(ct - cnmea.dbt_ts > S_TIMEOUT))
            sprintf(msg_dbt, cnmea.dbt > 70.0? "DBT: %.0f" : "DBT: %.1f", cnmea.dbt);

        // MTW - Water temperature in C
        if (!(ct - cnmea.mtw_ts > S_TIMEOUT))
            sprintf(msg_mtw, "TMP: %.1f", cnmea.mtw);
                      
        angle = rotate(roundf(cnmea.hdm), res); res=0;

        // Run needle and roll with smooth acceleration
        if (angle > t_angle) t_angle += 0.8 * (fabsf(angle -t_angle) / 24);
        else if (angle < t_angle) t_angle -= 0.8 * (fabsf(angle -t_angle) / 24);

        if (roll > t_roll) t_roll += 0.8 * (fabsf(roll -t_roll) / 10);
        else if (roll < t_roll) t_roll -= 0.8 * (fabsf(roll -t_roll) / 10);

        SDL_RenderCopy(sdlApp->renderer, Background_Tx, NULL, NULL);

        SDL_RenderCopyEx(sdlApp->renderer, outerRing, NULL, &outerRingR, 0, NULL, SDL_FLIP_NONE);
        SDL_RenderCopyEx(sdlApp->renderer, compassRose, NULL, &compassR, 360-t_angle, NULL, SDL_FLIP_NONE);
        
        if (!(ct - cnmea.roll_i2cts > S_TIMEOUT))
            SDL_RenderCopyEx(sdlApp->renderer, clinoMeter, NULL, &clinoMeterR, t_roll, NULL, SDL_FLIP_NONE);

        get_text_and_rect(sdlApp->renderer, 226, 180, 3, msg_src, fontSrc, &textField, &textField_rect, BLACK);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

    
        get_text_and_rect(sdlApp->renderer, 200, 200, 3, msg_hdm, fontCog, &textField, &textField_rect, BLACK);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
       

        get_text_and_rect(sdlApp->renderer, 224, 248, 2, msg_rll, fontRoll, &textField, &textField_rect, BLACK);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        if (!(ct - cnmea.stw_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_stw, fontCog, &textField, &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        }

        if (!(ct - cnmea.rmc_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_sog, fontCog, &textField, &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        }

        if (!(ct - cnmea.dbt_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_dbt, fontCog, &textField, &textField_rect, cnmea.dbt <DWRN? RED : WHITE);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        }

        if (!(ct - cnmea.mtw_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_mtw, fontCog, &textField, &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        }
       
        SDL_RenderCopyEx(sdlApp->renderer, menuBar, NULL, &menuBarR, 0, NULL, SDL_FLIP_NONE);
        addMenuItems(sdlApp, fontSrc);

        get_text_and_rect(sdlApp->renderer, 650, 10, 0, msg_tod, fontTod, &textField, &textField_rect, WHITE);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        if (subTaskbar != NULL) {
            SDL_RenderCopyEx(sdlApp->renderer, subTaskbar, NULL, &subTaskbarR, 0, NULL, SDL_FLIP_NONE);
        }

        if (sdlApp->conf->netStat == 1) {
           SDL_RenderCopyEx(sdlApp->renderer, netStatBar, NULL, &netStatbarR, 0, NULL, SDL_FLIP_NONE);
        }

        if (boxItem) {
            textBoxR.h = boxItem*50 +30;
            SDL_RenderCopyEx(sdlApp->renderer, textBox, NULL, &textBoxR, 0, NULL, SDL_FLIP_NONE);
        }

        SDL_RenderPresent(sdlApp->renderer);

        if (sdlApp->conf->runVnc && sdlApp->conf->vncClients && sdlApp->conf->vncPixelBuffer && (toggle = !toggle)) {
            // Read the pixels from the current render target and save them onto the surface
            // This will slow down the application a bit.
            SDL_RenderReadPixels(sdlApp->renderer, NULL, SDL_GetWindowPixelFormat(sdlApp->window),
                sdlApp->conf->vncPixelBuffer->pixels, sdlApp->conf->vncPixelBuffer->pitch);
            rfbMarkRectAsModified(sdlApp->conf->vncServer, 0, 0, WINDOW_W, WINDOW_H);
        }

        // Reduce CPU load if only short scale movements
        dynUpd = (1/fabsf(angle -t_angle))*200;
        dynUpd = dynUpd > 200? 200:dynUpd;

        SDL_Delay(30+(int)dynUpd);
    }

    if (subTaskbar != NULL) {
        SDL_DestroyTexture(subTaskbar);
    }

    SDL_DestroyTexture(compassRose);
    SDL_DestroyTexture(outerRing);
    SDL_DestroyTexture(clinoMeter);
    SDL_DestroyTexture(menuBar);
    SDL_DestroyTexture(netStatBar);
    SDL_DestroyTexture(textBox);
    TTF_CloseFont(fontCog);
    TTF_CloseFont(fontRoll);
    TTF_CloseFont(fontSrc);
    TTF_CloseFont(fontTod);
    IMG_Quit();

    return event.type;
}

// Present the Sumlog (NMEA net only)
static int doSumlog(sdl2_app *sdlApp)
{
    SDL_Event event;
    SDL_Rect gaugeR, needleR, menuBarR, subTaskbarR, netStatbarR, textBoxR;
    TTF_Font* fontLarge =  TTF_OpenFont(sdlApp->fontPath, 46);
    TTF_Font* fontSmall =  TTF_OpenFont(sdlApp->fontPath, 20);
    TTF_Font* fontCog = TTF_OpenFont(sdlApp->fontPath, 42);
    TTF_Font* fontSrc = TTF_OpenFont(sdlApp->fontPath, 14);
    TTF_Font* fontTod = TTF_OpenFont(sdlApp->fontPath, 12);

    gaugeR.w = 440;
    gaugeR.h = 440;
    gaugeR.x = 19;
    gaugeR.y = 18;

    needleR.w = 240;
    needleR.h = 240;
    needleR.x = 120;
    needleR.y = 122;

    menuBarR.w = 340;
    menuBarR.h = 50;
    menuBarR.x = 430;
    menuBarR.y = 400;

    subTaskbarR.w = 50;
    subTaskbarR.h = 50;
    subTaskbarR.x = 30;
    subTaskbarR.y = 400;

    netStatbarR.w = 25;
    netStatbarR.h = 25;
    netStatbarR.x = 20;
    netStatbarR.y = 20;

    textBoxR.w = 290;
    textBoxR.h = 42;
    textBoxR.x = 470;
    textBoxR.y = 106;

    SDL_Texture* gaugeSumlog = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "sumlog.png");
    SDL_Texture* gaugeNeedle = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "needle.png");
    SDL_Texture* menuBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "menuBar.png");
    SDL_Texture* netStatBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "netStat.png");
    SDL_Texture* textBox = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "textBox.png");

    SDL_Texture* subTaskbar = NULL;

    sdlApp->curPage = SOGPAGE;

    if (sdlApp->subAppsCmd[sdlApp->curPage][0] != NULL) {
        char icon[PATH_MAX];
        sprintf(icon , "%s/%s.png", IMAGE_PATH, sdlApp->subAppsCmd[sdlApp->curPage][0]);
        if ((subTaskbar = IMG_LoadTexture(sdlApp->renderer, icon)) == NULL)
            subTaskbar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "tool.png");           
    }

    SDL_Texture* textField;
    SDL_Rect textField_rect;

    float t_angle = 0;
    float angle = 0;
    int boxItems[] = {120,170,220};
    int toggle = 1;
    float dynUpd;

    while (1) {
        int boxItem = 0;
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

        if (sdlApp->conf->onHold) {
            SDL_Delay(4000);
            continue;
        }

        SDL_PollEvent(&event);

        if(event.type == SDL_QUIT || event.type == SDL_MOUSEBUTTONDOWN)
            break;

        if(event.type == SDL_FINGERDOWN)
        {
            if ((event.type=pageSelect(sdlApp, &event)))
                break;
        }

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

        // MTW - Water temperature in C
        if (!(ct - cnmea.mtw_ts > S_TIMEOUT))
            sprintf(msg_mtw, "TMP: %.1f", cnmea.mtw);
                         
        speed = wspeed * (maxangle/maxspeed);
        angle = roundf(speed+minangle);

        // Run needle with smooth acceleration
        if (angle > t_angle) t_angle += 3.2 * (fabsf(angle -t_angle) / 24) ;
        else if (angle < t_angle) t_angle -= 3.2 * (fabsf(angle -t_angle) / 24);

        SDL_RenderCopy(sdlApp->renderer, Background_Tx, NULL, NULL);
       
        SDL_RenderCopyEx(sdlApp->renderer, gaugeSumlog, NULL, &gaugeR, 0, NULL, SDL_FLIP_NONE);

        if (wspeed)
            SDL_RenderCopyEx(sdlApp->renderer, gaugeNeedle, NULL, &needleR, t_angle, NULL, SDL_FLIP_NONE);

        get_text_and_rect(sdlApp->renderer, 182, 300, 4, msg_stw, fontLarge, &textField, &textField_rect, BLACK);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        if (!(ct - cnmea.hdm_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_hdm, fontCog, &textField, &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        }

        if (!(ct - cnmea.dbt_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_dbt, fontCog, &textField, &textField_rect, cnmea.dbt <DWRN? RED : WHITE);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        }

        if (!(ct - cnmea.mtw_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_mtw, fontCog, &textField, &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        }

        SDL_RenderCopyEx(sdlApp->renderer, menuBar, NULL, &menuBarR, 0, NULL, SDL_FLIP_NONE);

        get_text_and_rect(sdlApp->renderer, 650, 10, 0, msg_tod, fontTod, &textField, &textField_rect, WHITE);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

         if (stw) {
            get_text_and_rect(sdlApp->renderer, 186, 366, 8, msg_sog, fontSmall, &textField, &textField_rect, BLACK);       
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        }

        addMenuItems(sdlApp, fontSrc);

        if (subTaskbar != NULL) {
            SDL_RenderCopyEx(sdlApp->renderer, subTaskbar, NULL, &subTaskbarR, 0, NULL, SDL_FLIP_NONE);
        }

        if (sdlApp->conf->netStat == 1) {
           SDL_RenderCopyEx(sdlApp->renderer, netStatBar, NULL, &netStatbarR, 0, NULL, SDL_FLIP_NONE);
        }

        if (boxItem) {
            textBoxR.h = boxItem*50 +30;
            SDL_RenderCopyEx(sdlApp->renderer, textBox, NULL, &textBoxR, 0, NULL, SDL_FLIP_NONE);
        }

        SDL_RenderPresent(sdlApp->renderer); 

        if (sdlApp->conf->runVnc && sdlApp->conf->vncClients && sdlApp->conf->vncPixelBuffer && (toggle = !toggle)) {
            // Read the pixels from the current render target and save them onto the surface
            // This will slow down the application a bit.
            SDL_RenderReadPixels(sdlApp->renderer, NULL, SDL_GetWindowPixelFormat(sdlApp->window),
                sdlApp->conf->vncPixelBuffer->pixels, sdlApp->conf->vncPixelBuffer->pitch);
            rfbMarkRectAsModified(sdlApp->conf->vncServer, 0, 0, WINDOW_W, WINDOW_H);
        }
        // Reduce CPU load if only short scale movements
        dynUpd = (1/fabsf(angle -t_angle))*200;
        dynUpd = dynUpd > 200? 200:dynUpd;

        SDL_Delay(30+(int)dynUpd);  
    }

    if (subTaskbar != NULL) {
        SDL_DestroyTexture(subTaskbar);
    }

    SDL_DestroyTexture(gaugeSumlog);
    SDL_DestroyTexture(gaugeNeedle);
    SDL_DestroyTexture(menuBar);
    SDL_DestroyTexture(netStatBar);
    SDL_DestroyTexture(textBox);
    TTF_CloseFont(fontLarge);
    TTF_CloseFont(fontSmall); 
    TTF_CloseFont(fontCog);
    TTF_CloseFont(fontSrc);
    TTF_CloseFont(fontTod);
    IMG_Quit();

    return event.type;
}

// Present GPS data
static int doGps(sdl2_app *sdlApp)
{
    SDL_Event event;
    SDL_Rect gaugeR, menuBarR, subTaskbarR, netStatbarR, textBoxR;

    TTF_Font* fontHD =  TTF_OpenFont(sdlApp->fontPath, 40);
    TTF_Font* fontLA =  TTF_OpenFont(sdlApp->fontPath, 30);
    TTF_Font* fontLO =  TTF_OpenFont(sdlApp->fontPath, 30);
    TTF_Font* fontMG =  TTF_OpenFont(sdlApp->fontPath, 14);
    TTF_Font* fontCog = TTF_OpenFont(sdlApp->fontPath, 42);
    TTF_Font* fontSrc = TTF_OpenFont(sdlApp->fontPath, 14);
    TTF_Font* fontTod = TTF_OpenFont(sdlApp->fontPath, 12);

    SDL_Texture* gaugeGps = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "gps.png");
    SDL_Texture* menuBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "menuBar.png");
    SDL_Texture* netStatBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "netStat.png");
    SDL_Texture* textBox = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "textBox.png");
        
    SDL_Texture* subTaskbar = NULL;

    sdlApp->curPage = GPSPAGE;

    if (sdlApp->subAppsCmd[sdlApp->curPage][0] != NULL) {
        char icon[PATH_MAX];
        sprintf(icon , "%s/%s.png", IMAGE_PATH, sdlApp->subAppsCmd[sdlApp->curPage][0]);
        if ((subTaskbar = IMG_LoadTexture(sdlApp->renderer, icon)) == NULL)
            subTaskbar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "tool.png");           
    }

    SDL_Texture* textField;
    SDL_Rect textField_rect;

    gaugeR.w = 440;
    gaugeR.h = 440;
    gaugeR.x = 19;
    gaugeR.y = 18;

    menuBarR.w = 340;
    menuBarR.h = 50;
    menuBarR.x = 430;
    menuBarR.y = 400;

    subTaskbarR.w = 50;
    subTaskbarR.h = 50;
    subTaskbarR.x = 30;
    subTaskbarR.y = 400;

    netStatbarR.w = 25;
    netStatbarR.h = 25;
    netStatbarR.x = 20;
    netStatbarR.y = 20;

    textBoxR.w = 290;
    textBoxR.h = 42;
    textBoxR.x = 470;
    textBoxR.y = 106;

    int boxItems[] = {120,170,220,270};

    while (1) {
        int boxItem = 0;
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

        if (sdlApp->conf->onHold) {
            SDL_Delay(4000);
            continue;
        }

        SDL_PollEvent(&event);

        if(event.type == SDL_QUIT || event.type == SDL_MOUSEBUTTONDOWN)
            break;

        if(event.type == SDL_FINGERDOWN)
        {
            if ((event.type=pageSelect(sdlApp, &event)))
                break;
        }

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
        
        // MTW - Water temperature in C
        if (!(ct - cnmea.mtw_ts > S_TIMEOUT))
            sprintf(msg_mtw, "TMP: %.1f", cnmea.mtw);

        // DBT - Depth Below Transponder
        if (!(ct - cnmea.dbt_ts > S_TIMEOUT))
            sprintf(msg_dbt, cnmea.dbt > 70.0? "DBT: %.0f" : "DBT: %.1f", cnmea.dbt);
        
        SDL_RenderCopy(sdlApp->renderer, Background_Tx, NULL, NULL);
       
        SDL_RenderCopyEx(sdlApp->renderer, gaugeGps, NULL, &gaugeR, 0, NULL, SDL_FLIP_NONE);

        get_text_and_rect(sdlApp->renderer, 196, 142, 3, msg_hdm, fontHD, &textField, &textField_rect, BLACK);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 290, 168, 1, msg_src, fontMG, &textField, &textField_rect, BLACK);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 148, 222, 9, msg_lat, fontLA, &textField, &textField_rect, BLACK);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 148, 292, 9, msg_lot, fontLO, &textField, &textField_rect, BLACK);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
       
        if (!(ct - cnmea.stw_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_stw, fontCog, &textField, &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        }

         if (!(ct - cnmea.rmc_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_sog, fontCog, &textField, &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        }

         if (!(ct - cnmea.dbt_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_dbt, fontCog, &textField, &textField_rect, cnmea.dbt <DWRN? RED : WHITE);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        }

         if (!(ct - cnmea.mtw_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_mtw, fontCog, &textField, &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        }

        SDL_RenderCopyEx(sdlApp->renderer, menuBar, NULL, &menuBarR, 0, NULL, SDL_FLIP_NONE);
        addMenuItems(sdlApp, fontSrc);

        get_text_and_rect(sdlApp->renderer, 650, 10, 0, msg_tod, fontTod, &textField, &textField_rect, WHITE);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        if (subTaskbar != NULL) {
            SDL_RenderCopyEx(sdlApp->renderer, subTaskbar, NULL, &subTaskbarR, 0, NULL, SDL_FLIP_NONE);
        }

        if (sdlApp->conf->netStat == 1) {
           SDL_RenderCopyEx(sdlApp->renderer, netStatBar, NULL, &netStatbarR, 0, NULL, SDL_FLIP_NONE);
        }

        if (boxItem) {
            textBoxR.h = boxItem*50 +30;
            SDL_RenderCopyEx(sdlApp->renderer, textBox, NULL, &textBoxR, 0, NULL, SDL_FLIP_NONE);
        }

        SDL_RenderPresent(sdlApp->renderer); 

        if (sdlApp->conf->runVnc && sdlApp->conf->vncClients && sdlApp->conf->vncPixelBuffer) {
            // Read the pixels from the current render target and save them onto the surface
            // This will slow down the application a bit.
            SDL_RenderReadPixels(sdlApp->renderer, NULL, SDL_GetWindowPixelFormat(sdlApp->window),
                sdlApp->conf->vncPixelBuffer->pixels, sdlApp->conf->vncPixelBuffer->pitch);
            rfbMarkRectAsModified(sdlApp->conf->vncServer, 0, 0, WINDOW_W, WINDOW_H);
        }
        
        SDL_Delay(500);
    }

    if (subTaskbar != NULL) {
        SDL_DestroyTexture(subTaskbar);
    }
    SDL_DestroyTexture(gaugeGps);
    SDL_DestroyTexture(menuBar);
    SDL_DestroyTexture(netStatBar);
    SDL_DestroyTexture(textBox);
    TTF_CloseFont(fontHD);
    TTF_CloseFont(fontLA);
    TTF_CloseFont(fontLO);
    TTF_CloseFont(fontMG);
    TTF_CloseFont(fontCog);
    TTF_CloseFont(fontSrc);
    TTF_CloseFont(fontTod);
    IMG_Quit();

    return event.type;
}

// Present Depth data (NMEA net only)
static int doDepth(sdl2_app *sdlApp)
{
    SDL_Event event;
    SDL_Rect gaugeR, needleR, menuBarR, subTaskbarR, netStatbarR, textBoxR;
    TTF_Font* fontLarge =  TTF_OpenFont(sdlApp->fontPath, 46);
    TTF_Font* fontSmall =  TTF_OpenFont(sdlApp->fontPath, 18);
    TTF_Font* fontCog = TTF_OpenFont(sdlApp->fontPath, 42);
    TTF_Font* fontSrc = TTF_OpenFont(sdlApp->fontPath, 14);
    TTF_Font* fontTod = TTF_OpenFont(sdlApp->fontPath, 12);

    SDL_Texture* gaugeDepthW = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "depthw.png");
    SDL_Texture* gaugeDepth = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "depth.png");
    SDL_Texture* gaugeDepthx10 = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "depthx10.png");
    SDL_Texture* menuBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "menuBar.png");
    SDL_Texture* netStatBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "netStat.png");   
    SDL_Texture* gaugeNeedle = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "needle.png");
    SDL_Texture* textBox = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "textBox.png");

    SDL_Texture* gauge;
    SDL_Texture* subTaskbar = NULL;
    
    SDL_Texture* textField;
    SDL_Rect textField_rect;

    gaugeR.w = 440;
    gaugeR.h = 440;
    gaugeR.x = 19;
    gaugeR.y = 18;

    needleR.w = 240;
    needleR.h = 240;
    needleR.x = 120;
    needleR.y = 122;

    menuBarR.w = 340;
    menuBarR.h = 50;
    menuBarR.x = 430;
    menuBarR.y = 400;

    subTaskbarR.w = 50;
    subTaskbarR.h = 50;
    subTaskbarR.x = 30;
    subTaskbarR.y = 400;

    netStatbarR.w = 25;
    netStatbarR.h = 25;
    netStatbarR.x = 20;
    netStatbarR.y = 20;

    textBoxR.w = 290;
    textBoxR.h = 42;
    textBoxR.x = 470;
    textBoxR.y = 106;

    int boxItems[] = {120,170,220};

    sdlApp->curPage = DPTPAGE;

    if (sdlApp->subAppsCmd[sdlApp->curPage][0] != NULL) {
        char icon[PATH_MAX];
        sprintf(icon , "%s/%s.png", IMAGE_PATH, sdlApp->subAppsCmd[sdlApp->curPage][0]);
        if ((subTaskbar = IMG_LoadTexture(sdlApp->renderer, icon)) == NULL)
            subTaskbar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "tool.png");           
    }

    float t_angle = 0;
    float angle = 0;

    int toggle = 1;

    float dynUpd;

    while (1) {
        int boxItem = 0;
        float depth;
        float scale; 
        char msg_dbt[40];
        char msg_mtw[40] = { "" };
        char msg_hdm[40] = { "" };
        char msg_stw[40] = { "" };
        char msg_rmc[40] = { "" };
        char msg_tod[40];
        time_t ct;

        // Constants for instrument
        const float minangle = 12;  // Scale start
        const float maxangle = 236; // Scale end
        const float maxsdepth = 10;

        if (sdlApp->conf->onHold) {
            SDL_Delay(4000);
            continue;
        }

        SDL_PollEvent(&event);

        if(event.type == SDL_QUIT || event.type == SDL_MOUSEBUTTONDOWN)
            break;

        if(event.type == SDL_FINGERDOWN)
        {
            if ((event.type=pageSelect(sdlApp, &event)))
                break;
        }

        ct = time(NULL);    // Get a timestamp for this turn 
        strftime(msg_tod, sizeof(msg_tod),TIMEDATFMT, localtime(&ct));

        // DPT - Depth
         if (ct - cnmea.dbt_ts > S_TIMEOUT || cnmea.dbt == 0)
            sprintf(msg_dbt, "----");
        else
            sprintf(msg_dbt, cnmea.dbt >= 100.0? "%.0f" : "%.1f", cnmea.dbt);

        // MTW - Water temperature in C
        if (ct - cnmea.mtw_ts > S_TIMEOUT || cnmea.mtw == 0)
            sprintf(msg_mtw, "----");
        else
            sprintf(msg_mtw, "Temp :%.1f", cnmea.mtw);
        
        // Heading
        if (!(ct - cnmea.hdm_ts > S_TIMEOUT))
            sprintf(msg_hdm, "COG: %.0f", cnmea.hdm);

        // RMC - Recommended minimum specific GPS/Transit data
        if (!(ct - cnmea.rmc_ts > S_TIMEOUT))
             sprintf(msg_rmc, "SOG: %.1f", cnmea.rmc);
        
        // VHW - Water speed and Heading
         if (!(ct - cnmea.stw_ts > S_TIMEOUT))
            sprintf(msg_stw, "STW: %.1f", cnmea.stw);
        
        gauge = gaugeDepth;
        if (cnmea.dbt < 5) gauge = gaugeDepthW;
        if (cnmea.dbt > 10) gauge = gaugeDepthx10;

        depth = cnmea.dbt;
        if (depth > 10.0) depth /=10;

        scale = depth * (maxangle/maxsdepth);
        angle = roundf(scale+minangle);

        // Run needle with smooth acceleration
        if (angle > t_angle) t_angle += 3.2 * (fabsf(angle -t_angle) / 24) ;
        else if (angle < t_angle) t_angle -= 3.2 * (fabsf(angle -t_angle) / 24);

        SDL_RenderCopy(sdlApp->renderer, Background_Tx, NULL, NULL);
    
        SDL_RenderCopyEx(sdlApp->renderer, gauge, NULL, &gaugeR, 0, NULL, SDL_FLIP_NONE);

        if (!(ct - cnmea.dbt_ts > S_TIMEOUT || cnmea.dbt == 0) && cnmea.dbt < 110)
            SDL_RenderCopyEx(sdlApp->renderer, gaugeNeedle, NULL, &needleR, t_angle, NULL, SDL_FLIP_NONE);

        get_text_and_rect(sdlApp->renderer, 182, 300, 4, msg_dbt, fontLarge, &textField, &textField_rect, BLACK);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 180, 370, 1, msg_mtw, fontSmall, &textField, &textField_rect, BLACK);   
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        if (!(ct - cnmea.hdm_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_hdm, fontCog, &textField, &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        }
        
        if (!(ct - cnmea.rmc_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_rmc, fontCog, &textField, &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        }
        
        if (!(ct - cnmea.stw_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_stw, fontCog, &textField, &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        }

        SDL_RenderCopyEx(sdlApp->renderer, menuBar, NULL, &menuBarR, 0, NULL, SDL_FLIP_NONE);
        addMenuItems(sdlApp, fontSrc);

        get_text_and_rect(sdlApp->renderer, 650, 10, 0, msg_tod, fontTod, &textField, &textField_rect, WHITE);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        if (subTaskbar != NULL) {
            SDL_RenderCopyEx(sdlApp->renderer, subTaskbar, NULL, &subTaskbarR, 0, NULL, SDL_FLIP_NONE);
        }

        if (sdlApp->conf->netStat == 1) {
           SDL_RenderCopyEx(sdlApp->renderer, netStatBar, NULL, &netStatbarR, 0, NULL, SDL_FLIP_NONE);
        }

        if (boxItem) {
            textBoxR.h = boxItem*50 +30;
            SDL_RenderCopyEx(sdlApp->renderer, textBox, NULL, &textBoxR, 0, NULL, SDL_FLIP_NONE);
        }

        SDL_RenderPresent(sdlApp->renderer);

        if (sdlApp->conf->runVnc && sdlApp->conf->vncClients && sdlApp->conf->vncPixelBuffer && (toggle = !toggle)) {
            // Read the pixels from the current render target and save them onto the surface
            // This will slow down the application a bit.
            SDL_RenderReadPixels(sdlApp->renderer, NULL, SDL_GetWindowPixelFormat(sdlApp->window),
                sdlApp->conf->vncPixelBuffer->pixels, sdlApp->conf->vncPixelBuffer->pitch);
            rfbMarkRectAsModified(sdlApp->conf->vncServer, 0, 0, WINDOW_W, WINDOW_H);
        }
        
        // Reduce CPU load if only short scale movements
        dynUpd = (1/fabsf(angle -t_angle))*200;
        dynUpd = dynUpd > 200? 200:dynUpd;

        SDL_Delay(30+(int)dynUpd);
    }

    if (subTaskbar != NULL) {
        SDL_DestroyTexture(subTaskbar);
    }
    SDL_DestroyTexture(gaugeDepth);
    SDL_DestroyTexture(gaugeDepthW);
    SDL_DestroyTexture(gaugeDepthx10);
    SDL_DestroyTexture(gaugeNeedle);
    SDL_DestroyTexture(menuBar);
    SDL_DestroyTexture(netStatBar);
    SDL_DestroyTexture(textBox);
    TTF_CloseFont(fontLarge);
    TTF_CloseFont(fontSmall);
    TTF_CloseFont(fontCog);
    TTF_CloseFont(fontSrc);
    TTF_CloseFont(fontTod);
    IMG_Quit();

    return event.type;
}

// Present Wind data (NMEA net only)
static int doWind(sdl2_app *sdlApp)
{
    SDL_Event event;
    SDL_Rect gaugeR, needleR, menuBarR, subTaskbarR, netStatbarR, textBoxR;;
    TTF_Font* fontLarge =  TTF_OpenFont(sdlApp->fontPath, 46);
    TTF_Font* fontSmall =  TTF_OpenFont(sdlApp->fontPath, 20);
    TTF_Font* fontCog = TTF_OpenFont(sdlApp->fontPath, 42);
    TTF_Font* fontSrc = TTF_OpenFont(sdlApp->fontPath, 14);
    TTF_Font* fontTod = TTF_OpenFont(sdlApp->fontPath, 12);

    SDL_Texture* gaugeSumlog = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "wind.png");
    SDL_Texture* gaugeNeedle = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "needle.png");
    SDL_Texture* menuBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "menuBar.png");
    SDL_Texture* netStatBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "netStat.png");
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

    menuBarR.w = 340;
    menuBarR.h = 50;
    menuBarR.x = 430;
    menuBarR.y = 400;

    subTaskbarR.w = 50;
    subTaskbarR.h = 50;
    subTaskbarR.x = 30;
    subTaskbarR.y = 400;

    netStatbarR.w = 25;
    netStatbarR.h = 25;
    netStatbarR.x = 20;
    netStatbarR.y = 20;

    textBoxR.w = 290;
    textBoxR.h = 42;
    textBoxR.x = 470;
    textBoxR.y = 106;

    int boxItems[] = {120,170,220,270,320};

    sdlApp->curPage = WNDPAGE;

    if (sdlApp->subAppsCmd[sdlApp->curPage][0] != NULL) {
        char icon[PATH_MAX];
        sprintf(icon , "%s/%s.png", IMAGE_PATH, sdlApp->subAppsCmd[sdlApp->curPage][0]);
        if ((subTaskbar = IMG_LoadTexture(sdlApp->renderer, icon)) == NULL)
            subTaskbar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "tool.png");           
    }

    SDL_Texture* textField;
    SDL_Rect textField_rect;

    float t_angle = 0;
    float angle = 0;
    int res = 1;

    int toggle = 1;

    float dynUpd;

    const float offset = 131; // For scale

    while (1) {
        int boxItem = 0;
        char msg_vwrs[40];
        char msg_vwra[40];
        char msg_dbt[40] = { "" };
        char msg_mtw[40] = { "" };
        char msg_stw[40] = { "" };      
        char msg_hdm[40] = { "" };
        char msg_rmc[40] = { "" };
        char msg_tod[40];
        time_t ct;

        if (sdlApp->conf->onHold) {
            SDL_Delay(4000);
            continue;
        }

        SDL_PollEvent(&event);

        if(event.type == SDL_QUIT || event.type == SDL_MOUSEBUTTONDOWN)
            break;

        if(event.type == SDL_FINGERDOWN)
        {
            if ((event.type=pageSelect(sdlApp, &event)))
                break;
        }

        ct = time(NULL);    // Get a timestamp for this turn
        strftime(msg_tod, sizeof(msg_tod),TIMEDATFMT, localtime(&ct));

        // Wind speed and angle (relative)
         if (ct -  cnmea.vwr_ts > S_TIMEOUT || cnmea.vwrs == 0)
            sprintf(msg_vwrs, "----");
        else
            sprintf(msg_vwrs, "%.1f", cnmea.vwrs);

        if (ct - cnmea.vwr_ts > S_TIMEOUT)
            sprintf(msg_vwra, "----");
        else
            sprintf(msg_vwra, "%.0f%c", cnmea.vwra, 0xb0);

        // DPT - Depth
        if (!(ct - cnmea.dbt_ts > S_TIMEOUT || cnmea.dbt == 0))
            sprintf(msg_dbt, "DBT: %.1f", cnmea.dbt);

        // MTW - Water temperature in C
        if (!(ct - cnmea.mtw_ts > S_TIMEOUT || cnmea.mtw == 0))
            sprintf(msg_mtw, "TMP: %.1f", cnmea.mtw);
        
        // Heading
        if (!(ct - cnmea.hdm_ts > S_TIMEOUT))
            sprintf(msg_hdm, "COG: %.0f", cnmea.hdm);

        // RMC - Recommended minimum specific GPS/Transit data
        if (!(ct - cnmea.rmc_ts > S_TIMEOUT))
             sprintf(msg_rmc, "SOG: %.1f", cnmea.rmc);
        
        // VHW - Water speed and Heading
         if (!(ct - cnmea.stw_ts > S_TIMEOUT))
            sprintf(msg_stw, "STW: %.1f", cnmea.stw);
        
        angle = cnmea.vwra; // 0-180

        if (cnmea.vwrd == 1) angle = 360 - angle; // Mirror the needle motion

        angle += offset;  

        angle = rotate(angle, res); res=0;
        
        // Run needle with smooth acceleration
        if (angle > t_angle) t_angle += 3.2 * (fabsf(angle -t_angle) / 24) ;
        else if (angle < t_angle) t_angle -= 3.2 * (fabsf(angle -t_angle) / 24);

        SDL_RenderCopy(sdlApp->renderer, Background_Tx, NULL, NULL);
       
        SDL_RenderCopyEx(sdlApp->renderer, gaugeSumlog, NULL, &gaugeR, 0, NULL, SDL_FLIP_NONE);

        if (!(ct - cnmea.vwr_ts > S_TIMEOUT || cnmea.vwra == 0))
            SDL_RenderCopyEx(sdlApp->renderer, gaugeNeedle, NULL, &needleR, t_angle, NULL, SDL_FLIP_NONE);

        get_text_and_rect(sdlApp->renderer, 216, 100, 4, msg_vwra, fontSmall, &textField, &textField_rect, BLACK);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 182, 300, 4, msg_vwrs, fontLarge, &textField, &textField_rect, BLACK);    
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        
        if (!(ct - cnmea.hdm_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_hdm, fontCog, &textField, &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        }
        
        if (!(ct - cnmea.stw_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_stw, fontCog, &textField, &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        }

        if (!(ct - cnmea.rmc_ts > S_TIMEOUT)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_rmc, fontCog, &textField, &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        }

        if (!(ct - cnmea.dbt_ts > S_TIMEOUT || cnmea.dbt == 0)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_dbt, fontCog, &textField, &textField_rect, cnmea.dbt <DWRN? RED : WHITE);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        }

        if (!(ct - cnmea.mtw_ts > S_TIMEOUT || cnmea.mtw == 0)) {
            get_text_and_rect(sdlApp->renderer, 500, boxItems[boxItem++], 0, msg_mtw, fontCog, &textField, &textField_rect, WHITE);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        }

        SDL_RenderCopyEx(sdlApp->renderer, menuBar, NULL, &menuBarR, 0, NULL, SDL_FLIP_NONE);
        addMenuItems(sdlApp, fontSrc);

        get_text_and_rect(sdlApp->renderer, 650, 10, 0, msg_tod, fontTod, &textField, &textField_rect, WHITE);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        if (subTaskbar != NULL) {
            SDL_RenderCopyEx(sdlApp->renderer, subTaskbar, NULL, &subTaskbarR, 0, NULL, SDL_FLIP_NONE);
        }

        if (sdlApp->conf->netStat == 1) {
           SDL_RenderCopyEx(sdlApp->renderer, netStatBar, NULL, &netStatbarR, 0, NULL, SDL_FLIP_NONE);
        }

        if (boxItem) {
            textBoxR.h = boxItem*50 +30;
            SDL_RenderCopyEx(sdlApp->renderer, textBox, NULL, &textBoxR, 0, NULL, SDL_FLIP_NONE);
        }

        SDL_RenderPresent(sdlApp->renderer);
 
        if (sdlApp->conf->runVnc && sdlApp->conf->vncClients && sdlApp->conf->vncPixelBuffer && (toggle = !toggle)) {
            // Read the pixels from the current render target and save them onto the surface
            // This will slow down the application a bit.
            SDL_RenderReadPixels(sdlApp->renderer, NULL, SDL_GetWindowPixelFormat(sdlApp->window),
                sdlApp->conf->vncPixelBuffer->pixels, sdlApp->conf->vncPixelBuffer->pitch);
            rfbMarkRectAsModified(sdlApp->conf->vncServer, 0, 0, WINDOW_W, WINDOW_H);
        }
        
        // Reduce CPU load if only short scale movements
        dynUpd = (1/fabsf(angle -t_angle))*200;
        dynUpd = dynUpd > 200? 200:dynUpd;

        SDL_Delay(30+(int)dynUpd);
    }

    if (subTaskbar != NULL) {
        SDL_DestroyTexture(subTaskbar);
    }
    SDL_DestroyTexture(gaugeSumlog);
    SDL_DestroyTexture(gaugeNeedle);
    SDL_DestroyTexture(menuBar);
    SDL_DestroyTexture(netStatBar);
    SDL_DestroyTexture(textBox);
    TTF_CloseFont(fontLarge);
    TTF_CloseFont(fontSmall);
    TTF_CloseFont(fontCog);
    TTF_CloseFont(fontSrc);
    TTF_CloseFont(fontTod);
    IMG_Quit();

    return event.type;
}

// Present Environmant page (Non standard NMEA)
static int doEnvironment(sdl2_app *sdlApp)
{
    SDL_Event event;
    SDL_Rect gaugeVoltR, gaugeCurrR, gaugeTempR, voltNeedleR, currNeedleR;
    SDL_Rect tempNeedleR, menuBarR, netStatbarR, subTaskbarR;
    TTF_Font* fontSmall = TTF_OpenFont(sdlApp->fontPath, 14);
    TTF_Font* fontLarge = TTF_OpenFont(sdlApp->fontPath, 18);
    TTF_Font* fontTod = TTF_OpenFont(sdlApp->fontPath, 12);

    SDL_Texture* menuBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "menuBar.png");
    SDL_Texture* netStatBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "netStat.png");

    SDL_Texture* gaugeVolt = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "volt.png");
    SDL_Texture* gaugeCurr = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "curr.png");
    SDL_Texture* gaugeTemp = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "temp.png");
    SDL_Texture* needleVolt = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "sneedle.png");
    SDL_Texture* needleCurr = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "sneedle.png");
    SDL_Texture* needleTemp = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "sneedle.png");

    sdlApp->curPage = PWRPAGE;
    SDL_Texture* subTaskbar = NULL;

    if (sdlApp->subAppsCmd[sdlApp->curPage][0] != NULL) {
        char icon[PATH_MAX];
        sprintf(icon , "%s/%s.png", IMAGE_PATH, sdlApp->subAppsCmd[sdlApp->curPage][0]);
        if ((subTaskbar = IMG_LoadTexture(sdlApp->renderer, icon)) == NULL)
            subTaskbar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "tool.png");           
    }

    gaugeVoltR.w = 200;
    gaugeVoltR.h = 200;
    gaugeVoltR.x = 80;
    gaugeVoltR.y = 30;

    gaugeCurrR.w = 200;
    gaugeCurrR.h = 200;
    gaugeCurrR.x = 300;
    gaugeCurrR.y = 30;

    gaugeTempR.w = 200;
    gaugeTempR.h = 200;
    gaugeTempR.x = 520;
    gaugeTempR.y = 30;

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

    menuBarR.w = 340;
    menuBarR.h = 50;
    menuBarR.x = 430;
    menuBarR.y = 400;

    netStatbarR.w = 25;
    netStatbarR.h = 25;
    netStatbarR.x = 20;
    netStatbarR.y = 20;

    subTaskbarR.w = 50;
    subTaskbarR.h = 50;
    subTaskbarR.x = 30;
    subTaskbarR.y = 400;

    SDL_Texture* textField;
    SDL_Rect textField_rect;

    // Scale adjustments
    float v_maxangle = 102;
    float v_offset = 6;
    float v_max = 16;
    float v_min = 8;
    float v_scaleoffset = 8;

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

#ifdef PLOTSDL
    float powerBuf[25];

    plot_params params;

    params.screen_width=760;
    params.screen_heigth=210;
    params.font_text_path=DEFAULT_FONT;
    params.font_text_size=12;
    params.hide_backgroud = 1;
    params.hide_caption = 1;
    params.caption_text_x="Time (s)";
    params.caption_text_y="Watt";
    params.scale_x = 1;
    params.max_x = sizeof(powerBuf)/sizeof(float);
    params.screen = sdlApp->window;
    params.renderer = sdlApp->renderer;
    params.offset_x = 0;
    params.offset_y = 190;

    memset(powerBuf, 0, sizeof(powerBuf));
#endif

    while (1) {

        char msg_tod[40];
        char msg_stm[40];
        char msg_kWhp[60];
        char msg_kWhn[60];
        float v_angle, c_angle, t_angle;
        float volt_value = 0;
        float curr_value = 0;
        float temp_value = 0;
        int doPlot = 0;
        time_t ct;

        if (sdlApp->conf->onHold) {
            SDL_Delay(4000);
            continue;
        }

        SDL_PollEvent(&event);

        if(event.type == SDL_QUIT || event.type == SDL_MOUSEBUTTONDOWN)
            break;

        if(event.type == SDL_FINGERDOWN)
        {
            if ((event.type=pageSelect(sdlApp, &event)))
                break;
        }

        ct = time(NULL);    // Get a timestamp for this turn
        strftime(msg_tod, sizeof(msg_tod),TIMEDATFMT, localtime(&ct));

        if (!(ct - cnmea.volt_ts > S_TIMEOUT)) {
            sprintf(msg_volt, "%.1f", cnmea.volt);
            volt_value = cnmea.volt;
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
                sprintf(msg_kWhn, "%.3f kWh consumed since %s", cnmea.kWhn, msg_stm);
            else
                sprintf(msg_kWhn, "%.1f kWh consumed since %s", cnmea.kWhn, msg_stm);
            
            if (cnmea.kWhp < 1.0)
                sprintf(msg_kWhp, "%.3f kWh charged. Net : %.3f kWh", cnmea.kWhp, cnmea.kWhp - cnmea.kWhn);
            else
                sprintf(msg_kWhp, "%.1f kWh charged. Net : %.3f kWh", cnmea.kWhp, cnmea.kWhp - cnmea.kWhn);
        }
 
        SDL_RenderCopy(sdlApp->renderer, Background_Tx, NULL, NULL);

        SDL_RenderCopyEx(sdlApp->renderer, gaugeVolt, NULL, &gaugeVoltR, 0, NULL, SDL_FLIP_NONE);
        SDL_RenderCopyEx(sdlApp->renderer, gaugeCurr, NULL, &gaugeCurrR, 0, NULL, SDL_FLIP_NONE);
        SDL_RenderCopyEx(sdlApp->renderer, gaugeTemp, NULL, &gaugeTempR, 0, NULL, SDL_FLIP_NONE);

        if (!(ct - cnmea.volt_ts > S_TIMEOUT || volt_value < v_min || volt_value > v_max )) {
            v_angle = ((volt_value-v_scaleoffset) * (v_maxangle/v_max) *2)+v_offset;
            SDL_RenderCopyEx(sdlApp->renderer, needleVolt, NULL, &voltNeedleR, v_angle, NULL, SDL_FLIP_NONE);

            get_text_and_rect(sdlApp->renderer, 164, 170, 0, msg_volt, fontSmall, &textField, &textField_rect, BLACK);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

            get_text_and_rect(sdlApp->renderer, 146, 240, 0, msg_volt_bank, fontLarge, &textField, &textField_rect, BLACK);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        }

        if (!(ct -  cnmea.curr_ts > S_TIMEOUT)) {
            if (fabs(curr_value) < 33 ) {
                c_angle = (((curr_value*0.5)-c_scaleoffset) * (c_maxangle/c_max)*2)+c_offset;
                SDL_RenderCopyEx(sdlApp->renderer, needleCurr, NULL, &currNeedleR, c_angle, NULL, SDL_FLIP_NONE);
            }

            get_text_and_rect(sdlApp->renderer, 386, 170, 0, msg_curr, fontSmall, &textField, &textField_rect, BLACK);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

            get_text_and_rect(sdlApp->renderer, 370, 240, 0, msg_curr_bank, fontLarge, &textField, &textField_rect, BLACK);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        }

        if (!(ct -  cnmea.temp_ts > S_TIMEOUT)) {
            t_angle = ((temp_value-t_scaleoffset) * (t_maxangle/t_max)*1.2)+t_offset;
            SDL_RenderCopyEx(sdlApp->renderer, needleTemp, NULL, &tempNeedleR, t_angle, NULL, SDL_FLIP_NONE);

            get_text_and_rect(sdlApp->renderer, 605, 170, 0, msg_temp, fontSmall, &textField, &textField_rect, BLACK);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

            get_text_and_rect(sdlApp->renderer, 586, 240, 0, msg_temp_loca, fontLarge, &textField, &textField_rect, BLACK);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        }

        if (sdlApp->conf->netStat == 1) {
           SDL_RenderCopyEx(sdlApp->renderer, netStatBar, NULL, &netStatbarR, 0, NULL, SDL_FLIP_NONE);
        }

        SDL_RenderCopyEx(sdlApp->renderer, menuBar, NULL, &menuBarR, 0, NULL, SDL_FLIP_NONE);
        addMenuItems(sdlApp, fontSmall);

        get_text_and_rect(sdlApp->renderer, 650, 10, 0, msg_tod, fontTod, &textField, &textField_rect, WHITE);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        if (cnmea.startTime)
        {
            get_text_and_rect(sdlApp->renderer, 104, 416, 0, msg_kWhn, fontSmall, &textField, &textField_rect, BLACK);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
            get_text_and_rect(sdlApp->renderer, 104, 432, 0, msg_kWhp, fontSmall, &textField, &textField_rect, BLACK);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        }

        if (subTaskbar != NULL) {
            SDL_RenderCopyEx(sdlApp->renderer, subTaskbar, NULL, &subTaskbarR, 0, NULL, SDL_FLIP_NONE);
        }

#ifdef PLOTSDL
        if (cnmea.startTime)
        {
            // The captionlist and coordlist lists
            captionlist caption_list = NULL;
            coordlist coordinate_list = NULL;
            int j=0;

            // Hidden but must be defined
            caption_list=push_back_caption(caption_list,"Power consumption", 0, (curr_value >=0? 0x00FF00 : 0xFF0000));

            int avtp = 0;
            float avpw = 0;

            // Populate plot parameter object
            powerBuf[0] = fabs(cnmea.volt * cnmea.curr);

            for (int i=sizeof(powerBuf)/sizeof(float); i >0; i--)
            {
                coordinate_list=push_back_coord(coordinate_list, 0, j, powerBuf[j]);
                if (powerBuf[j] && avtp < 6) {
                    avpw += powerBuf[j];
                    avtp++;
                }
                if (j++)
                    powerBuf[i] = powerBuf[i-1];    // History shift
            }

            avpw /= avtp;

            // Adjust y-scale according to sampled average watt
            params.max_y = 1000; params.scale_y = 75;    // Default
            if (avpw < 500) {params.max_y = 500;    params.scale_y = 50;}
            if (avpw < 200) {params.max_y = 220;    params.scale_y = 20;}
            if (avpw < 100) {params.max_y = 120;    params.scale_y = 10;}
            if (avpw < 40)  {params.max_y = 50;     params.scale_y = 5;}
            if (avpw < 20)  {params.max_y = 30;     params.scale_y = 3;}
            if (avpw < 6)   {params.max_y = 8;      params.scale_y = 1;}
            //if (avpw < 3)   {params.max_y = 5;      params.scale_y = 1;}
           
            params.caption_list = caption_list;
            if(doPlot)
	            params.coordinate_list = coordinate_list;

            plot_graph(&params);
        }
#endif

        SDL_RenderPresent(sdlApp->renderer);
 
        if (sdlApp->conf->runVnc && sdlApp->conf->vncClients && sdlApp->conf->vncPixelBuffer) {
            // Read the pixels from the current render target and save them onto the surface
            // This will slow down the application a bit.
            SDL_RenderReadPixels(sdlApp->renderer, NULL, SDL_GetWindowPixelFormat(sdlApp->window),
                sdlApp->conf->vncPixelBuffer->pixels, sdlApp->conf->vncPixelBuffer->pitch);
            rfbMarkRectAsModified(sdlApp->conf->vncServer, 0, 0, WINDOW_W, WINDOW_H);
        }

        SDL_Delay(1000);
    }

    if (subTaskbar != NULL) {
        SDL_DestroyTexture(subTaskbar);
    }

#ifdef PLOTSDL
    params.screen_width=0;
    plot_graph(&params);
#endif
    SDL_DestroyTexture(gaugeVolt);
    SDL_DestroyTexture(gaugeCurr);
    SDL_DestroyTexture(gaugeTemp);
    SDL_DestroyTexture(needleVolt);
    SDL_DestroyTexture(needleCurr);
    SDL_DestroyTexture(needleTemp);
    SDL_DestroyTexture(menuBar);
    SDL_DestroyTexture(netStatBar);    
    TTF_CloseFont(fontTod);
    TTF_CloseFont(fontSmall);
    TTF_CloseFont(fontLarge);
    IMG_Quit();

    return event.type;
}


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
    SDL_Event event;
    SDL_Rect menuBarR;

    TTF_Font* fontCAL =  TTF_OpenFont(sdlApp->fontPath, 28);
    TTF_Font* fontPRG =  TTF_OpenFont(sdlApp->fontPath, 11);
    TTF_Font* fontSrc = TTF_OpenFont(sdlApp->fontPath, 14);

    SDL_Texture* menuBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "menuBar.png");

    CURL *curl = NULL;

    calRunner doRun;

    if (!configParams->i2cFile)
        return SDL_MOUSEBUTTONDOWN;

    doRun.run = 1;
    doRun.i2cFile = configParams->i2cFile;

    SDL_Texture* textField;
    SDL_Rect textField_rect;

    menuBarR.w = 340;
    menuBarR.h = 50;
    menuBarR.x = 430;
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

        SDL_PollEvent(&event);

        if(event.type == SDL_QUIT || event.type == SDL_MOUSEBUTTONDOWN)
            break;

        if(event.type == SDL_FINGERDOWN)
        {
            if ((event.type=pageSelect(sdlApp, &event)))
                break;
        }

        SDL_RenderCopy(sdlApp->renderer, Background_Tx, NULL, NULL);

        if (seconds ++ > 10) {
            sprintf(msg_cal, "Calibration about to begin in %d seconds", progress--);
            seconds = 0;
            if (progress < 0) break;
        }   

        get_text_and_rect(sdlApp->renderer, 10, 250, 1, msg_cal, fontCAL, &textField, &textField_rect, BLACK);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        SDL_RenderCopyEx(sdlApp->renderer, menuBar, NULL, &menuBarR, 0, NULL, SDL_FLIP_NONE);
        addMenuItems(sdlApp, fontSrc);

        SDL_RenderPresent(sdlApp->renderer); 
        
        SDL_Delay(100);
    }

    if (progress < 0) {

        progress = cperiod;   // Will unconditionally last for 'cperiod' seconds

        while (1) {

            event.type = COGPAGE;

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

            get_text_and_rect(sdlApp->renderer, 10, 250, 1, msg_cal, fontCAL, &textField, &textField_rect, BLACK);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

            get_text_and_rect(sdlApp->renderer, 10, 320, 1, doRun.progress, fontPRG, &textField, &textField_rect, BLACK);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

            SDL_RenderPresent(sdlApp->renderer); 
            
            SDL_Delay(100); 
        } 
    }

    doRun.run = 0;  // Quit the thread

    SDL_Delay(1000); 
    SDL_Log("Calibration completed");

    SDL_DestroyTexture(menuBar);
    TTF_CloseFont(fontCAL);
    TTF_CloseFont(fontPRG);
    TTF_CloseFont(fontSrc);

    return event.type;
}

static void closeSDL2(sdl2_app *sdlApp)
{
    TTF_Quit();
    SDL_DestroyRenderer(sdlApp->renderer);
    SDL_DestroyWindow(sdlApp->window);
    SDL_VideoQuit();
    SDL_Quit();
}

static int openSDL2(configuration *configParams, sdl2_app *sdlApp)
{
    SDL_Surface* Loading_Surf;
    SDL_Thread *threadNmea = NULL;
    SDL_Thread *threadI2C = NULL;
    SDL_Thread *threadGPS = NULL;
    SDL_Thread *threadMon = NULL;
    SDL_Thread *threadVNC = NULL;

    // This is what this application is built for!
    setenv("SDL_VIDEODRIVER", "RPI", 1);

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,  "Couldn't initialize SDL. Video driver %s!", SDL_GetError());
        return SDL_QUIT;
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

        if (NULL == threadI2C) {
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

     if (configParams->runMon == 1) {
        threadMon = SDL_CreateThread(threadMonstat, "threadMonstat", configParams);
        if (threadMon != NULL)
            SDL_DetachThread(threadMon);
        configParams->runMon = 2;
    }

    SDL_ShowCursor(SDL_DISABLE);

    if ((sdlApp->window = SDL_CreateWindow("sdlSpeedometer",
            SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED,
            WINDOW_W, WINDOW_H,
            SDL_WINDOW_RESIZABLE)) == NULL) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateWindow failed: %s", SDL_GetError());
            configParams->runGps = configParams->runi2c = configParams->runNet = 0;
            return SDL_QUIT;
    }

    sdlApp->renderer = SDL_CreateRenderer(sdlApp->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    TTF_Init();

    Loading_Surf = SDL_LoadBMP(DEFAULT_BACKGROUND);
    Background_Tx = SDL_CreateTextureFromSurface(sdlApp->renderer, Loading_Surf);
    SDL_FreeSurface(Loading_Surf);

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
        if ((rval=sqlite3_prepare_v2(configParams->conn, "select task,args from subtasks", -1, &res, &tail)) == SQLITE_OK)
        {
            while (sqlite3_step(res) != SQLITE_DONE) {  
    
                strcpy((sdlApp->subAppsCmd[c][0]=(char*)malloc(PATH_MAX)), (char*)sqlite3_column_text(res, 0));
                strcpy((sdlApp->subAppsCmd[c][1]=(char*)malloc(PATH_MAX)), (char*)sqlite3_column_text(res, 1));

                sprintf(subtask, "which %s", sdlApp->subAppsCmd[c][0]);
                *buff = '\0';
                if ((fd = popen(subtask, "r")) != NULL) {
                    fgets(buff, sizeof(buff) , fd);
                    pclose(fd);
                }
                if (strlen(buff) < 2)
                    sdlApp->subAppsCmd[c][0] = NULL;

                if (c++ >= TSKPAGE) break;
            }
            if (c >1)
                sdlApp->subAppsCmd[0][0] = "1"; // Checked
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
    int runners[3];
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
    configParams->runGps = configParams->runi2c = configParams->runNet = 0;

    while(configParams->numThreads)
        SDL_Delay(100);

    closeSDL2(sdlApp);
    
    configParams->subTaskPID = fork ();

    if (configParams->subTaskPID == 0) {
        // Child
        setpgid(0, 0);
        prctl(PR_SET_PDEATHSIG, SIGINT);
        execv ("/bin/bash", args);
    }

    // You've picked my bones clean, speak now ...
    waitpid(configParams->subTaskPID, &status, 0);
    // ... before I reclaim the meat. (Solonius)
    configParams->subTaskPID = 0;

    (void)configureDb(configParams);   // Fetch eventually a new configuration

    // Regain SDL2 control
    configParams->runGps = runners[0];
    configParams->runi2c = runners[1];
    configParams->runNet = runners[2];
    status = openSDL2(configParams, sdlApp);

    if (status == 0)    
        status = sdlApp->curPage;

    return status;
}

int main(int argc, char *argv[])
{
    int c, step;
    configuration configParams;
    sdl2_app sdlApp;

    memset(&cnmea, 0, sizeof(cnmea));
    memset(&sdlApp, 0, sizeof(sdlApp));
    memset(&configParams, 0, sizeof(configParams));
    sdlApp.conf = &configParams;

    sdlApp.fontPath = DEFAULT_FONT;

    SDL_LogSetOutputFunction((void*)logCallBack, argv[0]);   

    configParams.runGps = configParams.runi2c = configParams.runNet = configParams.runMon = 1;
        
    sdlApp.nextPage = COGPAGE; // Start-page for touch
    step = COGPAGE; // .. mouse

    (void)configureDb(&configParams);   // Fetch configuration

    while ((c = getopt (argc, argv, "hsvginV")) != -1)
    {
        switch (c)
            {
            case 's':
                useSyslog = 1;
                break;
            case 'g':   configParams.runGps = 0;    // Diable GPS data collection
                break;
            case 'i':   configParams.runi2c = 0;    // Disable i2c data collection
                break;
            case 'n':   configParams.runNet = 0;    // Disable NMEA net data collection
                break;
            case 'V':   configParams.runVnc = 1;    // Enable VNC server
                break;
            case 'v':
                fprintf(stderr, "revision: %s\n", SWREV);
                exit(EXIT_SUCCESS);
                break;
            case 'h':
            default:
                fprintf(stderr, "Usage: %s -s (use syslog) -g -i -n -V -v (version)\n", basename(argv[0]));
                fprintf(stderr, "       Where: -g Disable GPS : -i Disable i2c : -n Disabe NMEA Net : -V Enable VNC Server\n");
                exit(EXIT_FAILURE);
                break;
            }
    }

    if (useSyslog) {
        setlogmask (LOG_UPTO (LOG_NOTICE));
        openlog (basename(argv[0]), LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);
        syslog (LOG_NOTICE, "Program started by User %d", getuid ());
    }

    if (configParams.runVnc == 1) {
        // Create an empty RGB surface that will be used to hold the VNC pixel buffer
        configParams.vncPixelBuffer = SDL_CreateRGBSurface(0, WINDOW_W, WINDOW_H, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000); 
        if (configParams.vncPixelBuffer != NULL) {
            rfbErr=SDL_Log; 
            rfbLog=SDL_Log;
            configParams.vncServer=rfbGetScreen(&argc, argv, WINDOW_W, WINDOW_H, 8, 3, 4);           
            configParams.vncServer->frameBuffer = configParams.vncPixelBuffer->pixels;
            configParams.vncServer->ipv6port = 0;
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateRGBSurfac failed: %s. VNC Server disabled!", SDL_GetError());
            configParams.runVnc = 0;
        }
    }

    if (openSDL2(&configParams, &sdlApp))
        exit(EXIT_FAILURE);

    (void)checkSubtask(&sdlApp, &configParams);

    if (configParams.runVnc) {
        rfbLog=(rfbLogProc)nullLog;
    }

    while(1)
    {
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
            case CALPAGE: sdlApp.nextPage = doCalibration(&sdlApp, &configParams);
                break;
            case TSKPAGE: sdlApp.nextPage = doSubtask(&sdlApp, &configParams);
                break;
            case SDL_MOUSEBUTTONDOWN:
                    if (++step >6) {
                        sdlApp.nextPage = step = COGPAGE;
                    } else{ sdlApp.nextPage = step; }
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

    configParams.runGps = configParams.runi2c = configParams.runNet = configParams.runMon = 0;

    // .. and let them close cleanly
    while(configParams.numThreads)
        SDL_Delay(100);
    
    closeSDL2(&sdlApp);

    SDL_Log("User terminated");

    exit(EXIT_SUCCESS);
}
