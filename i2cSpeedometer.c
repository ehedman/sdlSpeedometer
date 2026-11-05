/*
 * A program that demonstrates how to program a magnetometer
 * on the Raspberry Pi and includes tilt compensation.
 * http://ozzmaker.com/2014/12/01/compass1
 *
 * Copyright (C) 2014  Mark Williams
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Library General Public License for more details.
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA
 *
 * 2018-2026 Erland Hedman: Rewritten as a library component with code
 * added from other BerryIMU examples.
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
#include <SDL2/SDL.h>
#include "sdlSpeedometer.h"

#ifndef M_PIf
#define M_PIf 3.14159265358979323846f
#endif

#define MAG_LPF_FACTOR  0.4
#define ACC_LPF_FACTOR  0.1

#define RAD_TO_DEG  57.29578
#define AA          0.97    // complementary filter constan

static int LSM9DS0 = 0;
static int LSM9DS1 = 0;

// Return values to main app
mCompass Comppass;

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

#if 0   // TBU
static void readGYR(int g[], int file)

    /* The gyroscope primarily provides faster response to movement and better
     * short-term stability. In the context of a Raspberry Pi-based compass 
     * using the LSM9DS1, this makes for an interesting distinction—especially
     * if you want the compass to perform well while the boat is rolling and pitching.
     * The accelerometer and the gyroscope measure two different things:
     * Sensor	        What it measures	Strength
     * Accelerometer	Acceleration,       including components of gravity	Can estimate pitch and roll during calm motion
     * Gyroscope	    Angular velocity    in degrees per second	Reacts quickly to rotation
     * Magnetometer	    The Earth’s magnetic field	Provides the basis for determining compass heading
     */
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
#endif

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

    if (LSM9DS0 + LSM9DS1 == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,  "Unable to find a BerryIMU (v1 or v2) on the i2c bus!");
        return -1;
    }

    enableIMU(file);
    
    return file;
}

// The lower the ALPHA, the more stable (but slower) the meter becomes.
// 0.05 to 0.15 is usually perfect for marine instruments.
#define RALFA 0.05f

// Threshold for when we consider the boat to be perfectly level (in degrees)
#define RDEAD_ZONE 0.9f

static float filterRoll(float rawData) {
    static float filtered_roll = 0.0f;

    // Low-pass filter: attenuates the rapid flicker (-1 to +1)
    filtered_roll = (RALFA * rawData) + ((1.0f - RALFA) * filtered_roll);

    // Zeroing: if the damped value is close to zero, force it to 0
    if (fabsf(filtered_roll) < RDEAD_ZONE) {
        return 0.0f;
    }

    return filtered_roll;
}

#define PALFA 0.05f
#define PDEAD_ZONE 0.9f

static float filterPitch(float rawData) {
    static float filtered_pitch = 0.0f;

    // Low-pass filter: attenuates the rapid flicker (-1 to +1)
    filtered_pitch = (PALFA * rawData) + ((1.0f - PALFA) * filtered_pitch);

    // Zeroing: if the damped value is close to zero, force it to 0
    if (fabsf(filtered_pitch) < PDEAD_ZONE) {
        return 0.0f;
    }

    return filtered_pitch;
}

#define HALFA 0.08f
#define HDEAD_ZONE 1.0f

static float filterHeading(float rawData)
{
    static float filtered_heading = 0.0f;
    static int initialized = 0;

    if (!initialized) {
        filtered_heading = rawData;
        initialized = 1;
        return filtered_heading;
    }

    // Shortest angular difference, range -180 to +180
    float diff = rawData - filtered_heading;

    while (diff > 180.0f)  diff -= 360.0f;
    while (diff < -180.0f) diff += 360.0f;

    // Ignore small changes around the current heading
    if (fabsf(diff) < HDEAD_ZONE)
        return filtered_heading;

    // Low-pass filter using the shortest angular direction
    filtered_heading += HALFA * diff;

    // Normalize to 0..360 degrees
    if (filtered_heading >= 360.0f)
        filtered_heading -= 360.0f;
    else if (filtered_heading < 0.0f)
        filtered_heading += 360.0f;

    return filtered_heading;
}

float i2cReadHdm(int file, calibration *calib)
{
    static float magXcomp, magYcomp, accYnorm;
    static int magRaw[3];
    static int accRaw[3];
    static int oldXMagRawValue;
    static int oldYMagRawValue;
    static int oldZMagRawValue;
    static int oldXAccRawValue;
    static int oldYAccRawValue;
    static int oldZAccRawValue;
    static float heading;
    static float roll, pitch;
    static float scaledMag[3];

    int result = 0;

    if (LSM9DS0 + LSM9DS1 == 0)
        return 0.0f;

    result += readMAG(magRaw, file);
    result += readACC(accRaw, file);

    if (result < 0)
        return result;

    // Low-pass filter
    magRaw[0] = magRaw[0] * MAG_LPF_FACTOR
              + oldXMagRawValue * (1 - MAG_LPF_FACTOR);
    magRaw[1] = magRaw[1] * MAG_LPF_FACTOR
              + oldYMagRawValue * (1 - MAG_LPF_FACTOR);
    magRaw[2] = magRaw[2] * MAG_LPF_FACTOR
              + oldZMagRawValue * (1 - MAG_LPF_FACTOR);

    accRaw[0] = accRaw[0] * ACC_LPF_FACTOR
              + oldXAccRawValue * (1 - ACC_LPF_FACTOR);
    accRaw[1] = accRaw[1] * ACC_LPF_FACTOR
              + oldYAccRawValue * (1 - ACC_LPF_FACTOR);
    accRaw[2] = accRaw[2] * ACC_LPF_FACTOR
              + oldZAccRawValue * (1 - ACC_LPF_FACTOR);

    oldXMagRawValue = magRaw[0];
    oldYMagRawValue = magRaw[1];
    oldZMagRawValue = magRaw[2];

    oldXAccRawValue = accRaw[0];
    oldYAccRawValue = accRaw[1];
    oldZAccRawValue = accRaw[2];

    // Hard-iron compensation
    magRaw[0] -= (calib->magXmin + calib->magXmax) / 2;
    magRaw[1] -= (calib->magYmin + calib->magYmax) / 2;
    magRaw[2] -= (calib->magZmin + calib->magZmax) / 2;


    if (calib->softi)
    {
        // Soft-iron compensation: approximate per-axis scaling
        float halfX = (calib->magXmax - calib->magXmin) / 2.0f;
        float halfY = (calib->magYmax - calib->magYmin) / 2.0f;
        float halfZ = (calib->magZmax - calib->magZmin) / 2.0f;
        float avgHalf = (halfX + halfY + halfZ) / 3.0f;

        scaledMag[0] = magRaw[0] * avgHalf / halfX;
        scaledMag[1] = magRaw[1] * avgHalf / halfY;
        scaledMag[2] = magRaw[2] * avgHalf / halfZ;
    }

    // Orientation uppside down ?
    if (calib->pitcho == 0) {
        accRaw[0] = -accRaw[0];
        accRaw[1] = -accRaw[1];
    }

    accYnorm = accRaw[1] /
        sqrt(accRaw[0] * accRaw[0] +
             accRaw[1] * accRaw[1] +
             accRaw[2] * accRaw[2]);

    pitch = atan2f((float)accRaw[0],
        sqrtf((float)accRaw[1] * accRaw[1] +
              (float)accRaw[2] * accRaw[2]));

    roll = -asinf(accYnorm / cosf(pitch));

    if (calib->rollo == 0)
        roll *= -1.0f;

    if (calib->softi) { // LSM9DS0
        // Tilt-compensated magnetometer
        magXcomp = scaledMag[0] * cosf(pitch)
                 + scaledMag[2] * sinf(pitch);

        if (LSM9DS0)
            magYcomp = scaledMag[0] * sinf(roll) * sinf(pitch)
                     + scaledMag[1] * cosf(roll)
                     - scaledMag[2] * sinf(roll) * cosf(pitch);
        else  // LSM9DS1
            magYcomp = scaledMag[0] * sinf(roll) * sinf(pitch)
                     + scaledMag[1] * cosf(roll)
                     + scaledMag[2] * sinf(roll) * cosf(pitch);
    } else {
        // Calculate the new tilt compensated values
	    magXcomp = magRaw[0] * cosf(pitch)
            +magRaw[2] * sinf(pitch);

	    if(LSM9DS0)
		    magYcomp = magRaw[0] *sinf(roll) *sinf(pitch)
                +magRaw[1] *cosf(roll)
                -magRaw[2]*sinf(roll)*cosf(pitch);
	    else // LSM9DS1
		    magYcomp = magRaw[0] * sinf(roll)*sinf(pitch)
                +magRaw[1] * cosf(roll) 
                +magRaw[2]*sinf(roll)*cosf(pitch); 
    }

    heading = 180.0f * atan2f(magYcomp, magXcomp) / M_PIf;

    if (heading < 0.0f)
        heading += 360.0f;

    // Return data through a stability filter
    heading = filterHeading(heading);

    Comppass.pitch = (filterPitch(pitch * RAD_TO_DEG) + calib->poffset) * -1.0f;    // Invert to match GUI perspective
    Comppass.roll =  (filterRoll(roll   * RAD_TO_DEG) + calib->roffset) * -1.0f;
    Comppass.heading = heading;

    return roundf(heading);
}
