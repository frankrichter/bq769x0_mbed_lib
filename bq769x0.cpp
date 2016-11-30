/*
    bq769x0.cpp - Battery management system based on bq769x0 for ARM mbed
    Copyright (C) 2015-2016  Martin Jäger (http://libre.solar)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this program. If not, see
    <http://www.gnu.org/licenses/>.
*/

/*
  TODO:
  - CRC check for cell voltage readings
  - Enable values like 1.5 mOhm for shunt resistor
  - Decrease current limit if only charge OR discharge FET is on (current goes through body diode of other FET with high voltage drop) or enable ideal diodes
  - Rework error handling (checkStatus method)

*/

#include <math.h>     // log for thermistor calculation

#include "bq769x0.h"
#include "registers.h"
#include "mbed.h"

#if BQ769X0_DEBUG

const char *byte2char(int x)
{
    static char b[9];
    b[0] = '\0';

    int z;
    for (z = 128; z > 0; z >>= 1)
    {
        strcat(b, ((x & z) == z) ? "1" : "0");
    }

    return b;
}

#endif

uint8_t _crc8_ccitt_update (uint8_t inCrc, uint8_t inData)
{
    uint8_t   i;
    uint8_t   data;

    data = inCrc ^ inData;

    for ( i = 0; i < 8; i++ )
    {
        if (( data & 0x80 ) != 0 )
        {
            data <<= 1;
            data ^= 0x07;
        }
        else
        {
            data <<= 1;
        }
    }
    return data;
}

//, Serial& serial
//----------------------------------------------------------------------------

bq769x0::bq769x0(I2C& bqI2C, PinName alertPin, int bqType, int bqI2CAddress, bool crc):
    _i2c(bqI2C), _alertInterrupt(alertPin)
{
    _timer.start();
    _alertInterrupt.rise(this, &bq769x0::setAlertInterruptFlag);

    I2CAddress = bqI2CAddress;

    type = bqType;
    crcEnabled = crc;

    // set some safe default values
    autoBalancingEnabled = false;
    balancingMinIdleTime_s = 1800;    // default: 30 minutes
    idleCurrentThreshold = 30; // mA

    thermistorBetaValue = 3435;  // typical value for Semitec 103AT-5 thermistor


    if (type == bq76920) {
        numberOfCells = 5;
    } else if (type == bq76930) {
        numberOfCells = 10;
    } else {
        numberOfCells = 15;
    }

    // initialize variables
    for (int i = 0; i < numberOfCells - 1; i++) {
        cellVoltages[i] = 0;
    }

    // test communication
    writeRegister(CC_CFG, 0x19);       // should be set to 0x19 according to datasheet
    if (readRegister(CC_CFG) == 0x19)
    {
        // initial settings for bq769x0
        writeRegister(SYS_CTRL1, 0b00011000);  // switch external thermistor and ADC on
        writeRegister(SYS_CTRL2, 0b01000000);  // switch CC_EN on

        // attach ALERT interrupt to this instance
        //instancePointer = this;
        //attachInterrupt(digitalPinToInterrupt(alertPin), bq769x0::alertISR, RISING);

        // get ADC offset and gain
        adcOffset = (signed int) readRegister(ADCOFFSET);  // convert from 2's complement
        adcGain = 365 + (((readRegister(ADCGAIN1) & 0b00001100) << 1) |
            ((readRegister(ADCGAIN2) & 0b11100000) >> 5)); // uV/LSB

    }
    else {
        // TODO: do something else... e.g. set error flag
#if BQ769X0_DEBUG
        printf("BMS communication error\n");
#endif
    }
}


//----------------------------------------------------------------------------
// Boot IC by pulling the boot pin TS1 high for some ms

void bq769x0::boot(PinName bootPin)
{
    DigitalInOut boot(bootPin);

    boot = 1;
    wait_ms(5);   // wait 5 ms for device to receive boot signal (datasheet: max. 2 ms)
    boot.input();         // don't disturb temperature measurement
    wait_ms(10);  // wait for device to boot up completely (datasheet: max. 10 ms)
}


//----------------------------------------------------------------------------
// Fast function to check whether BMS has an error
// (returns 0 if everything is OK)

int bq769x0::checkStatus()
{
    //  printf("errorStatus: ");
    //  printf(errorStatus);
    if (alertInterruptFlag == false && errorStatus == 0) {
        return 0;
    } else {

        regSYS_STAT_t sys_stat;
        sys_stat.regByte = readRegister(SYS_STAT);

        // TODO: folgender Abschnitt eigentlich nicht nötig... CC_READY wird über update regelmäßig aufgerufen,
        //       tatsächliche Fehler werden später nochmal direkt abgefragt.

        // first check, if only a new CC reading is available
        if (sys_stat.bits.CC_READY == 1) {
            //printf("Interrupt: CC ready");
            updateCurrent();  // automatically clears CC ready flag
        }

        // --- end TODO

        // Serious error occured
        if (sys_stat.regByte & 0b00111111)
        {
            if (alertInterruptFlag == true) {
                secSinceErrorCounter = 0;
            }
            errorStatus = sys_stat.regByte;

            unsigned int secSinceInterrupt = (_timer.read_ms() - interruptTimestamp) / 1000;

            // check for overrun of _timer.read_ms() or very slow running program
            if (abs((long)(secSinceInterrupt - secSinceErrorCounter)) > 2) {
                secSinceErrorCounter = secSinceInterrupt;
            }

            // called only once per second
            if (secSinceInterrupt >= secSinceErrorCounter)
            {
                if (sys_stat.regByte & 0b00100000) { // XR error
                    // datasheet recommendation: try to clear after waiting a few seconds
                    if (secSinceErrorCounter % 3 == 0) {
                        #if BQ769X0_DEBUG
                        printf("Attempting to clear XR error");
                        #endif
                        writeRegister(SYS_STAT, 0b00100000);
                    }
                }
                if (sys_stat.regByte & 0b00010000) { // Alert error
                    if (secSinceErrorCounter % 10 == 0) {
                        #if BQ769X0_DEBUG
                        printf("Attempting to clear Alert error");
                        #endif
                        writeRegister(SYS_STAT, 0b00010000);
                    }
                }
                if (sys_stat.regByte & 0b00001000) { // UV error
                    updateVoltages();
                    if (cellVoltages[idCellMinVoltage] > minCellVoltage) {
                        #if BQ769X0_DEBUG
                        printf("Attempting to clear UV error");
                        #endif
                        writeRegister(SYS_STAT, 0b00001000);
                    }
                }
                if (sys_stat.regByte & 0b00000100) { // OV error
                    updateVoltages();
                    if (cellVoltages[idCellMaxVoltage] < maxCellVoltage) {
                        #if BQ769X0_DEBUG
                        printf("Attempting to clear OV error");
                        #endif
                        writeRegister(SYS_STAT, 0b00000100);
                    }
                }
                if (sys_stat.regByte & 0b00000010) { // SCD
                    if (secSinceErrorCounter % 60 == 0) {
                        #if BQ769X0_DEBUG
                        printf("Attempting to clear SCD error");
                        #endif
                        writeRegister(SYS_STAT, 0b00000010);
                    }
                }
                if (sys_stat.regByte & 0b00000001) { // OCD
                    if (secSinceErrorCounter % 60 == 0) {
                        #if BQ769X0_DEBUG
                        printf("Attempting to clear OCD error");
                        #endif
                        writeRegister(SYS_STAT, 0b00000001);
                    }
                }
                secSinceErrorCounter++;
            }
        }
        else {
            errorStatus = 0;
        }

        return errorStatus;
    }
}

//----------------------------------------------------------------------------
// should be called at least once every 250 ms to get correct coulomb counting

void bq769x0::update()
{
    updateCurrent();  // will only read new current value if alert was triggered
    updateVoltages();
    updateTemperatures();
    updateBalancingSwitches();
}

//----------------------------------------------------------------------------
// puts BMS IC into SHIP mode (i.e. switched off)

void bq769x0::shutdown()
{
    writeRegister(SYS_CTRL1, 0x0);
    writeRegister(SYS_CTRL1, 0x1);
    writeRegister(SYS_CTRL1, 0x2);
}

//----------------------------------------------------------------------------

bool bq769x0::enableCharging()
{
    #if BQ769X0_DEBUG
    printf("checkStatus() = %d\n", checkStatus());
    printf("Umax = %d\n", cellVoltages[idCellMaxVoltage]);
    printf("temperatures[0] = %d\n", temperatures[0]);
    #endif

    if (checkStatus() == 0 &&
        cellVoltages[idCellMaxVoltage] < maxCellVoltage &&
        temperatures[0] < maxCellTempCharge &&
        temperatures[0] > minCellTempCharge)
    {
        int sys_ctrl2;
        sys_ctrl2 = readRegister(SYS_CTRL2);
        writeRegister(SYS_CTRL2, sys_ctrl2 | 0b00000001);  // switch CHG on
        #if BQ769X0_DEBUG
        printf("Enabling CHG FET\n");
        #endif
        return true;
    }
    else {
        return false;
    }
}

//----------------------------------------------------------------------------

bool bq769x0::enableDischarging()
{
    #if BQ769X0_DEBUG
    printf("checkStatus() = %d\n", checkStatus());
    printf("Umin = %d\n", cellVoltages[idCellMinVoltage]);
    printf("temperatures[0] = %d\n", temperatures[0]);
    #endif

    if (checkStatus() == 0 &&
        cellVoltages[idCellMinVoltage] > minCellVoltage &&
        temperatures[0] < maxCellTempDischarge &&
        temperatures[0] > minCellTempDischarge)
    {
        int sys_ctrl2;
        sys_ctrl2 = readRegister(SYS_CTRL2);
        writeRegister(SYS_CTRL2, sys_ctrl2 | 0b00000010);  // switch DSG on
        return true;
    }
    else {
        return false;
    }
}

//----------------------------------------------------------------------------

void bq769x0::enableAutoBalancing(void)
{
    autoBalancingEnabled = true;
}


//----------------------------------------------------------------------------

void bq769x0::setBalancingThresholds(int idleTime_min, int absVoltage_mV, int voltageDifference_mV)
{
    balancingMinIdleTime_s = idleTime_min * 60;
    balancingMinCellVoltage_mV = absVoltage_mV;
    balancingMaxVoltageDifference_mV = voltageDifference_mV;
}

//----------------------------------------------------------------------------
// sets balancing registers if balancing is allowed
// (sufficient idle time + voltage)

void bq769x0::updateBalancingSwitches(void)
{
    long idleSeconds = (_timer.read_ms() - idleTimestamp) / 1000;
    int numberOfSections = numberOfCells/5;

    // check for _timer.read_ms() overflow
    if (idleSeconds < 0) {
        idleTimestamp = 0;
        idleSeconds = _timer.read_ms() / 1000;
    }

    // check if balancing allowed
    if (checkStatus() == 0 &&
        idleSeconds >= balancingMinIdleTime_s &&
        cellVoltages[idCellMaxVoltage] > balancingMinCellVoltage_mV &&
        (cellVoltages[idCellMaxVoltage] - cellVoltages[idCellMinVoltage]) > balancingMaxVoltageDifference_mV)
    {
        //printf("Balancing enabled!");
        balancingStatus = 0;  // current status will be set in following loop

        //regCELLBAL_t cellbal;
        int balancingFlags;
        int balancingFlagsTarget;

        for (int section = 0; section < numberOfSections; section++)
        {
            balancingFlags = 0;
            for (int i = 0; i < 5; i++)
            {
                if ((cellVoltages[section*5 + i] - cellVoltages[idCellMinVoltage]) > balancingMaxVoltageDifference_mV) {

                    // try to enable balancing of current cell
                    balancingFlagsTarget = balancingFlags | (1 << i);

                    // check if attempting to balance adjacent cells
                    bool adjacentCellCollision =
                        ((balancingFlagsTarget << 1) & balancingFlags) ||
                        ((balancingFlags << 1) & balancingFlagsTarget);

                    if (adjacentCellCollision == false) {
                        balancingFlags = balancingFlagsTarget;
                    }
                }
            }

            #if BQ769X0_DEBUG
            //printf("Setting CELLBAL%d register to: %s\n", section+1, byte2char(balancingFlags));
            #endif

            balancingStatus |= balancingFlags << section;

            // set balancing register for this section
            writeRegister(CELLBAL1+section, balancingFlags);

        } // section loop
    }
    else if (balancingStatus > 0)
    {
        // clear all CELLBAL registers
        for (int section = 0; section < numberOfSections; section++)
        {
            #if BQ769X0_DEBUG
            printf("Clearing Register CELLBAL%d\n", section+1);
            #endif

            writeRegister(CELLBAL1+section, 0x0);
        }

        balancingStatus = 0;
    }
}

//----------------------------------------------------------------------------

int bq769x0::getBalancingStatus()
{
    return balancingStatus;
}

//----------------------------------------------------------------------------

void bq769x0::setShuntResistorValue(float res_mOhm)
{
    shuntResistorValue_mOhm = res_mOhm;
}

//----------------------------------------------------------------------------

void bq769x0::setThermistorBetaValue(int beta_K)
{
    thermistorBetaValue = beta_K;
}

//----------------------------------------------------------------------------

void bq769x0::setBatteryCapacity(long capacity_mAh)
{
    nominalCapacity = capacity_mAh * 3600;
}

//----------------------------------------------------------------------------

void bq769x0::setOCV(int voltageVsSOC[NUM_OCV_POINTS])
{
    OCV = voltageVsSOC;
}

//----------------------------------------------------------------------------

float bq769x0::getSOC(void)
{
    return (double) coulombCounter / nominalCapacity * 100;
}

//----------------------------------------------------------------------------

void bq769x0::resetSOC(int percent)
{
    if (percent <= 100 && percent >= 0)
    {
        coulombCounter = nominalCapacity * percent / 100.0;
    }
    else  // reset based on OCV
    {
        int voltage = getMaxCellVoltage();

        coulombCounter = 0;  // initialize with totally depleted battery (0% SOC)

        for (int i = 0; i < NUM_OCV_POINTS; i++)
        {
            if (OCV[i] <= voltage) {
                if (i == 0) {
                    coulombCounter = nominalCapacity;  // 100% full
                }
                else {
                    // interpolate between OCV[i] and OCV[i-1]
                    coulombCounter = (double) nominalCapacity / (NUM_OCV_POINTS - 1.0) *
                    (NUM_OCV_POINTS - 1.0 - i + ((float)voltage - OCV[i])/(OCV[i-1] - OCV[i]));
                }
                return;
            }
        }
    }
}

//----------------------------------------------------------------------------

void bq769x0::setTemperatureLimits(int minDischarge_degC, int maxDischarge_degC,
  int minCharge_degC, int maxCharge_degC)
{
    // Temperature limits (°C/10)
    minCellTempDischarge = minDischarge_degC * 10;
    maxCellTempDischarge = maxDischarge_degC * 10;
    minCellTempCharge = minCharge_degC * 10;
    maxCellTempCharge = maxCharge_degC * 10;
}

//----------------------------------------------------------------------------

void bq769x0::setIdleCurrentThreshold(int current_mA)
{
    idleCurrentThreshold = current_mA;
}

//----------------------------------------------------------------------------

long bq769x0::setShortCircuitProtection(long current_mA, int delay_us)
{
    regPROTECT1_t protect1;

    // only RSNS = 1 considered
    protect1.bits.RSNS = 1;

    protect1.bits.SCD_THRESH = 0;
    for (int i = sizeof(SCD_threshold_setting)-1; i > 0; i--) {
        if (current_mA * shuntResistorValue_mOhm / 1000 >= SCD_threshold_setting[i]) {
            protect1.bits.SCD_THRESH = i;
            break;
        }
    }

    protect1.bits.SCD_DELAY = 0;
    for (int i = sizeof(SCD_delay_setting)-1; i > 0; i--) {
        if (delay_us >= SCD_delay_setting[i]) {
            protect1.bits.SCD_DELAY = i;
            break;
        }
    }

    writeRegister(PROTECT1, protect1.regByte);

    // returns the actual current threshold value
    return (long)SCD_threshold_setting[protect1.bits.SCD_THRESH] * 1000 /
        shuntResistorValue_mOhm;
}

//----------------------------------------------------------------------------

long bq769x0::setOvercurrentChargeProtection(long current_mA, int delay_ms)
{
    // ToDo: Software protection for charge overcurrent
    return 0;
}

//----------------------------------------------------------------------------

long bq769x0::setOvercurrentDischargeProtection(long current_mA, int delay_ms)
{
    regPROTECT2_t protect2;

    // Remark: RSNS must be set to 1 in PROTECT1 register

    protect2.bits.OCD_THRESH = 0;
    for (int i = sizeof(OCD_threshold_setting)-1; i > 0; i--) {
        if (current_mA * shuntResistorValue_mOhm / 1000 >= OCD_threshold_setting[i]) {
            protect2.bits.OCD_THRESH = i;
            break;
        }
    }

    protect2.bits.OCD_DELAY = 0;
    for (int i = sizeof(OCD_delay_setting)-1; i > 0; i--) {
        if (delay_ms >= OCD_delay_setting[i]) {
            protect2.bits.OCD_DELAY = i;
            break;
        }
    }

    writeRegister(PROTECT2, protect2.regByte);

    // returns the actual current threshold value
    return (long)OCD_threshold_setting[protect2.bits.OCD_THRESH] * 1000 /
        shuntResistorValue_mOhm;
}


//----------------------------------------------------------------------------

int bq769x0::setCellUndervoltageProtection(int voltage_mV, int delay_s)
{
    regPROTECT3_t protect3;
    int uv_trip = 0;

    minCellVoltage = voltage_mV;

    protect3.regByte = readRegister(PROTECT3);

    uv_trip = ((((long)voltage_mV - adcOffset) * 1000 / adcGain) >> 4) & 0x00FF;
    uv_trip += 1;   // always round up for lower cell voltage
    writeRegister(UV_TRIP, uv_trip);

    protect3.bits.UV_DELAY = 0;
    for (int i = sizeof(UV_delay_setting)-1; i > 0; i--) {
        if (delay_s >= UV_delay_setting[i]) {
            protect3.bits.UV_DELAY = i;
            break;
        }
    }

    writeRegister(PROTECT3, protect3.regByte);

    // returns the actual current threshold value
    return ((long)1 << 12 | uv_trip << 4) * adcGain / 1000 + adcOffset;
}

//----------------------------------------------------------------------------

int bq769x0::setCellOvervoltageProtection(int voltage_mV, int delay_s)
{
    regPROTECT3_t protect3;
    int ov_trip = 0;

    maxCellVoltage = voltage_mV;

    protect3.regByte = readRegister(PROTECT3);

    ov_trip = ((((long)voltage_mV - adcOffset) * 1000 / adcGain) >> 4) & 0x00FF;
    writeRegister(OV_TRIP, ov_trip);

    protect3.bits.OV_DELAY = 0;
    for (int i = sizeof(OV_delay_setting)-1; i > 0; i--) {
        if (delay_s >= OV_delay_setting[i]) {
            protect3.bits.OV_DELAY = i;
            break;
        }
    }

    writeRegister(PROTECT3, protect3.regByte);

    // returns the actual current threshold value
    return ((long)1 << 13 | ov_trip << 4) * adcGain / 1000 + adcOffset;
}


//----------------------------------------------------------------------------

int bq769x0::getBatteryCurrent()
{
    return batCurrent;
}

//----------------------------------------------------------------------------

int bq769x0::getBatteryVoltage()
{
    return batVoltage;
}

//----------------------------------------------------------------------------

int bq769x0::getMaxCellVoltage()
{
    return cellVoltages[idCellMaxVoltage];
}

//----------------------------------------------------------------------------

int bq769x0::getMinCellVoltage()
{
    return cellVoltages[idCellMinVoltage];
}

//----------------------------------------------------------------------------

int bq769x0::getCellVoltage(int idCell)
{
    return cellVoltages[idCell-1];
}


//----------------------------------------------------------------------------

float bq769x0::getTemperatureDegC(int channel)
{
    if (channel >= 1 && channel <= 3) {
        return (float)temperatures[channel-1] / 10.0;
    }
    else {
        return -273.15;   // Error: Return absolute minimum temperature
    }
}

//----------------------------------------------------------------------------

float bq769x0::getTemperatureDegF(int channel)
{
    return getTemperatureDegC(channel) * 1.8 + 32;
}


//----------------------------------------------------------------------------
// TODO: support multiple temperature sensors

void bq769x0::updateTemperatures()
{
    float tmp = 0;
    int adcVal = 0;
    int vtsx = 0;
    unsigned long rts = 0;

    // calculate R_thermistor according to bq769x0 datasheet
    adcVal = (readRegister(TS1_HI_BYTE) & 0b00111111) << 8 | readRegister(TS1_LO_BYTE);
    vtsx = adcVal * 0.382; // mV
    rts = 10000.0 * vtsx / (3300.0 - vtsx); // Ohm

    // Temperature calculation using Beta equation
    // - According to bq769x0 datasheet, only 10k thermistors should be used
    // - 25°C reference temperature for Beta equation assumed
    tmp = 1.0/(1.0/(273.15+25) + 1.0/thermistorBetaValue*log(rts/10000.0)); // K

    temperatures[0] = (tmp - 273.15) * 10.0;
}


//----------------------------------------------------------------------------

void bq769x0::updateCurrent()
{
    int adcVal = 0;
    regSYS_STAT_t sys_stat;
    sys_stat.regByte = readRegister(SYS_STAT);

    // check if new current reading available
    if (sys_stat.bits.CC_READY == 1)
    {
        //printf("reading CC register...\n");
        adcVal = (readRegister(CC_HI_BYTE) << 8) | readRegister(CC_LO_BYTE);
        batCurrent = (int16_t) adcVal * 8.44 / shuntResistorValue_mOhm;  // mA

        coulombCounter += batCurrent / 4;  // is read every 250 ms

        // reduce resolution for actual current value
        if (batCurrent > -10 && batCurrent < 10) {
            batCurrent = 0;
        }

        // reset idleTimestamp
        if (abs(batCurrent) > idleCurrentThreshold) {
            idleTimestamp = _timer.read_ms();
        }

        // no error occured which caused alert
        if (!(sys_stat.regByte & 0b00111111)) {
            alertInterruptFlag = false;
        }

        writeRegister(SYS_STAT, 0b10000000);  // Clear CC ready flag
    }
}

//----------------------------------------------------------------------------
// reads all cell voltages to array cellVoltages[NUM_CELLS] and updates batVoltage

void bq769x0::updateVoltages()
{
    long adcVal = 0;
    char buf[4];

    // read battery pack voltage
    adcVal = (readRegister(BAT_HI_BYTE) << 8) | readRegister(BAT_LO_BYTE);
    batVoltage = 4.0 * adcGain * adcVal / 1000.0 + 4 * adcOffset;

    // read cell voltages
    buf[0] = (char) VC1_HI_BYTE;
    _i2c.write(I2CAddress << 1, buf, 1);;

    idCellMaxVoltage = 0;
    idCellMinVoltage = 0;
    for (int i = 0; i < numberOfCells; i++)
    {
        if (crcEnabled == true) {
            _i2c.read(I2CAddress << 1, buf, 4);
            adcVal = (buf[0] & 0b00111111) << 8 | buf[2];
            // TODO: check CRC byte buf[1] and buf[3]
        }
        else {
            _i2c.read(I2CAddress << 1, buf, 2);
            adcVal = (buf[0] & 0b00111111) << 8 | buf[1];
        }

        cellVoltages[i] = adcVal * adcGain / 1000 + adcOffset;

        if (cellVoltages[i] > cellVoltages[idCellMaxVoltage]) {
            idCellMaxVoltage = i;
        }
        if (cellVoltages[i] < cellVoltages[idCellMinVoltage] && cellVoltages[i] > 500) {
            idCellMinVoltage = i;
        }
    }
}

//----------------------------------------------------------------------------

void bq769x0::writeRegister(int address, int data)
{
    int crc = 0;
    char buf[3];

    buf[0] = (char) address;
    buf[1] = data;

    if (crcEnabled == true) {
        // CRC is calculated over the slave address (including R/W bit), register address, and data.
        crc = _crc8_ccitt_update(crc, (I2CAddress << 1) | 0);
        crc = _crc8_ccitt_update(crc, address);
        crc = _crc8_ccitt_update(crc, data);
        buf[2] = crc;
        _i2c.write(I2CAddress << 1, buf, 3);
    }
    else {
        _i2c.write(I2CAddress << 1, buf, 2);
    }
}

//----------------------------------------------------------------------------

int bq769x0::readRegister(int address)
{
    unsigned int crc = 0, crcSlave = 0, data;
    char buf[2];

    #if BQ769X0_DEBUG
    //printf("Read register: 0x%x \n", address);
    #endif

    buf[0] = (char)address;
    _i2c.write(I2CAddress << 1, buf, 1);;

    if (crcEnabled == true) {
        do {
            _i2c.read(I2CAddress << 1, buf, 2);
            data = buf[0];
            crcSlave = buf[1];
            // CRC is calculated over the slave address (including R/W bit) and data.
            crc = _crc8_ccitt_update(crc, (I2CAddress << 1) | 1);
            crc = _crc8_ccitt_update(crc, data);

        } while (crc != crcSlave);
        return data;
    }
    else {
        _i2c.read(I2CAddress << 1, buf, 1);
        return buf[0];
    }
}

//----------------------------------------------------------------------------
// The bq769x0 drives the ALERT pin high if the SYS_STAT register contains
// a new value (either new CC reading or an error)

void bq769x0::setAlertInterruptFlag()
{
    interruptTimestamp = _timer.read_ms();
    alertInterruptFlag = true;
}

#if BQ769X0_DEBUG

//----------------------------------------------------------------------------
// for debug purposes

void bq769x0::printRegisters()
{
    printf("0x00 SYS_STAT:  %s\n", byte2char(readRegister(SYS_STAT)));
    printf("0x01 CELLBAL1:  %s\n", byte2char(readRegister(CELLBAL1)));
    printf("0x04 SYS_CTRL1: %s\n", byte2char(readRegister(SYS_CTRL1)));
    printf("0x05 SYS_CTRL2: %s\n", byte2char(readRegister(SYS_CTRL2)));
    printf("0x06 PROTECT1:  %s\n", byte2char(readRegister(PROTECT1)));
    printf("0x07 PROTECT2:  %s\n", byte2char(readRegister(PROTECT2)));
    printf("0x08 PROTECT3:  %s\n", byte2char(readRegister(PROTECT3)));
    printf("0x09 OV_TRIP:   %s\n", byte2char(readRegister(OV_TRIP)));
    printf("0x0A UV_TRIP:   %s\n", byte2char(readRegister(UV_TRIP)));
    printf("0x0B CC_CFG:    %s\n", byte2char(readRegister(CC_CFG)));
    printf("0x32 CC_HI:     %s\n", byte2char(readRegister(CC_HI_BYTE)));
    printf("0x33 CC_LO:     %s\n", byte2char(readRegister(CC_LO_BYTE)));
    /*
    printf("0x50 ADCGAIN1:  %s\n", byte2char(readRegister(ADCGAIN1)));
    printf("0x51 ADCOFFSET: %s\n", byte2char(readRegister(ADCOFFSET)));
    printf("0x59 ADCGAIN2:  %s\n", byte2char(readRegister(ADCGAIN2)));
    */
}

#endif