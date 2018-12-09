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
#include <sqlite3.h>
#include <curl/curl.h>
#include <termios.h>
#include <libgen.h>
#include <time.h>
#include <syslog.h>
#include <errno.h>

#include "sdlSpeedometer.h"

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    char *fontPath;
    char *subAppsCmd[TSKPAGE][TSKPAGE];
    int nextPage;
    int curPage;
    sqlite3 *conn3;
} sdl2_app;
 
#define TIMEDATFMT  "%x - %H:%M %Z"

#define WINDOW_W 800        // Resolution
#define WINDOW_H 480

#define S_TIMEOUT     4     // Invalidate current sentences after # seconds without a refresh from talker.
#define NMPARSE(str, nsent) !strncmp(nsent, &str[3], strlen(nsent))

#define DEFAULT_FONT        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf";
#define TTY_GPS             "/dev/ttyS0"    // RPI 3B+

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

static int useSyslog = 0;

static SDL_Texture* Background_Tx;

static collected_nmea cnmea;

void* logCallBack(char *userdata, int category, SDL_LogPriority priority, const char *message)
{
    FILE *out = priority == SDL_LOG_PRIORITY_ERROR? stderr : stdout;

    if (useSyslog)
        syslog (LOG_NOTICE, message, getuid ());
    else
        fprintf(out, "[%s] %s\n", basename(userdata), message);

    return 0;
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

                    sqlite3_prepare_v2(conn, "CREATE TABLE config (Id INTEGER PRIMARY KEY, rev TEXT, tty TEXT, baud INTEGER, server TEXT, port INTEGER)", -1, &res, &tail);
                    sqlite3_step(res);

                    sprintf(buf, "INSERT INTO config (rev,tty,baud,server,port) VALUES ('%s','%s',9600,'127.0.0.1',10110)", SWREV,TTY_GPS);
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
                    sprintf(buf, "INSERT INTO subtasks (task,args) VALUES ('xterm','-maximized -e /usr/bin/sudo /usr/bin/raspi-config')");
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
    rval = sqlite3_prepare_v2(conn, "select tty,baud,server,port from config", -1, &res, &tail);        
    if (rval == SQLITE_OK && sqlite3_step(res) == SQLITE_ROW) {
        strcpy(configParams->tty,       (char*)sqlite3_column_text(res, 0));
        configParams->baud =            sqlite3_column_int(res, 1);
        strcpy(configParams->server,    (char*)sqlite3_column_text(res, 2));
        configParams->port =            sqlite3_column_int(res, 3);
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
static int nmeaChecksum(char * buffer, int cnt)
{
    uint8_t checksum;
    int i, cs;
    int debug = 0;

    cs = checksum = 0;

    for (i = 0; i < cnt; i++) {
        if (buffer[i] == '*') cs=i+1;
        if (buffer[i] == '\r' || buffer[i] == '\n') { buffer[i] = '\0'; break; }
    }

    if (cs > 0) {
        for (i=0; i < cs-1; i++) {
            if (buffer[i] == '$' || buffer[i] == '!') continue;
            checksum ^= (uint8_t)buffer[i];
        }
    }  

    if ( !cs || (checksum != (uint8_t)strtol(&buffer[cs], NULL, 16))) {
        if (debug) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Checksum error in nmea sentence: 0x%02x/0x%02x - '%s'/'%s', pos %d", \
                checksum, (uint8_t)strtol(&buffer[cs], NULL, 16), buffer, &buffer[cs], cs);
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

    while(configParams->runGps)
    {
        time_t ct;
        int cnt;

        // The vessels network has precedence
        if (!(time(NULL) - cnmea.net_ts > S_TIMEOUT)) {
            SDL_Delay(4000);
            printf("on hold again\n");
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

        if(nmeaChecksum(buffer, strlen(buffer))) continue;

        ct = time(NULL);    // Get a timestamp for this turn 

        // RMC - Recommended minimum specific GPS/Transit data
        // RMC feed is assumed to be present at all time 
        if (NMPARSE(buffer, "RMC")) {
            cnmea.rmc=atof(getf(7, buffer));
            strcpy(cnmea.gll, getf(3, buffer));
            strcpy(cnmea.glo, getf(5, buffer));
            strcpy(cnmea.glns, getf(4, buffer));
            strcpy(cnmea.glne, getf(6, buffer));
            if (cnmea.rmc_time_ts == 0 ) {
                strcpy(cnmea.time, getf(1, buffer));
                strcpy(cnmea.date, getf(9, buffer));
                cnmea.rmc_time_ts = 1;
            }
            if(strlen(cnmea.gll))
                cnmea.gll_ts = cnmea.rmc_ts = ct;
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
                cnmea.rmc=atof(getf(5, buffer));
                cnmea.rmc_ts = ct;
                continue;
            }
        }

    }

    close(fd);

    SDL_Log("threadSerial stopped");

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
    int update = 0;
    const char *tail;
    sqlite3 *conn = NULL;
    sqlite3_stmt *res;
    calibration calib;
    struct stat sb;
    FILE *fd;

    if( (configParams->i2cFile = i2cinit(bus)) < 0) {
	    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Unable to run the i2c system!");
        return 0;
    }

    configParams->conn3 = NULL;

    SDL_Log("Starting up i2c collector");

    if (sqlite3_open_v2(SQLDBPATH, &conn, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, 0)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open configuration databas : %s", (char*)sqlite3_errmsg(conn));
        (void)sqlite3_close(conn);
        configParams->conn3 = conn = NULL;
    }

    if (conn)
        configParams->conn3 = conn;

    while(configParams->runi2c)
    {
        time_t ct;
        float hdm;

        SDL_Delay(dt);

        if (conn && connOk) {
            if (update++ > dt / 10) {
                if (!stat(SQLCONFIG, &sb)) {
                    SDL_Delay(600);
                    char sqlbuf[150];
                    memset(sqlbuf, 0, sizeof(sqlbuf));
                    SDL_Log("Got new calibration:");
                    if ((fd = fopen(SQLCONFIG, "r")) != NULL) {
                        if (fread(sqlbuf, 1, sizeof(sqlbuf), fd) > 0) {
                            SDL_Log("  %s", &sqlbuf[12]);
                            if (sqlite3_prepare_v2(conn, sqlbuf, -1,  &res, &tail)  == SQLITE_OK) {
                                if (sqlite3_step(res) != SQLITE_DONE) {
                                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to step calibration data : %s", (char*)sqlite3_errmsg(conn)); 
                                }
                                sqlite3_finalize(res);
                            } else { SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to update calibration data : %s", (char*)sqlite3_errmsg(conn)); }
                        } else {
                            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to read calibration data : %s", strerror(errno));
                        }                      
                    } else {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open calibration data file : %s", strerror(errno));
                    }
                    unlink(SQLCONFIG);
                }

                rval = sqlite3_prepare_v2(conn, "select magXmax,magYmax,magZmax,magXmin,magYmin,magZmin,declval from calib", -1, &res, &tail);        
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
                            (char*)sqlite3_errmsg(conn));
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
            cnmea.hdm=hdm;
            cnmea.hdm_i2cts = ct;
        }      
    }

    if (conn)
        sqlite3_close(conn);

    close(configParams->i2cFile);
    configParams->i2cFile = 0;

    SDL_Log("i2cCollector stopped");

    return 0;
}

/*
- x, y: upper left corner.
- texture, rect: outputs.
*/
static void get_text_and_rect(SDL_Renderer *renderer, int x, int y, int l, char *text,
        TTF_Font *font, SDL_Texture **texture, SDL_Rect *rect) {
    int text_width;
    int text_height;
    int f_width;
    int f_height;
    SDL_Surface *surface;
    SDL_Color textColor = {0, 0, 0, 0};

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

    while(1)
    {
        int retry = 0;
        int rretry = 0;

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
        int activeSockets = SDLNet_CheckSockets(socketSet, 5000);

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
            static char buffer[2048];
            time_t ts;
            int cnt = 0;

            if (++retry > 10) break;

            SDL_Delay(30);

            ts = time(NULL);        // Get a timestamp for this turn

            // Check if we got an NMEA response from the server
            if (SDLNet_CheckSockets(socketSet, 2000) > 0)
            {

                if ((cnt = SDLNet_TCP_Recv(clientSocket, buffer, sizeof(buffer))) <= 0)
                    continue;

                retry = 0;

                if (nmeaChecksum(buffer, cnt)) continue;

                // RMC - Recommended minimum specific GPS/Transit data
                if (NMPARSE(buffer, "RMC")) {
                    cnmea.rmc=atof(getf(7, buffer));
                    cnmea.rmc_ts = ts;
                    strcpy(cnmea.gll, getf(3, buffer));
                    strcpy(cnmea.glo, getf(5, buffer));
                    strcpy(cnmea.glns, getf(4, buffer));
                    strcpy(cnmea.glne, getf(6, buffer));
                    if (cnmea.rmc_time_ts == 0) {
                        strcpy(cnmea.time, getf(1, buffer));
                        strcpy(cnmea.date, getf(9, buffer));
                        cnmea.rmc_time_ts = 1;
                    }
                    cnmea.net_ts = cnmea.gll_ts = ts;
                    continue;
                }

                // GLL - Geographic Position, Latitude / Longitude
                if (ts - cnmea.gll_ts > S_TIMEOUT/2) { // If not from RMC
                    if (NMPARSE(buffer, "GLL")) {
                        strcpy(cnmea.gll, getf(1, buffer));
                        strcpy(cnmea.glo, getf(3, buffer));
                        strcpy(cnmea.glns, getf(2, buffer));
                        strcpy(cnmea.glne, getf(4, buffer));
                        cnmea.net_ts = cnmea.gll_ts = ts;
                        continue;
                    }
                }

                // VTG - Track made good and ground speed
                if (ts - cnmea.rmc_ts > S_TIMEOUT/2) { // If not from RMC
                    if (NMPARSE(buffer, "VTG")) {
                        cnmea.rmc=atof(getf(5, buffer));
                        cnmea.net_ts = cnmea.rmc_ts = ts;
                        continue;
                    }
                }

                // VHW - Water speed and Heading
                if(NMPARSE(buffer, "VHW")) {
                    if ((cnmea.hdm=atof(getf(3, buffer))) != 0)
                        cnmea.hdm_ts = ts;
                    cnmea.stw=atof(getf(5, buffer));
                    cnmea.stw_ts = ts;
                    continue;
                }

                // DPT - Depth (Depth of transponder added)
                if (NMPARSE(buffer, "DPT")) {
                    cnmea.dbt=atof(getf(1, buffer))+atof(getf(2, buffer));
                    cnmea.dbt_ts = ts;
                    continue;
                }

                // DBT - Depth Below Transponder
                if (ts - cnmea.dbt_ts > S_TIMEOUT/2) { // If not from DPT
                    if (NMPARSE(buffer, "DBT")) {
                        cnmea.dbt=atof(getf(3, buffer));
                        cnmea.dbt_ts = ts;
                        continue;
                    }
                }

                // MTW - Water temperature in C
                if (NMPARSE(buffer, "MTW")) {
                    cnmea.mtw=atof(getf(1, buffer));
                    cnmea.mtw_ts = ts;
                    continue;
                }

                // MWV - Wind Speed and Angle (report VWR style)
                if (NMPARSE(buffer, "MWV")) {
                    if (strncmp(getf(2, buffer),"R",1) + strncmp(getf(4, buffer),"N",1) == 0) {
                        cnmea.vwra=atof(getf(1, buffer));
                        cnmea.vwrs=atof(getf(3, buffer))/1.94; // kn 2 m/s;
                        if (cnmea.vwra > 180) {
                            cnmea.vwrd = 1;
                            cnmea.vwra = 360 - cnmea.vwra;
                        } else cnmea.vwrd = 0;
                        cnmea.vwr_ts = ts;
                    } else if (strncmp(getf(2, buffer),"T",1) + strncmp(getf(4, buffer),"N",1) == 0) {
                        cnmea.vwta=atof(getf(1, buffer));
                        cnmea.vwts=atof(getf(3, buffer))/1.94; // kn 2 m/s;
                        cnmea.vwt_ts = ts;
                    }
                    continue;
                }

                // VWR - Relative Wind Speed and Angle (obsolete)
                if (ts - cnmea.vwr_ts > S_TIMEOUT/2) { // If not from MWV
                    if (NMPARSE(buffer, "VWR")) {
                        cnmea.vwra=atof(getf(1, buffer));
                        cnmea.vwrs=atof(getf(3, buffer))/1.94; // kn 2 m/s
                        cnmea.vwrd=strncmp(getf(2, buffer),"R",1)==0? 0:1;
                        cnmea.vwr_ts = ts;
                        continue;
                    }
                }

            } else {
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

        if (configParams->runNet)
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Server %s possibly gone, awaiting its return", configParams->server);
    }

    if (clientSocket && socketSet) {
        SDLNet_TCP_DelSocket(socketSet, clientSocket);
        SDLNet_TCP_Close(clientSocket);
        SDLNet_FreeSocketSet(socketSet);
    }

    SDL_Log("nmeaNetCollector stopped");

    return 0;
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


    if (y > 400  && y < 440)
    {
        if (x > 450 && x < 480)
            return COGPAGE;
        if (x > 504 && x < 544)
            return SOGPAGE;
        if (x > 560 && x < 600)
            return DPTPAGE;
        if (x > 614 && x < 656)
            return WNDPAGE;
        if (x > 670 && x < 710)
           return GPSPAGE;
        if (x > 730 && x < 770)
           return CALPAGE;
        if (sdlApp->subAppsCmd[sdlApp->curPage][0] != NULL) {
            if (x > 30 && x < 80)
                return TSKPAGE;
        }
    }
    return 0;
}

static void addMenuItems(SDL_Renderer *renderer, TTF_Font *font)
{  
    // Add text on top of a simple menu bar

    SDL_Texture* textM1;
    SDL_Rect M1_rect;

    get_text_and_rect(renderer, 440, 416, 0, "COG", font, &textM1, &M1_rect);
    SDL_RenderCopy(renderer, textM1, NULL, &M1_rect); SDL_DestroyTexture(textM1);

    get_text_and_rect(renderer, 498, 416, 0, "SOG", font, &textM1, &M1_rect);
    SDL_RenderCopy(renderer, textM1, NULL, &M1_rect); SDL_DestroyTexture(textM1);

    get_text_and_rect(renderer, 556, 416, 0, "DPT", font, &textM1, &M1_rect);
    SDL_RenderCopy(renderer, textM1, NULL, &M1_rect); SDL_DestroyTexture(textM1);

    get_text_and_rect(renderer, 610, 416, 0, "WND", font, &textM1, &M1_rect);
    SDL_RenderCopy(renderer, textM1, NULL, &M1_rect); SDL_DestroyTexture(textM1);

    get_text_and_rect(renderer, 668, 416, 0, "GPS", font, &textM1, &M1_rect);
    SDL_RenderCopy(renderer, textM1, NULL, &M1_rect); SDL_DestroyTexture(textM1);
    
    get_text_and_rect(renderer, 726, 416, 0, "CAL", font, &textM1, &M1_rect);
    SDL_RenderCopy(renderer, textM1, NULL, &M1_rect); SDL_DestroyTexture(textM1);
}

static void setUTCtime(void)
{
    struct tm *settm = NULL;
    time_t rawtime, sys_rawtime;
    char buf[40];
    static int fails;
;
    if (++fails > 20) {
        // No luck - give up this
        cnmea.rmc_time_ts = 2;
        return;
    }
    // UTC of position in hhmmss.sss format + UTC date of position fix, ddmmyy format 
    if (16 != strlen(cnmea.time) + strlen(cnmea.date)) {
        cnmea.rmc_time_ts = 0;  // Try again
        return;
    }

    cnmea.rmc_time_ts = 2;      // One time only

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

// Present the compass with heading ant roll
static int doCompass(sdl2_app *sdlApp)
{
    SDL_Event event;
    SDL_Rect compassR, outerRingR, clinoMeterR, menuBarR, subTaskbarR;
    TTF_Font* fontCog = TTF_OpenFont(sdlApp->fontPath, 42);
    TTF_Font* fontRoll = TTF_OpenFont(sdlApp->fontPath, 22);
    TTF_Font* fontSrc = TTF_OpenFont(sdlApp->fontPath, 14);
    TTF_Font* fontTod = TTF_OpenFont(sdlApp->fontPath, 12);

    SDL_Texture* compassRose = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "compassRose.png");
    SDL_Texture* outerRing = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "outerRing.png");
    SDL_Texture* clinoMeter = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "clinometer.png");
    SDL_Texture* menuBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "menuBar.png");

    SDL_Texture* subTaskbar = NULL;

    sdlApp->curPage = COGPAGE;

    if (sdlApp->subAppsCmd[sdlApp->curPage][0] != NULL) {
        char icon[PATH_MAX];
        sprintf(icon , "%s/%s.png", IMAGE_PATH, sdlApp->subAppsCmd[sdlApp->curPage][0]);
        subTaskbar = IMG_LoadTexture(sdlApp->renderer, icon);
    }

    compassR.w = 366;
    compassR.h = 366;
    compassR.x = 56;
    compassR.y = 56;

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

    SDL_Texture* textField;
    SDL_Rect textField_rect;

    float t_angle = 0;
    float angle = 0;
    float t_roll = 0;
    float roll = 0;
    float c_angle = 0;

    while (1) {
        char msg_hdm[40] = { " " };
        char msg_roll[40 ]= { " " };
        char msg_sog[40] = { " " };
        char msg_stw[40] = { " " };
        char msg_dbt[40] = { " " };
        char msg_mtw[40] = { " " };
        char msg_src[40] = { " " };
        char msg_tod[40];
        time_t ct;

        SDL_PollEvent(&event);

        if(event.type == SDL_QUIT || event.type == SDL_MOUSEBUTTONDOWN)
            break;

        if(event.type == SDL_FINGERDOWN)
        {
            if ((event.type=pageSelect(sdlApp, &event)))
                break;
        }

        ct = time(NULL);    // Get a timestamp for this turn 

        if (!(ct - cnmea.rmc_ts > S_TIMEOUT)) {
            // Set system UTC time
            if (cnmea.rmc_time_ts == 1)
                setUTCtime();
        }

        strftime(msg_tod, sizeof(msg_tod),TIMEDATFMT, localtime(&ct));

        if (!(ct - cnmea.hdm_i2cts > S_TIMEOUT)) {                        // HDM & ROLL Magnetic
            sprintf(msg_hdm, "%.0f", cnmea.hdm);
            sprintf(msg_src, "mag");
        } else if (!(ct - cnmea.stw_ts > S_TIMEOUT || cnmea.stw == 0)) {  // VHW - Water speed and Heading NMEA
            sprintf(msg_stw, "STW: %.1f", cnmea.stw);
            sprintf(msg_hdm, "%.0f", cnmea.hdm);
            sprintf(msg_src, "net");
        }

        if (!(ct - cnmea.roll_i2cts > S_TIMEOUT))
            sprintf(msg_roll, "%.0f", fabs(roll=cnmea.roll));

        // RMC - Recommended minimum specific GPS/Transit data
        if (!(ct - cnmea.rmc_ts > S_TIMEOUT || cnmea.rmc < 1))
            sprintf(msg_sog, "SOG: %.1f", cnmea.rmc);

        // DBT - Depth Below Transponder
        if (!(ct - cnmea.dbt_ts > S_TIMEOUT))
            sprintf(msg_dbt, "DBT: %.1f", cnmea.dbt);

        // MTW - Water temperature in C
        if (!(ct - cnmea.mtw_ts > S_TIMEOUT))
            sprintf(msg_mtw, "TMP: %.1f", cnmea.mtw);
                      
        angle = roundf(cnmea.hdm);

        // Run needle and roll with smooth acceleration
        if ((c_angle >= 180 && angle <= 180) || (c_angle <= 180  && angle >= 180)) {
            t_angle = angle;  // Avoid smooth full turn aroud 0
        } else {
            if (angle > t_angle) t_angle += 0.8 * (fabsf(angle -t_angle) / 24);
            else if (angle < t_angle) t_angle -= 0.8 * (fabsf(angle -t_angle) / 24);
        }
        c_angle = angle;

        if (roll > t_roll) t_roll += 0.8 * (fabsf(roll -t_roll) / 10);
        else if (roll < t_roll) t_roll -= 0.8 * (fabsf(roll -t_roll) / 10);

        SDL_RenderCopy(sdlApp->renderer, Background_Tx, NULL, NULL);

        SDL_RenderCopyEx(sdlApp->renderer, outerRing, NULL, &outerRingR, 0, NULL, SDL_FLIP_NONE);
        SDL_RenderCopyEx(sdlApp->renderer, compassRose, NULL, &compassR, 360-t_angle, NULL, SDL_FLIP_NONE);
        
        if (!(ct - cnmea.roll_i2cts > S_TIMEOUT))
            SDL_RenderCopyEx(sdlApp->renderer, clinoMeter, NULL, &clinoMeterR, t_roll, NULL, SDL_FLIP_NONE);

        get_text_and_rect(sdlApp->renderer, 226, 180, 3, msg_src, fontSrc, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 196, 200, 3, msg_hdm, fontCog, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 223, 248, 2, msg_roll, fontRoll, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 500, 120, 0, msg_stw, fontCog, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 500, 170, 0, msg_sog, fontCog, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 500, 220, 0, msg_dbt, fontCog, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 500, 270, 0, msg_mtw, fontCog, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
       
        SDL_RenderCopyEx(sdlApp->renderer, menuBar, NULL, &menuBarR, 0, NULL, SDL_FLIP_NONE);
        addMenuItems(sdlApp->renderer, fontSrc);

        get_text_and_rect(sdlApp->renderer, 650, 10, 0, msg_tod, fontTod, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        if (subTaskbar != NULL) {
            SDL_RenderCopyEx(sdlApp->renderer, subTaskbar, NULL, &subTaskbarR, 0, NULL, SDL_FLIP_NONE);
        }

        SDL_RenderPresent(sdlApp->renderer);

        SDL_Delay(40);       
    }

    SDL_DestroyTexture(compassRose);
    SDL_DestroyTexture(outerRing);
    SDL_DestroyTexture(clinoMeter);
    SDL_DestroyTexture(menuBar);
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
    SDL_Rect gaugeR, needleR, menuBarR, subTaskbarR;
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

    SDL_Texture* gaugeSumlog = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "sumlog.png");
    SDL_Texture* gaugeNeedle = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "needle.png");
    SDL_Texture* menuBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "menuBar.png");

    SDL_Texture* subTaskbar = NULL;

    sdlApp->curPage = SOGPAGE;

    if (sdlApp->subAppsCmd[sdlApp->curPage][0] != NULL) {
        char icon[PATH_MAX];
        sprintf(icon , "%s/%s.png", IMAGE_PATH, sdlApp->subAppsCmd[sdlApp->curPage][0]);
        subTaskbar = IMG_LoadTexture(sdlApp->renderer, icon);
    }

    SDL_Texture* textField;
    SDL_Rect textField_rect;

    float t_angle = 0;
    float angle = 0;

    while (1) {
        char msg_stw[40];
        char msg_sog[40];
        char msg_dbt[40] = { " " };
        char msg_mtw[40] = { " " };
        char msg_hdm[40] = { " " };
        float speed, wspeed;
        int stw;
        char msg_tod[40];
        time_t ct;

        // Constants for instrument
        const float minangle = 13;  // Scale start
        const float maxangle = 237; // Scale end
        const float maxspeed = 10;

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
         if (ct - cnmea.stw_ts > S_TIMEOUT || cnmea.stw == 0) {
            sprintf(msg_stw, "----");
            wspeed = 0.0;
            stw = 0;
        } else {
            sprintf(msg_stw, "%.2f", cnmea.stw);
            wspeed = cnmea.stw;
            stw = 1;
        }

        // RMC - Recommended minimum specific GPS/Transit data
        if (ct - cnmea.rmc_ts > S_TIMEOUT || cnmea.rmc < 1.0)
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
            sprintf(msg_dbt, "DBT: %.1f", cnmea.dbt);

        // MTW - Water temperature in C
        if (!(ct - cnmea.mtw_ts > S_TIMEOUT))
            sprintf(msg_mtw, "TMP: %.1f", cnmea.mtw);
                         
        speed = wspeed * (maxangle/maxspeed);
        angle = roundf(speed+minangle);

        // Run needle with smooth acceleration
        if (angle > t_angle) t_angle += 0.8 * (fabsf(angle -t_angle) / 24) ;
        else if (angle < t_angle) t_angle -= 0.8 * (fabsf(angle -t_angle) / 24);

        SDL_RenderCopy(sdlApp->renderer, Background_Tx, NULL, NULL);
       
        SDL_RenderCopyEx(sdlApp->renderer, gaugeSumlog, NULL, &gaugeR, 0, NULL, SDL_FLIP_NONE);

        if (wspeed)
            SDL_RenderCopyEx(sdlApp->renderer, gaugeNeedle, NULL, &needleR, t_angle, NULL, SDL_FLIP_NONE);

        get_text_and_rect(sdlApp->renderer, 182, 300, 4, msg_stw, fontLarge, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 500, 120, 0, msg_hdm, fontCog, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 500, 170, 0, msg_dbt, fontCog, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 500, 220, 0, msg_mtw, fontCog, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        SDL_RenderCopyEx(sdlApp->renderer, menuBar, NULL, &menuBarR, 0, NULL, SDL_FLIP_NONE);

        get_text_and_rect(sdlApp->renderer, 650, 10, 0, msg_tod, fontTod, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

         if (stw) {
            get_text_and_rect(sdlApp->renderer, 186, 366, 8, msg_sog, fontSmall, &textField, &textField_rect);       
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        }

        addMenuItems(sdlApp->renderer, fontSrc);

        if (subTaskbar != NULL) {
            SDL_RenderCopyEx(sdlApp->renderer, subTaskbar, NULL, &subTaskbarR, 0, NULL, SDL_FLIP_NONE);
        }

        SDL_RenderPresent(sdlApp->renderer); 
        
        SDL_Delay(25);
    }

    SDL_DestroyTexture(gaugeSumlog);
    SDL_DestroyTexture(gaugeNeedle);
    SDL_DestroyTexture(menuBar);
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
    SDL_Rect gaugeR, menuBarR, subTaskbarR;

    TTF_Font* fontHD =  TTF_OpenFont(sdlApp->fontPath, 40);
    TTF_Font* fontLA =  TTF_OpenFont(sdlApp->fontPath, 30);
    TTF_Font* fontLO =  TTF_OpenFont(sdlApp->fontPath, 30);
    TTF_Font* fontMG =  TTF_OpenFont(sdlApp->fontPath, 14);
    TTF_Font* fontCog = TTF_OpenFont(sdlApp->fontPath, 42);
    TTF_Font* fontSrc = TTF_OpenFont(sdlApp->fontPath, 14);
    TTF_Font* fontTod = TTF_OpenFont(sdlApp->fontPath, 12);

    SDL_Texture* menuBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "menuBar.png");
    
    SDL_Texture* subTaskbar = NULL;

    sdlApp->curPage = GPSPAGE;

    if (sdlApp->subAppsCmd[sdlApp->curPage][0] != NULL) {
        char icon[PATH_MAX];
        sprintf(icon , "%s/%s.png", IMAGE_PATH, sdlApp->subAppsCmd[sdlApp->curPage][0]);
        subTaskbar = IMG_LoadTexture(sdlApp->renderer, icon);
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

    SDL_Texture* gaugeGps = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "gps.png");

    while (1) {

        char msg_hdm[40];
        char msg_lat[40];
        char msg_lot[40];
        char msg_src[40];
        char msg_dbt[40] = { " " };
        char msg_mtw[40] = { " " };
        char msg_sog[40] = { " " };
        char msg_stw[40] = { " " };
        char msg_tod[40];
        time_t ct;

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
            if (!(ct - cnmea.hdm_i2cts > S_TIMEOUT))
                sprintf(msg_src, "mag");
         }

        // RMC - Recommended minimum specific GPS/Transit data
        if (!(ct - cnmea.rmc_ts > S_TIMEOUT || cnmea.rmc < 1.0))
            sprintf(msg_sog, "SOG: %.1f", cnmea.rmc);

        // VHW - Water speed and Heading
         if (!(ct - cnmea.stw_ts > S_TIMEOUT || cnmea.stw == 0))
            sprintf(msg_stw, "STW: %.1f", cnmea.stw);
        
        // MTW - Water temperature in C
        if (!(ct - cnmea.mtw_ts > S_TIMEOUT))
            sprintf(msg_mtw, "TMP: %.1f", cnmea.mtw);

        // DBT - Depth Below Transponder
        if (!(ct - cnmea.dbt_ts > S_TIMEOUT))
            sprintf(msg_dbt, "DBT: %.1f", cnmea.dbt);
        
        SDL_RenderCopy(sdlApp->renderer, Background_Tx, NULL, NULL);
       
        SDL_RenderCopyEx(sdlApp->renderer, gaugeGps, NULL, &gaugeR, 0, NULL, SDL_FLIP_NONE);

        get_text_and_rect(sdlApp->renderer, 196, 142, 3, msg_hdm, fontHD, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 290, 168, 1, msg_src, fontMG, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 148, 222, 9, msg_lat, fontLA, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 148, 292, 9, msg_lot, fontLO, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
       
        get_text_and_rect(sdlApp->renderer, 500, 120, 0, msg_stw, fontCog, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 500, 170, 0, msg_sog, fontCog, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 500, 220, 0, msg_dbt, fontCog, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 500, 270, 0, msg_mtw, fontCog, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        SDL_RenderCopyEx(sdlApp->renderer, menuBar, NULL, &menuBarR, 0, NULL, SDL_FLIP_NONE);
        addMenuItems(sdlApp->renderer, fontSrc);

        get_text_and_rect(sdlApp->renderer, 650, 10, 0, msg_tod, fontTod, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        if (subTaskbar != NULL) {
            SDL_RenderCopyEx(sdlApp->renderer, subTaskbar, NULL, &subTaskbarR, 0, NULL, SDL_FLIP_NONE);
        }

        SDL_RenderPresent(sdlApp->renderer); 
        
        SDL_Delay(25);
    }

    SDL_DestroyTexture(gaugeGps);
    SDL_DestroyTexture(menuBar);
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
    SDL_Rect gaugeR, needleR, menuBarR, subTaskbarR;
    TTF_Font* fontLarge =  TTF_OpenFont(sdlApp->fontPath, 46);
    TTF_Font* fontSmall =  TTF_OpenFont(sdlApp->fontPath, 18);
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

    SDL_Texture* gaugeDepthW = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "depthw.png");
    SDL_Texture* gaugeDepth = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "depth.png");
    SDL_Texture* gaugeDepthx10 = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "depthx10.png");
    SDL_Texture* menuBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "menuBar.png");
    
    SDL_Texture* gaugeNeedle = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "needle.png");

    SDL_Texture* gauge;

    SDL_Texture* subTaskbar = NULL;

    sdlApp->curPage = DPTPAGE;

    if (sdlApp->subAppsCmd[sdlApp->curPage][0] != NULL) {
        char icon[PATH_MAX];
        sprintf(icon , "%s/%s.png", IMAGE_PATH, sdlApp->subAppsCmd[sdlApp->curPage][0]);
        subTaskbar = IMG_LoadTexture(sdlApp->renderer, icon);
    }

    SDL_Texture* textField;
    SDL_Rect textField_rect;

    float t_angle = 0;
    float angle = 0;

    while (1) {
        float depth;
        float scale; 
        char msg_dbt[40];
        char msg_mtw[40] = { " " };
        char msg_hdm[40] = { " " };
        char msg_stw[40] = { " " };
        char msg_rmc[40] = { " " };
        char msg_tod[40];
        time_t ct;

        // Constants for instrument
        const float minangle = 12;  // Scale start
        const float maxangle = 236; // Scale end
        const float maxsdepth = 10;

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
            sprintf(msg_dbt, "%.1f", cnmea.dbt);

        // MTW - Water temperature in C
        if (ct - cnmea.mtw_ts > S_TIMEOUT || cnmea.mtw == 0)
            sprintf(msg_mtw, "----");
        else
            sprintf(msg_mtw, "Temp :%.1f", cnmea.mtw);
        
        // Heading
        if (!(ct - cnmea.hdm_ts > S_TIMEOUT))
            sprintf(msg_hdm, "COG: %.0f", cnmea.hdm);

        // RMC - Recommended minimum specific GPS/Transit data
        if (!(ct - cnmea.rmc_ts > S_TIMEOUT || cnmea.rmc < 1.0))
             sprintf(msg_rmc, "SOG: %.1f", cnmea.rmc);
        
        // VHW - Water speed and Heading
         if (!(ct - cnmea.stw_ts > S_TIMEOUT || cnmea.stw == 0))
            sprintf(msg_stw, "STW: %.1f", cnmea.stw);
        
        gauge = gaugeDepth;
        if (cnmea.dbt < 5) gauge = gaugeDepthW;
        if (cnmea.dbt > 10) gauge = gaugeDepthx10;

        depth = cnmea.dbt;
        if (depth > 10.0) depth /=10;

        scale = depth * (maxangle/maxsdepth);
        angle = roundf(scale+minangle);

        // Run needle with smooth acceleration
        if (angle > t_angle) t_angle += 0.8 * (fabsf(angle -t_angle) / 24) ;
        else if (angle < t_angle) t_angle -= 0.8 * (fabsf(angle -t_angle) / 24);

        SDL_RenderCopy(sdlApp->renderer, Background_Tx, NULL, NULL);
    
        SDL_RenderCopyEx(sdlApp->renderer, gauge, NULL, &gaugeR, 0, NULL, SDL_FLIP_NONE);

        if (!(ct - cnmea.dbt_ts > S_TIMEOUT || cnmea.dbt == 0))
            SDL_RenderCopyEx(sdlApp->renderer, gaugeNeedle, NULL, &needleR, t_angle, NULL, SDL_FLIP_NONE);

        get_text_and_rect(sdlApp->renderer, 182, 300, 4, msg_dbt, fontLarge, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 180, 370, 1, msg_mtw, fontSmall, &textField, &textField_rect);   
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 500, 120, 0, msg_hdm, fontCog, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        
        get_text_and_rect(sdlApp->renderer, 500, 170, 0, msg_rmc, fontCog, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        
        get_text_and_rect(sdlApp->renderer, 500, 220, 0, msg_stw, fontCog, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        SDL_RenderCopyEx(sdlApp->renderer, menuBar, NULL, &menuBarR, 0, NULL, SDL_FLIP_NONE);
        addMenuItems(sdlApp->renderer, fontSrc);

        get_text_and_rect(sdlApp->renderer, 650, 10, 0, msg_tod, fontTod, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        if (subTaskbar != NULL) {
            SDL_RenderCopyEx(sdlApp->renderer, subTaskbar, NULL, &subTaskbarR, 0, NULL, SDL_FLIP_NONE);
        }

        SDL_RenderPresent(sdlApp->renderer); 
        
        SDL_Delay(25);
    }

    SDL_DestroyTexture(gaugeDepth);
    SDL_DestroyTexture(gaugeDepthW);
    SDL_DestroyTexture(gaugeDepthx10);
    SDL_DestroyTexture(gaugeNeedle);
    SDL_DestroyTexture(menuBar);
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
    SDL_Rect gaugeR, needleR, menuBarR, subTaskbarR;
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

    SDL_Texture* gaugeSumlog = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "wind.png");
    SDL_Texture* gaugeNeedle = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "needle.png");
    SDL_Texture* menuBar = IMG_LoadTexture(sdlApp->renderer, IMAGE_PATH "menuBar.png");

    SDL_Texture* subTaskbar = NULL;

    sdlApp->curPage = WNDPAGE;

    if (sdlApp->subAppsCmd[sdlApp->curPage][0] != NULL) {
        char icon[PATH_MAX];
        sprintf(icon , "%s/%s.png", IMAGE_PATH, sdlApp->subAppsCmd[sdlApp->curPage][0]);
        subTaskbar = IMG_LoadTexture(sdlApp->renderer, icon);
    }

    SDL_Texture* textField;
    SDL_Rect textField_rect;

    float t_angle = 0;
    float angle = 0;
    const float offset = 131; // For scale

    while (1) {
        char msg_vwrs[40];
        char msg_vwra[40];
        char msg_dbt[40] = { " " };
        char msg_mtw[40] = { " " };
        char msg_stw[40] = { " " };      
        char msg_hdm[40] = { " " };
        char msg_rmc[40] = { " " };
        char msg_tod[40];
        time_t ct;

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
            sprintf(msg_vwra, "%.0f", cnmea.vwra);

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
        if (!(ct - cnmea.rmc_ts > S_TIMEOUT || cnmea.rmc < 1.0))
             sprintf(msg_rmc, "SOG: %.1f", cnmea.rmc);
        
        // VHW - Water speed and Heading
         if (!(ct - cnmea.stw_ts > S_TIMEOUT || cnmea.stw == 0))
            sprintf(msg_stw, "STW: %.1f", cnmea.stw);
        
        angle = cnmea.vwra; // 0-180

        if (cnmea.vwrd == 1) angle = 360 - angle; // Mirror the needle motion

        angle += offset;  
        
        // Run needle with smooth acceleration
        if (angle > t_angle) t_angle += 0.8 * (fabsf(angle -t_angle) / 24) ;
        else if (angle < t_angle) t_angle -= 0.8 * (fabsf(angle -t_angle) / 24);

        SDL_RenderCopy(sdlApp->renderer, Background_Tx, NULL, NULL);
       
        SDL_RenderCopyEx(sdlApp->renderer, gaugeSumlog, NULL, &gaugeR, 0, NULL, SDL_FLIP_NONE);

        if (!(ct - cnmea.vwr_ts > S_TIMEOUT || cnmea.vwra == 0))
            SDL_RenderCopyEx(sdlApp->renderer, gaugeNeedle, NULL, &needleR, t_angle, NULL, SDL_FLIP_NONE);

        get_text_and_rect(sdlApp->renderer, 216, 100, 3, msg_vwra, fontSmall, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 182, 300, 4, msg_vwrs, fontLarge, &textField, &textField_rect);    
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        
        get_text_and_rect(sdlApp->renderer, 500, 120, 0, msg_hdm, fontCog, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);
        
        get_text_and_rect(sdlApp->renderer, 500, 170, 0, msg_stw, fontCog, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 500, 220, 0, msg_rmc, fontCog, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 500, 270, 0, msg_dbt, fontCog, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        get_text_and_rect(sdlApp->renderer, 500, 320, 0, msg_mtw, fontCog, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        SDL_RenderCopyEx(sdlApp->renderer, menuBar, NULL, &menuBarR, 0, NULL, SDL_FLIP_NONE);
        addMenuItems(sdlApp->renderer, fontSrc);

        get_text_and_rect(sdlApp->renderer, 650, 10, 0, msg_tod, fontTod, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        if (subTaskbar != NULL) {
            SDL_RenderCopyEx(sdlApp->renderer, subTaskbar, NULL, &subTaskbarR, 0, NULL, SDL_FLIP_NONE);
        }

        SDL_RenderPresent(sdlApp->renderer); 
        
        SDL_Delay(25);
    }

    SDL_DestroyTexture(gaugeSumlog);
    SDL_DestroyTexture(gaugeNeedle);
    SDL_DestroyTexture(menuBar);
    TTF_CloseFont(fontLarge);
    TTF_CloseFont(fontSmall);
    TTF_CloseFont(fontCog);
    TTF_CloseFont(fontSrc);
    TTF_CloseFont(fontTod);
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
    SDL_Log(msg_cal);

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

        get_text_and_rect(sdlApp->renderer, 10, 250, 1, msg_cal, fontCAL, &textField, &textField_rect);
        SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

        SDL_RenderCopyEx(sdlApp->renderer, menuBar, NULL, &menuBarR, 0, NULL, SDL_FLIP_NONE);
        addMenuItems(sdlApp->renderer, fontSrc);

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

            get_text_and_rect(sdlApp->renderer, 10, 250, 1, msg_cal, fontCAL, &textField, &textField_rect);
            SDL_RenderCopy(sdlApp->renderer, textField, NULL, &textField_rect); SDL_DestroyTexture(textField);

            get_text_and_rect(sdlApp->renderer, 10, 320, 1, doRun.progress, fontPRG, &textField, &textField_rect);
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
    SDL_Thread *threadNmea;
    SDL_Thread *threadI2C;
    SDL_Thread *threadGPS;

    // This is what this application is built for!
    setenv("SDL_VIDEODRIVER", "RPI", 1);

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,  "Couldn't initialize SDL. Video driver %s!", SDL_GetError());
        return SDL_QUIT;
    }

    if (configParams->runNet) {
        threadNmea = SDL_CreateThread(nmeaNetCollector, "nmeaNetCollector", configParams);

        if (NULL == threadNmea) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateThread nmeaNetCollector failed: %s", SDL_GetError());
        } else SDL_DetachThread(threadNmea);
    }

    if (configParams->runi2c) {
        threadI2C = SDL_CreateThread(i2cCollector, "i2cCollector", configParams);

        if (NULL == threadI2C) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateThread threadI2C failed: %s", SDL_GetError());
        } else SDL_DetachThread(threadI2C);
    }

    if (configParams->runGps) {
        threadGPS = SDL_CreateThread(threadSerial, "threadGPS", configParams);

        if (NULL == threadGPS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateThread threadGPS failed: %s", SDL_GetError());
        } else SDL_DetachThread(threadGPS);
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

    if (threadI2C != NULL)
        sdlApp->conn3 = configParams->conn3;

    return 0;
}

// Check if subtask is within PATH.
static int checkSubtask(sdl2_app *sdlApp)
{
    char subtask[PATH_MAX];
    char buff[PATH_MAX];
    int rval;
    FILE *fd;
    sqlite3_stmt *res;
    const char *tail;

    if (sdlApp->subAppsCmd[0][0] == NULL) {
       if (sdlApp->conn3 == NULL) {
            return 0;
        }

        int c = 1;
        if ((rval=sqlite3_prepare_v2(sdlApp->conn3, "select task,args from subtasks", -1, &res, &tail)) == SQLITE_OK)
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
        }
    }

    if (sdlApp->subAppsCmd[sdlApp->curPage][0] == NULL)
        return 0;

    return 1;
}

// Give up all resources in favor of a subtask execution.
static int doSubtask(sdl2_app *sdlApp, configuration *configParams)
{

    int pid, runners[3];
    int status, i=0;
    char *args[20];
    char cmd[1024];

    if (!checkSubtask(sdlApp))
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

    closeSDL2(sdlApp);
       
    pid = fork ();

    if (pid == 0) {
        // Child
        execv ("/bin/bash", args);
    }

    // You've picked my bones clean, speak now ...
    waitpid(pid, &status, 0);
    // ... before I reclaim the meat. (Solonius)

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

    sdlApp.fontPath = DEFAULT_FONT;

    SDL_LogSetOutputFunction((void*)logCallBack, argv[0]);

    configParams.runGps = configParams.runi2c = configParams.runNet = 1;
        
    sdlApp.nextPage = COGPAGE; // Start-page for touch
    step = COGPAGE; // .. mouse

    (void)configureDb(&configParams);   // Fetch configuration

    while ((c = getopt (argc, argv, "sn:p:vgiN")) != -1)
    {
        switch (c)
            {
            case 's':
                useSyslog = 1;
                break;
            case 'n':
                strcpy (configParams.server, argv[optind-1]);   // Override DB
                break;
            case 'p':
                configParams.port = atoi(optarg);               // Override DB 
                break;
            case 'g':   configParams.runGps = 0;                // Diable GPS data collection
                break;
            case 'i':   configParams.runi2c = 0;                // Disable i2c data collection
                break;
            case 'N':   configParams.runNet = 0;                // Disable NMEA net data collection
                break;
            case 'v':
                fprintf(stderr, "revision: %s\n", SWREV);
                exit(EXIT_SUCCESS);
                break;
            default:
                fprintf(stderr, "Usage: %s -s (use syslog) -n (NMEA server) -p (port) -g -i -N -v (version)\n", basename(argv[0]));
                fprintf(stderr, "       Where: -g Disable GPS : -i Disable i2c : -N Disabe NMEA Net\n");
                exit(EXIT_FAILURE);
                break;
            }
    }

    if (useSyslog) {
        setlogmask (LOG_UPTO (LOG_NOTICE));
        openlog (basename(argv[0]), LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);
        syslog (LOG_NOTICE, "Program started by User %d", getuid ());
    }

    if (openSDL2(&configParams, &sdlApp))
        exit(EXIT_FAILURE);

    (void)checkSubtask(&sdlApp);

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

    configParams.runGps = configParams.runi2c = configParams.runNet = 0;

    closeSDL2(&sdlApp);

    SDL_Delay(1600);

    SDL_Log("User terminated");

    exit(EXIT_SUCCESS);
}
