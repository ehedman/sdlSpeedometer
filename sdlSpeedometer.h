#ifndef SPEEDOMETER_H
#define SPEEDOMETER_H

#include <sqlite3.h>
#include <rfb/rfb.h>
#ifdef HAS_SMBUS_H
#include <i2c/smbus.h>
#endif
#include <alsa/asoundlib.h>

// Dendent on project  https://github.com/ehedman/flowSensor
//#define DIGIFLOW


// Volume slider
#define SWINDOW_WIDTH 20
#define SWINDOW_HEIGHT 300
#define SLIDER_WIDTH 20
#define SLIDER_HEIGHT 300
#define RIGHT_MARGIN 20

// Power plot
#define PWINDOW_W 700
#define PWINDOW_H 250
#define PHISTORY 250
#define SCALE_WINDOW 50

// Depth plot
#define DWINDOW_W 800
#define DWINDOW_H 480
#define DHISTORY 300

// Video
#define VIDEO_BUFFERS 4
#define AUDIO_BUFFER_FRAMES 1024
#define FRAME_SIZE 1024

// See: BerryIMU/compass_tutorial03_calibration
// Defaults if db fails
#define dmagXmax 2029
#define dmagYmax 1297
#define dmagZmax 579
#define dmagXmin -324
#define dmagYmin -1066
#define dmagZmin -1338
#define ddeclval 0.13

#define RFB_TOUCH 1

#define NMBUFF 2048

typedef struct {
    int magXmax;
    int magYmax;
    int magZmax;
    int magXmin;
    int magYmin;
    int magZmin;
    float declval;
    int coffset;
    float roffset;
    float depthw;
} calibration;

typedef struct {
    volatile int runSer;
    volatile int runi2c;
    volatile int runNet;
    volatile int runVnc;
    volatile int runWrn;
    volatile int runTyd;
	pid_t wayvncPid;
    int numThreads;
    short port;
    char server[100];
    int useWm;
    rfbScreenInfoPtr vncServer;
    SDL_Surface* vncPixelBuffer;
    float scale;
    char ssize[50];
    int window_w;
    int window_h;
    int vncClients;
    int vncPort;
    int doFokusCheck;
    char tty[40];
    int baud;
    int i2cFile;
    sqlite3 *conn;
    int netStat;
    int muted;
    int subTaskPID;
    int cursor;
    int ttydPID;
    snd_mixer_t *mixer;
    snd_mixer_elem_t *elem;
    long snd_minv, snd_maxv;
    int snd_useMixer;
    int snd_showMixer;
    char snd_card[60];
    char snd_card_name[256];
    char cam_url[200];
    char vid_device[60];
    SDL_mutex* nm_mutex;
} configuration;

enum sdlPages {
    COGPAGE = 1,
    SOGPAGE,
    DPTPAGE,
    WNDPAGE,
    GPSPAGE,
    PWRPAGE,
    VIDPAGE,
    CALPAGE,
    TSKPAGE,
    WTRPAGE
};

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    char *fontPath;
    char *subAppsCmd[VIDPAGE][VIDPAGE];
    char *subAppsIco[VIDPAGE][VIDPAGE];
    int nextPage;
    int curPage;
    int plotMode;
    SDL_Texture* textFieldArr[60];
    SDL_Surface* formattedSurf;
    char *rfbPauseBuffer;
    SDL_mutex* vnc_mutex;
    int textFieldArrIndx;
    Uint32 last_reset_time;
    configuration *conf;
} sdl2_app;


typedef struct {
    int run;
    float latitude;
    float longitude;
    float declination;
    char progress[200];
    int i2cFile;
} calRunner;

typedef struct {
    volatile int run;
    int mute;
    int astat;
    char snd_card[60];
    char snd_card_dev[60];
} audRunner;


typedef struct {
    float depthw;
    float draftw;
    float lowvoltw;
    float highcurrw;
} warnings;

extern int i2cinit(int bus);
extern float i2cReadHdm(int file, calibration *calib);
extern float i2cReadRoll(int file, int dt, calibration *calib);
extern void i2creadMAG(int  m[], int file);

typedef struct {
    // Dynamic data from NMEA server
    float   rmc;        // RMC (Speed Over Ground) in knots
    char    time[20];   // UTC Time
    char    date[20];   // Date
    int     rmc_tm_set; // time isset ?
    time_t  rmc_ts;     // RMC Timestamp
    time_t  rmc_nme_ts; // Got RMC
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
    float   rsa;        // Rudder angle
    time_t  rsa_ts;     // Rudder angle Timestamp
    time_t  hdm_i2cts;  // HDM Timestamp (i2c)
    time_t  xdr_ts;     // Wessel roll Timestamp
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
    char    volt_bank[20];  // Batery sensor bank #
    time_t  volt_ts;    // Volt Timestamp
    float   curr;       // Sensor Current
    char    curr_bank[20];  // Current sensor bank #
    time_t  curr_ts;    // Current Timestamp
    float   temp;       // Sensor Temp
    char    temp_loc[20];   // Sensor location i.e, indoor ...
    time_t  temp_ts;    // Temp Timestamp
    float   kWhp;       // Kilowatt hour - charged
    float   kWhn;       // Kilowatt hour - consumed
    time_t  startTime;  // Server's starttime
    // Misc
    float   declination;  // from NOAA
#ifdef DIGIFLOW
    time_t  fdate;      // Filter date
    float   tvol;       // Total consumed volume
    float   gvol;       // Grand total consumed volume
    float   tank;       // Tank Volume
    int     tds;        // TDS value
    float   ttemp;      // Water temp
#endif
} collected_nmea;


#endif /* SPEEDOMETER_H */
