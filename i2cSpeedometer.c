/*
    A simple program that demonstrates how to program a magnetometer
    on the Raspberry Pi and includes tilt compensation.
    http://ozzmaker.com/2014/12/01/compass1


    Copyright (C) 2014  Mark Williams

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.
    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    Library General Public License for more details.
    You should have received a copy of the GNU Library General Public
    License along with this library; if not, write to the Free
    Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
    MA 02111-1307, USA

    2018 Erland Hedman: Rewritten as a library component with code
    added from other BerryIMU examples.
*/
#include <stdint.h>
#include "LSM9DS0.h"
#include "LSM9DS1.h"
#include <linux/i2c-dev.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>
#include <SDL2/SDL.h>   // For the purpose of logging
#include "sdlSpeedometer.h"

#define MAG_LPF_FACTOR  0.4
#define ACC_LPF_FACTOR  0.1

#define G_GAIN 0.070    // [deg/s/LSB]
#define RAD_TO_DEG      57.29578
//#define DT 0.2          // [s/loop] loop period.  0.2  = 200ms
#define AA      0.97    // complementary filter constan
#define RDEV    2       // Return heading results within +/- RDEV range

static int LSM9DS0 = 0;
static int LSM9DS1 = 0;

static int selectDevice(int file, int addr)
{
    if (ioctl(file, I2C_SLAVE, addr) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to select I2C device %d - %s", addr, strerror(errno));
        return -1;
    }
    return 0;
}

static int readBlock(uint8_t command, uint8_t size, uint8_t *data, int file)
{
    int result = i2c_smbus_read_i2c_block_data(file, command, size, data);
    if (result != size)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to read block from I2C %d - %s", command, strerror(errno));
        return -1;
    }
    return 0;
}

static void writeGyrReg(uint8_t reg, uint8_t value, int file)
{
    if (LSM9DS0)
        selectDevice(file,LSM9DS0_GYR_ADDRESS);
    else if (LSM9DS1)
        selectDevice(file,LSM9DS1_GYR_ADDRESS);
  
    int result = i2c_smbus_write_byte_data(file, reg, value);
    if (result == -1){
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,  "Failed to write byte to I2C Gyr.");
    }
}

static void readGYR(int g[], int file)
{
    uint8_t block[6];
    if (LSM9DS0){
        selectDevice(file,LSM9DS0_GYR_ADDRESS);
        readBlock(0x80 |  LSM9DS0_OUT_X_L_M, sizeof(block), block, file);
    }
    else if (LSM9DS1){
        selectDevice(file,LSM9DS1_GYR_ADDRESS);
        readBlock(0x80 |  LSM9DS1_OUT_X_L_M, sizeof(block), block, file);
    }

    // Combine readings for each axis.
    g[0] = (int16_t)(block[0] | block[1] << 8);
    g[1] = (int16_t)(block[2] | block[3] << 8);
    g[2] = (int16_t)(block[4] | block[5] << 8);
}

static int writeAccReg(uint8_t reg, uint8_t value, int file)
{
    if (LSM9DS0)
        selectDevice(file,LSM9DS0_ACC_ADDRESS);
    else if (LSM9DS1)
        selectDevice(file,LSM9DS1_ACC_ADDRESS);

    int result = i2c_smbus_write_byte_data(file, reg, value);
    if (result == -1){
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to write byte to I2C Acc.");
        return result;
    }
    return 0;
}

static int readACC(int  a[], int file)
{
    uint8_t block[6];
    int result = 0;

    if (LSM9DS0){
        selectDevice(file,LSM9DS0_ACC_ADDRESS);
        result = readBlock(0x80 |  LSM9DS0_OUT_X_L_A, sizeof(block), block, file);
    }
    else if (LSM9DS1){
        selectDevice(file,LSM9DS1_ACC_ADDRESS);
        result = readBlock(0x80 |  LSM9DS1_OUT_X_L_XL, sizeof(block), block, file);       
    }

    // Combine readings for each axis.
    a[0] = (int16_t)(block[0] | block[1] << 8);
    a[1] = (int16_t)(block[2] | block[3] << 8);
    a[2] = (int16_t)(block[4] | block[5] << 8);

    return result;
}

static int readMAG(int  m[], int file)
{
    uint8_t block[6];
    int result = 0;

    if (LSM9DS0){
        selectDevice(file,LSM9DS0_MAG_ADDRESS);
        result = readBlock(0x80 |  LSM9DS0_OUT_X_L_M, sizeof(block), block, file);
    }
    else if (LSM9DS1){
        selectDevice(file,LSM9DS1_MAG_ADDRESS);
        result = readBlock(0x80 |  LSM9DS1_OUT_X_L_M, sizeof(block), block, file);    
    }

    // Combine readings for each axis.
    m[0] = (int16_t)(block[0] | block[1] << 8);
    m[1] = (int16_t)(block[2] | block[3] << 8);
    m[2] = (int16_t)(block[4] | block[5] << 8);

    return result;

}

void i2creadMAG(int  m[], int file)
{
    readMAG(m, file);
}

static int writeMagReg(uint8_t reg, uint8_t value, int file)
{
    if (LSM9DS0)
        selectDevice(file,LSM9DS0_MAG_ADDRESS);
    else if (LSM9DS1)
        selectDevice(file,LSM9DS1_MAG_ADDRESS);;

    int result = i2c_smbus_write_byte_data(file, reg, value);
    if (result == -1)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to write byte to I2C Mag %d - %s", reg, strerror(errno));
        return result;
    }
    return 0;
}

static void enableIMU(int file)
{

    if (LSM9DS0){//For BerryIMUv1
        // Enable accelerometer.
        writeAccReg(LSM9DS0_CTRL_REG1_XM, 0b01100111, file); //  z,y,x axis enabled, continuous update,  100Hz data rate
        writeAccReg(LSM9DS0_CTRL_REG2_XM, 0b00100000, file); // +/- 16G full scale

        //Enable the magnetometer
        writeMagReg(LSM9DS0_CTRL_REG5_XM, 0b11110000, file); // Temp enable, M data rate = 50Hz
        writeMagReg(LSM9DS0_CTRL_REG6_XM, 0b01100000, file); // +/-12gauss
        writeMagReg(LSM9DS0_CTRL_REG7_XM, 0b00000000, file); // Continuous-conversion mode

        // Enable Gyro
        writeGyrReg(LSM9DS0_CTRL_REG1_G, 0b00001111, file); // Normal power mode, all axes enabled
        writeGyrReg(LSM9DS0_CTRL_REG4_G, 0b00110000, file); // Continuos update, 2000 dps full scale
    }

    if (LSM9DS1){//For BerryIMUv2
        // Enable the gyroscope
        writeGyrReg(LSM9DS1_CTRL_REG4,0b00111000, file);      // z, y, x axis enabled for gyro
        writeGyrReg(LSM9DS1_CTRL_REG1_G,0b10111000, file);    // Gyro ODR = 476Hz, 2000 dps
        writeGyrReg(LSM9DS1_ORIENT_CFG_G,0b10111000, file);   // Swap orientation 

        // Enable the accelerometer
        writeAccReg(LSM9DS1_CTRL_REG5_XL,0b00111000, file);   // z, y, x axis enabled for accelerometer
        writeAccReg(LSM9DS1_CTRL_REG6_XL,0b00101000, file);   // +/- 16g

        //Enable the magnetometer
        writeMagReg(LSM9DS1_CTRL_REG1_M, 0b10011100, file);   // Temp compensation enabled,Low power mode mode,80Hz ODR
        writeMagReg(LSM9DS1_CTRL_REG2_M, 0b01000000, file);   // +/-12gauss
        writeMagReg(LSM9DS1_CTRL_REG3_M, 0b00000000, file);   // continuos update
        writeMagReg(LSM9DS1_CTRL_REG4_M, 0b00000000, file);   // lower power mode for Z axis
    }

}

int i2cinit(int bus)
{
    char filename[20];
    int file;

    // Open the i2c bus
    sprintf(filename, "/dev/i2c-%d", bus);

    if ((file = open(filename, O_RDWR)) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,  "Unable to open I2C bus!: %s - %s", filename, strerror(errno));
        return -1;
    }

    //Detect if BerryIMUv1 (Which uses a LSM9DS0) is connected
    selectDevice(file,LSM9DS0_ACC_ADDRESS);
    int LSM9DS0_WHO_XM_response = i2c_smbus_read_byte_data(file, LSM9DS0_WHO_AM_I_XM);

    selectDevice(file,LSM9DS0_GYR_ADDRESS);    
    int LSM9DS0_WHO_G_response = i2c_smbus_read_byte_data(file, LSM9DS0_WHO_AM_I_G);

    if (LSM9DS0_WHO_G_response == 0xd4 && LSM9DS0_WHO_XM_response == 0x49){
        SDL_Log("BerryIMUv1/LSM9DS0  DETECTED");
        LSM9DS0 = 1;
    }

    //Detect if BerryIMUv2 (Which uses a LSM9DS1) is connected
    selectDevice(file,LSM9DS1_MAG_ADDRESS);
    int LSM9DS1_WHO_M_response = i2c_smbus_read_byte_data(file, LSM9DS1_WHO_AM_I_M);

    selectDevice(file,LSM9DS1_GYR_ADDRESS);    
    int LSM9DS1_WHO_XG_response = i2c_smbus_read_byte_data(file, LSM9DS1_WHO_AM_I_XG);

    if (LSM9DS1_WHO_XG_response == 0x68 && LSM9DS1_WHO_M_response == 0x3d){
        SDL_Log("BerryIMUv2/LSM9DS1  DETECTED");
        LSM9DS1 = 1;
    }

    enableIMU(file);

    return file;
}

float i2cReadHdm(int file, calibration *calib)
{

    static float accXnorm,accYnorm,pitch,roll,magXcomp,magYcomp;
    static int magRaw[3];
    static int accRaw[3];
    static int oldXMagRawValue;
    static int oldYMagRawValue;
    static int oldZMagRawValue;
    static int oldXAccRawValue;
    static int oldYAccRawValue;
    static int oldZAccRawValue;
    static int sampleCnt;

    static float heading, curHeading;
    int result = 0;

    if (sampleCnt++ < 5) {
        return curHeading;
    }
    sampleCnt = 0;

    result += readMAG(magRaw, file);
    result += readACC(accRaw, file);

    if (result < 0)
        return result;

    //Apply low pass filter to reduce noise
    magRaw[0] =  magRaw[0]  * MAG_LPF_FACTOR + oldXMagRawValue*(1 - MAG_LPF_FACTOR);
    magRaw[1] =  magRaw[1]  * MAG_LPF_FACTOR + oldYMagRawValue*(1 - MAG_LPF_FACTOR);
    magRaw[2] =  magRaw[2]  * MAG_LPF_FACTOR + oldZMagRawValue*(1 - MAG_LPF_FACTOR);
    accRaw[0] =  accRaw[0]  * ACC_LPF_FACTOR + oldXAccRawValue*(1 - ACC_LPF_FACTOR);
    accRaw[1] =  accRaw[1]  * ACC_LPF_FACTOR + oldYAccRawValue*(1 - ACC_LPF_FACTOR);
    accRaw[2] =  accRaw[2]  * ACC_LPF_FACTOR + oldZAccRawValue*(1 - ACC_LPF_FACTOR);

    oldXMagRawValue = magRaw[0];
    oldYMagRawValue = magRaw[1];
    oldZMagRawValue = magRaw[2];
    oldXAccRawValue = accRaw[0];
    oldYAccRawValue = accRaw[1];
    oldZAccRawValue = accRaw[2];

     //Apply hard iron calibration
     magRaw[0] -= (calib->magXmin + calib->magXmax) /2 ;
     magRaw[1] -= (calib->magYmin + calib->magYmax) /2 ;
     magRaw[2] -= (calib->magZmin + calib->magZmax) /2 ;

#if 0
     //Apply soft iron calibration
    static float scaledMag[3];
    scaledMag[0]  = (float)(magRaw[0] - calib->magXmin) / (calib->magXmax - calib->magXmin) * 2 - 1;
    scaledMag[1]  = (float)(magRaw[1] - calib->magYmin) / (calib->magYmax - calib->magYmin) * 2 - 1;
    scaledMag[2]  = (float)(magRaw[2] - calib->magZmin) / (calib->magZmax - calib->magZmin) * 2 - 1;
#endif

    //If your IMU is upside down, comment out the two lines below which we correct the tilt calculation
//    accRaw[0] = -accRaw[0];
//    accRaw[1] = -accRaw[1];

    //Normalize accelerometer raw values.
    accXnorm = accRaw[0]/sqrt(accRaw[0] * accRaw[0] + accRaw[1] * accRaw[1] + accRaw[2] * accRaw[2]);
    accYnorm = accRaw[1]/sqrt(accRaw[0] * accRaw[0] + accRaw[1] * accRaw[1] + accRaw[2] * accRaw[2]);

    //Calculate pitch and roll
    pitch = asin(accXnorm);
    roll = -asin(accYnorm/cos(pitch));

    //Calculate the new tilt compensated values
    magXcomp = magRaw[0]*cos(pitch)+magRaw[2]*sin(pitch);
    if(LSM9DS0)
        magYcomp = magRaw[0]*sin(roll)*sin(pitch)+magRaw[1]*cos(roll)-magRaw[2]*sin(roll)*cos(pitch); // LSM9DS0
    else
       magYcomp = magRaw[0]*sin(roll)*sin(pitch)+magRaw[1]*cos(roll)+magRaw[2]*sin(roll)*cos(pitch); // LSM9DS1

    //Calculate heading with declination
    heading = (180*atan2(magYcomp,magXcomp)/M_PI) + calib->declval + calib->coffset;

    //Convert heading to 0 - 360
    if(heading < 0)
        heading += 360;

    if (!curHeading) curHeading = heading;

    if (!(heading > curHeading+RDEV || heading < curHeading-RDEV)) {
        heading = curHeading;
    } else {
        curHeading = heading;
    }

    return roundf(heading);

}

float i2cReadRoll(int file, int dt, calibration *calib)
{
    //Each (dt) loop should be at least 20ms.

    static float gyroXangle;
    static float gyroYangle;
    static float gyroZangle;
    static float AccYangle;
    static float AccXangle;
    static float CFangleX;
    static float CFangleY;

    static float rate_gyr_y;    // [deg/s]
    static float rate_gyr_x;    // [deg/s]
    static float rate_gyr_z;    // [deg/s]

    int  acc_raw[3];
    int  gyr_raw[3];

    //read ACC and GYR data
    readACC(acc_raw, file);
    readGYR(gyr_raw, file);

    //Convert Gyro raw to degrees per second
    rate_gyr_x = (float) gyr_raw[0] * G_GAIN;
    rate_gyr_y = (float) gyr_raw[1]  * G_GAIN;
    rate_gyr_z = (float) gyr_raw[2]  * G_GAIN;

    //Calculate the angles from the gyro
    gyroXangle+=rate_gyr_x*(float)dt/1000;
    gyroYangle+=rate_gyr_y*(float)dt/1000;
    gyroZangle+=rate_gyr_z*(float)dt/1000;;

    //Convert Accelerometer values to degrees
    AccXangle = (float) (atan2(acc_raw[1],acc_raw[2])+M_PI)*RAD_TO_DEG;
    AccYangle = (float) (atan2(acc_raw[2],acc_raw[0])+M_PI)*RAD_TO_DEG;

    //Change the rotation value of the accelerometer to -/+ 180 and move the Y axis '0' point to up.
    //Two different pieces of code are used depending on how your IMU is mounted.
    //If IMU is upside down
    /*
    if (AccXangle >180)
        AccXangle -= (float)360.0;

    AccYangle-=90;
    if (AccYangle >180)
    A    ccYangle -= (float)360.0;
    */

    //If IMU is up the correct way, use these lines
    AccXangle -= (float)180.0;
    if (AccYangle > 90)
        AccYangle -= (float)270;
    else
        AccYangle += (float)90;

    //Complementary filter used to combine the accelerometer and gyro values.
    CFangleX=AA*(CFangleX+rate_gyr_x*(float)dt/1000) +(1 - AA) * AccXangle;
    CFangleY=AA*(CFangleY+rate_gyr_y*(float)dt/1000) +(1 - AA) * AccYangle;

    //printf ("   GyroX  %7.3f \t AccXangle \e[m %7.3f \t \033[22;31mCFangleX %7.3f\033[0m\t GyroY  %7.3f \t AccYangle %7.3f \t \033[22;36mCFangleY %7.3f\t\033[0m\n",gyroXangle,AccXangle,CFangleX,gyroYangle,AccYangle,CFangleY);

    return roundf(AccXangle)+calib->roffset;
}

