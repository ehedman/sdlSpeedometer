#ifndef SPEEDOMETER_H
#define SPEEDOMETER_H

#include <sqlite3.h>
#include <rfb/rfb.h>
#ifdef HAS_SMBUS_H
#include <i2c/smbus.h>
#endif

#ifdef PLOTSDL
#include <plotsdl/plot.h>
#include <plotsdl/llist.h>
#endif

// See: BerryIMU/compass_tutorial03_calibration
// Defaults if db fails
#define dmagXmax 1707
#define dmagYmax 717
#define dmagZmax -618
#define dmagXmin -681
#define dmagYmin -1414
#define dmagZmin -2719
#define ddeclval 0.11

typedef struct {
    int magXmax;
    int magYmax;
    int magZmax;
    int magXmin;
    int magYmin;
    int magZmin;
    float declval;
} calibration;


typedef struct {
    int run;
    float latitude;
    float longitude;
    float declination;
    char progress[200];
    int i2cFile;
} calRunner;

typedef struct {
    int runGps;
    int runi2c;
    int runNet;
    int runVnc;
    int runMon;
    int numThreads;
    short port;
    char server[100];
    int useWm;
    int useKms;
    rfbScreenInfoPtr vncServer;
    SDL_Surface* vncPixelBuffer;
    int vncClients;
    int vncPort;
    char tty[40];
    int baud;
    int i2cFile;
    sqlite3 *conn;
    int netStat;
    int onHold;
    int subTaskPID;
} configuration;

enum sdlPages {
    COGPAGE = 1,
    SOGPAGE,
    DPTPAGE,
    WNDPAGE,
    GPSPAGE,
    CALPAGE,
    PWRPAGE,
    TSKPAGE  
};

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    char *fontPath;
    char *subAppsCmd[TSKPAGE][TSKPAGE];
    int nextPage;
    int curPage;
    configuration *conf;
} sdl2_app;

extern int i2cinit(int bus);
extern float i2cReadHdm(int file, calibration *calib);
extern float i2cReadRoll(int file, int dt);
extern void i2creadMAG(int  m[], int file);

typedef struct {
    // Dynamic data from NMEA server
    float   rmc;        // RMC (Speed Over Ground) in knots
    char    time[20];   // UTC Time
    char    date[20];   // Date
    int     rmc_tm_set; // time isset ?
    time_t  rmc_ts;     // RMC Timestamp
    time_t  rmc_gps_ts; // Got RMC from GPS
    float   roll;       // Vessel roll (non NMEA)
    time_t  roll_i2cts; // Roll timestamp
    float   stw;        // Speed of vessel relative to the water (Knots)
    time_t  stw_ts;     // STW Timestamp
    float   dbt;        // Depth in meters
    time_t  dbt_ts;     // DBT Timestamp
    float   mtw;        // Water temperature
    time_t  mtw_ts;     // Water temperature Timestamp
    float   hdm;        // Heading
    time_t  hdm_ts;     // HDM Timestamp (nmea)
    time_t  hdm_i2cts;  // HDM Timestamp (i2c)
    float   vwra;       // Relative wind angle (0-180)
    float   vwta;       // True wind angle
    time_t  vwr_ts;     // Wind data Timestamp
    time_t  vwt_ts;     // True wind data Timestamp
    int     vwrd;       // Right or Left Heading
    float   vwrs;       // Relative wind speed knots
    float   vwts;       // True wind speed
    char    gll[40];    // Position Latitude
    time_t  gll_ts;     // Position Timestamp
    char    glo[40];    // Position Longitude
    char    glns[2];    // North (N) or South (S)
    char    glne[2];    // East (E) or West (W)
    time_t  net_ts;     // Data valid from network
    // Sensors $P type messages
    float   volt;       // Sensor Volt
    int     volt_bank;  // Batery sensor bank #
    time_t  volt_ts;    // Volt Timestamp
    float   curr;       // Sensor Current
    int     curr_bank;  // Current sensor bank #
    time_t  curr_ts;    // Current Timestamp
    float   temp;       // Sensor Temp
    int     temp_loc;   // Sensor location i.e, indoor ...
    time_t  temp_ts;    // Temp Timestamp
    float   kWhp;       // Kilowatt hour - charged
    float   kWhn;       // Kilowatt hour - consumed
    time_t  startTime;  // Server's starttime
    // Misc
    float   declination;  // from NOAA
} collected_nmea;


#endif /* SPEEDOMETER_H */
