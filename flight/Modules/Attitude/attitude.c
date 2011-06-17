/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{
 * @addtogroup Attitude Copter Control Attitude Estimation
 * @brief Acquires sensor data and computes attitude estimate 
 * Specifically updates the the @ref AttitudeActual "AttitudeActual" and @ref AttitudeRaw "AttitudeRaw" settings objects
 * @{
 *
 * @file       attitude.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      Module to handle all comms to the AHRS on a periodic basis.
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 ******************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/**
 * Input objects: None, takes sensor data via pios
 * Output objects: @ref AttitudeRaw @ref AttitudeActual
 *
 * This module computes an attitude estimate from the sensor data
 *
 * The module executes in its own thread.
 *
 * UAVObjects are automatically generated by the UAVObjectGenerator from
 * the object definition XML file.
 *
 * Modules have no API, all communication to other modules is done through UAVObjects.
 * However modules may use the API exposed by shared libraries.
 * See the OpenPilot wiki for more details.
 * http://www.openpilot.org/OpenPilot_Application_Architecture
 *
 */

#include "pios.h"
#include "attitude.h"
#include "attituderaw.h"
#include "attitudeactual.h"
#include "attitudesettings.h"
#include "flightstatus.h"
#include "CoordinateConversions.h"
#include "pios_flash_w25x.h"

// Private constants
#define STACK_SIZE_BYTES 540
#define TASK_PRIORITY (tskIDLE_PRIORITY+3)

#define UPDATE_RATE  2.0f
#define GYRO_NEUTRAL 1665

#define PI_MOD(x) (fmod(x + M_PI, M_PI * 2) - M_PI)
// Private types

// Private variables
static xTaskHandle taskHandle;

// Private functions
static void AttitudeTask(void *parameters);

static float gyro_correct_int[3] = {0,0,0};
static xQueueHandle gyro_queue;

static void updateSensors(AttitudeRawData *);
static void updateAttitude(AttitudeRawData *);
static void settingsUpdatedCb(UAVObjEvent * objEv);

static float accelKi = 0;
static float accelKp = 0;
static float yawBiasRate = 0;
static float gyroGain = 0.42;
static int16_t accelbias[3];
static float q[4] = {1,0,0,0};
static float R[3][3];
static int8_t rotate = 0;
static bool zero_during_arming = false;

/**
 * Initialise the module, called on startup
 * \returns 0 on success or -1 if initialisation failed
 */
module_initcall(AttitudeInitialize, 0);
int32_t AttitudeInitialize(void)
{
	// Initialize quaternion
	AttitudeActualData attitude;
	AttitudeActualGet(&attitude);
	attitude.q1 = 1;
	attitude.q2 = 0;
	attitude.q3 = 0;
	attitude.q4 = 0;
	AttitudeActualSet(&attitude);
	
	// Create queue for passing gyro data, allow 2 back samples in case
	gyro_queue = xQueueCreate(1, sizeof(float) * 4);
	if(gyro_queue == NULL) 
		return -1;
	PIOS_ADC_SetQueue(gyro_queue);
	
	AttitudeSettingsConnectCallback(&settingsUpdatedCb);
	
	// Start main task
	xTaskCreate(AttitudeTask, (signed char *)"Attitude", STACK_SIZE_BYTES/4, NULL, TASK_PRIORITY, &taskHandle);
	TaskMonitorAdd(TASKINFO_RUNNING_ATTITUDE, taskHandle);
	PIOS_WDG_RegisterFlag(PIOS_WDG_ATTITUDE);
	return 0;
}

/**
 * Module thread, should not return.
 */
static void AttitudeTask(void *parameters)
{

	uint8_t init = 0;
	AlarmsClear(SYSTEMALARMS_ALARM_ATTITUDE);

	PIOS_ADC_Config((PIOS_ADC_RATE / 1000.0f) * UPDATE_RATE);

	// Keep flash CS pin high while talking accel
	PIOS_FLASH_DISABLE;		
	PIOS_ADXL345_Init();
			
	zero_during_arming = false;
	// Main task loop
	while (1) {
		
		FlightStatusData flightStatus;
		FlightStatusGet(&flightStatus);

		if(xTaskGetTickCount() < 7000) {
			// Force settings update to make sure rotation loaded
			settingsUpdatedCb(AttitudeSettingsHandle());
			// For first 7 seconds use accels to get gyro bias
			accelKp = 1;
			accelKi = 0.9;
			yawBiasRate = 0.23;
			init = 0;
		} 	
		else if (zero_during_arming && (flightStatus.Armed == FLIGHTSTATUS_ARMED_ARMING)) {
			accelKp = 1;
			accelKi = 0.9;
			yawBiasRate = 0.23;
			init = 0;			
		} else if (init == 0) {
			settingsUpdatedCb(AttitudeSettingsHandle());
			init = 1;
		}						
			
		PIOS_WDG_UpdateFlag(PIOS_WDG_ATTITUDE);
		
		AttitudeRawData attitudeRaw;
		AttitudeRawGet(&attitudeRaw);		
		updateSensors(&attitudeRaw);		
		updateAttitude(&attitudeRaw);
		AttitudeRawSet(&attitudeRaw); 	

	}
}

static void updateSensors(AttitudeRawData * attitudeRaw) 
{	
	struct pios_adxl345_data accel_data;
	float gyro[4];
	
	// Only wait the time for two nominal updates before setting an alarm
	if(xQueueReceive(gyro_queue, (void * const) gyro, UPDATE_RATE * 2) == errQUEUE_EMPTY) {
		AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE, SYSTEMALARMS_ALARM_ERROR);
		return;
	}
	
	
	// First sample is temperature
	attitudeRaw->gyros[ATTITUDERAW_GYROS_X] = -(gyro[1] - GYRO_NEUTRAL) * gyroGain;
	attitudeRaw->gyros[ATTITUDERAW_GYROS_Y] = (gyro[2] - GYRO_NEUTRAL) * gyroGain;
	attitudeRaw->gyros[ATTITUDERAW_GYROS_Z] = -(gyro[3] - GYRO_NEUTRAL) * gyroGain;
	
	int32_t x = 0;
	int32_t y = 0;
	int32_t z = 0;
	uint8_t i = 0;
	uint8_t samples_remaining;
	do {
		i++;
		samples_remaining = PIOS_ADXL345_Read(&accel_data);
		x +=  accel_data.x;
		y += -accel_data.y;
		z += -accel_data.z;
	} while ( (i < 32) && (samples_remaining > 0) );
	attitudeRaw->gyrotemp[0] = samples_remaining;
	attitudeRaw->gyrotemp[1] = i;
	
	float accel[3] = {(float) x / i, (float) y / i, (float) z / i};
	
	if(rotate) {
		// TODO: rotate sensors too so stabilization is well behaved
		float vec_out[3];
		rot_mult(R, accel, vec_out);
		attitudeRaw->accels[0] = vec_out[0];
		attitudeRaw->accels[1] = vec_out[1];
		attitudeRaw->accels[2] = vec_out[2];
		rot_mult(R, attitudeRaw->gyros, vec_out);
		attitudeRaw->gyros[0] = vec_out[0];
		attitudeRaw->gyros[1] = vec_out[1];
		attitudeRaw->gyros[2] = vec_out[2];
	} else {
		attitudeRaw->accels[0] = accel[0];
		attitudeRaw->accels[1] = accel[1];
		attitudeRaw->accels[2] = accel[2];
	}		
	
	// Scale accels and correct bias
	attitudeRaw->accels[ATTITUDERAW_ACCELS_X] = (attitudeRaw->accels[ATTITUDERAW_ACCELS_X] - accelbias[0]) * 0.004f * 9.81f;
	attitudeRaw->accels[ATTITUDERAW_ACCELS_Y] = (attitudeRaw->accels[ATTITUDERAW_ACCELS_Y] - accelbias[1]) * 0.004f * 9.81f;
	attitudeRaw->accels[ATTITUDERAW_ACCELS_Z] = (attitudeRaw->accels[ATTITUDERAW_ACCELS_Z] - accelbias[2]) * 0.004f * 9.81f;
	
	// Applying integral component here so it can be seen on the gyros and correct bias
	attitudeRaw->gyros[ATTITUDERAW_GYROS_X] += gyro_correct_int[0];
	attitudeRaw->gyros[ATTITUDERAW_GYROS_Y] += gyro_correct_int[1];
	
	// Because most crafts wont get enough information from gravity to zero yaw gyro
	attitudeRaw->gyros[ATTITUDERAW_GYROS_Z] += gyro_correct_int[2];
	gyro_correct_int[2] += - attitudeRaw->gyros[ATTITUDERAW_GYROS_Z] * yawBiasRate;			
}

static void updateAttitude(AttitudeRawData * attitudeRaw)
{
	static portTickType lastSysTime = 0;
	static portTickType thisSysTime;
	
	static float dT = 0;
	
	thisSysTime = xTaskGetTickCount();
	if(thisSysTime > lastSysTime) // reuse dt in case of wraparound
		dT = (thisSysTime - lastSysTime) / portTICK_RATE_MS / 1000.0f;
	lastSysTime = thisSysTime;
	
	// Bad practice to assume structure order, but saves memory
	float gyro[3];
	gyro[0] = attitudeRaw->gyros[0];
	gyro[1] = attitudeRaw->gyros[1];
	gyro[2] = attitudeRaw->gyros[2];
	
	{
		float * accels = attitudeRaw->accels;
		float grot[3];
		float accel_err[3];
		
		// Rotate gravity to body frame and cross with accels
		grot[0] = -(2 * (q[1] * q[3] - q[0] * q[2]));
		grot[1] = -(2 * (q[2] * q[3] + q[0] * q[1]));
		grot[2] = -(q[0] * q[0] - q[1]*q[1] - q[2]*q[2] + q[3]*q[3]);
		CrossProduct((const float *) accels, (const float *) grot, accel_err);
		
		// Account for accel magnitude 
		float accel_mag = sqrt(accels[0]*accels[0] + accels[1]*accels[1] + accels[2]*accels[2]);
		accel_err[0] /= accel_mag;
		accel_err[1] /= accel_mag;
		accel_err[2] /= accel_mag;
		
		// Accumulate integral of error.  Scale here so that units are (rad/s) but Ki has units of s
		gyro_correct_int[0] += accel_err[0] * accelKi;
		gyro_correct_int[1] += accel_err[1] * accelKi;
		//gyro_correct_int[2] += accel_err[2] * settings.AccelKI * dT;
		
		// Correct rates based on error, integral component dealt with in updateSensors
		gyro[0] += accel_err[0] * accelKp / dT;
		gyro[1] += accel_err[1] * accelKp / dT;
		gyro[2] += accel_err[2] * accelKp / dT;
	}
	
	{ // scoping variables to save memory
		// Work out time derivative from INSAlgo writeup
		// Also accounts for the fact that gyros are in deg/s
		float qdot[4];
		qdot[0] = (-q[1] * gyro[0] - q[2] * gyro[1] - q[3] * gyro[2]) * dT * M_PI / 180 / 2;
		qdot[1] = (q[0] * gyro[0] - q[3] * gyro[1] + q[2] * gyro[2]) * dT * M_PI / 180 / 2;
		qdot[2] = (q[3] * gyro[0] + q[0] * gyro[1] - q[1] * gyro[2]) * dT * M_PI / 180 / 2;
		qdot[3] = (-q[2] * gyro[0] + q[1] * gyro[1] + q[0] * gyro[2]) * dT * M_PI / 180 / 2;
		
		// Take a time step
		q[0] = q[0] + qdot[0];
		q[1] = q[1] + qdot[1];
		q[2] = q[2] + qdot[2];
		q[3] = q[3] + qdot[3];
	}
	
	// Renomalize
	float qmag = sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
	q[0] = q[0] / qmag;
	q[1] = q[1] / qmag;
	q[2] = q[2] / qmag;
	q[3] = q[3] / qmag;
	
	AttitudeActualData attitudeActual;
	AttitudeActualGet(&attitudeActual);
	
	quat_copy(q, &attitudeActual.q1);
	
	// Convert into eueler degrees (makes assumptions about RPY order)
	Quaternion2RPY(&attitudeActual.q1,&attitudeActual.Roll);	

	AttitudeActualSet(&attitudeActual);
}

static void settingsUpdatedCb(UAVObjEvent * objEv) {
	AttitudeSettingsData attitudeSettings;
	AttitudeSettingsGet(&attitudeSettings);
	
	
	accelKp = attitudeSettings.AccelKp;
	accelKi = attitudeSettings.AccelKi;		
	yawBiasRate = attitudeSettings.YawBiasRate;
	gyroGain = attitudeSettings.GyroGain;
	
	zero_during_arming = attitudeSettings.ZeroDuringArming == ATTITUDESETTINGS_ZERODURINGARMING_TRUE;
	
	accelbias[0] = attitudeSettings.AccelBias[ATTITUDESETTINGS_ACCELBIAS_X];
	accelbias[1] = attitudeSettings.AccelBias[ATTITUDESETTINGS_ACCELBIAS_Y];
	accelbias[2] = attitudeSettings.AccelBias[ATTITUDESETTINGS_ACCELBIAS_Z];
	
	// Indicates not to expend cycles on rotation
	if(attitudeSettings.BoardRotation[0] == 0 && attitudeSettings.BoardRotation[1] == 0 &&
	   attitudeSettings.BoardRotation[2] == 0) {
		rotate = 0;
		
		// Shouldn't be used but to be safe
		float rotationQuat[4] = {1,0,0,0};
		Quaternion2R(rotationQuat, R);
	} else {
		float rotationQuat[4];
		const float rpy[3] = {attitudeSettings.BoardRotation[ATTITUDESETTINGS_BOARDROTATION_ROLL], 
			attitudeSettings.BoardRotation[ATTITUDESETTINGS_BOARDROTATION_PITCH], 
			attitudeSettings.BoardRotation[ATTITUDESETTINGS_BOARDROTATION_YAW]};
		RPY2Quaternion(rpy, rotationQuat);
		Quaternion2R(rotationQuat, R);
		rotate = 1;
	}		
}
/**
  * @}
  * @}
  */
