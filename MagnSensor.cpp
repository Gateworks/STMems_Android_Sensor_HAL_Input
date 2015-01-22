/*
 * Copyright (C) 2012 STMicroelectronics
 * Matteo Dameno, Ciocca Denis, Alberto Marinoni - Motion MEMS Product Div.
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "configuration.h"
#if (SENSORS_MAGNETIC_FIELD_ENABLE == 1)

#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/select.h>
#include <cutils/log.h>
#include <linux/time.h>

#include "MagnSensor.h"
#include SENSOR_MAG_INCLUDE_FILE_NAME

#if (SENSOR_GEOMAG_ENABLE == 1)
#include "iNemoEngineGeoMagAPI.h"
#endif

#define FETCH_FULL_EVENT_BEFORE_RETURN		0
#define MS2_TO_MG(x)				(x*102.040816327f)
#define UT_TO_MGAUSS(x)				(x*10.0f)
#define MGAUSS_TO_UT(x)				(x/10.0f)

/*****************************************************************************/

sensors_vec_t  MagnSensor::dataBuffer;
int MagnSensor::freq = 0;
int MagnSensor::count_call_ecompass = 0;
int MagnSensor::mEnabled = 0;
int64_t MagnSensor::delayms = 0;
int MagnSensor::current_fullscale = 0;
#if (SENSORS_ACCELEROMETER_ENABLE == 1)
AccelSensor* MagnSensor::acc = NULL;
#endif
static int calibration_running;
int64_t MagnSensor::setDelayBuffer[numSensors] = {0};
int MagnSensor::DecimationBuffer[numSensors] = {0};
int MagnSensor::DecimationCount[numSensors] = {0};
pthread_mutex_t MagnSensor::dataMutex;

MagnSensor::MagnSensor()
	: SensorBase(NULL, SENSOR_DATANAME_MAGNETIC_FIELD),
	mInputReader(4),
	mHasPendingEvent(false)
{
	int err;

	pthread_mutex_init(&dataMutex, NULL);

	memset(mPendingEvent, 0, sizeof(mPendingEvent));
	memset(DecimationCount, 0, sizeof(DecimationCount));

	mPendingEvent[MagneticField].version = sizeof(sensors_event_t);
	mPendingEvent[MagneticField].sensor = ID_MAGNETIC_FIELD;
	mPendingEvent[MagneticField].type = SENSOR_TYPE_MAGNETIC_FIELD;
	memset(mPendingEvent[MagneticField].data, 0, sizeof(mPendingEvent[MagneticField].data));
	mPendingEvent[MagneticField].magnetic.status = SENSOR_STATUS_UNRELIABLE;

#if (SENSORS_UNCALIB_MAGNETIC_FIELD_ENABLE == 1)
	mPendingEvent[UncalibMagneticField].version = sizeof(sensors_event_t);
	mPendingEvent[UncalibMagneticField].sensor = ID_UNCALIB_MAGNETIC_FIELD;
	mPendingEvent[UncalibMagneticField].type = SENSOR_TYPE_MAGNETIC_FIELD_UNCALIBRATED;
	memset(mPendingEvent[UncalibMagneticField].data, 0, sizeof(mPendingEvent[UncalibMagneticField].data));
	mPendingEvent[UncalibMagneticField].magnetic.status = SENSOR_STATUS_UNRELIABLE;
#endif
#if (SENSOR_FUSION_ENABLE == 0)
  #if ((GEOMAG_COMPASS_ORIENTATION_ENABLE == 1) || (SENSORS_COMPASS_ORIENTATION_ENABLE == 1))
	mPendingEvent[Orientation].version = sizeof(sensors_event_t);
	mPendingEvent[Orientation].sensor = ID_ORIENTATION;
	mPendingEvent[Orientation].type = SENSOR_TYPE_ORIENTATION;
	memset(mPendingEvent[Orientation].data, 0, sizeof(mPendingEvent[Orientation].data));
	mPendingEvent[Orientation].orientation.status = SENSOR_STATUS_UNRELIABLE;
  #endif
  #if (GEOMAG_GRAVITY_ENABLE == 1)
	mPendingEvent[Gravity_Accel].version = sizeof(sensors_event_t);
	mPendingEvent[Gravity_Accel].sensor = ID_GRAVITY;
	mPendingEvent[Gravity_Accel].type = SENSOR_TYPE_GRAVITY;
	memset(mPendingEvent[Gravity_Accel].data, 0, sizeof(mPendingEvent[Gravity_Accel].data));
	mPendingEvent[Gravity_Accel].acceleration.status = SENSOR_STATUS_ACCURACY_HIGH;
  #endif
  #if (GEOMAG_LINEAR_ACCELERATION_ENABLE == 1)
	mPendingEvent[Linear_Accel].version = sizeof(sensors_event_t);
	mPendingEvent[Linear_Accel].sensor = ID_LINEAR_ACCELERATION;
	mPendingEvent[Linear_Accel].type = SENSOR_TYPE_LINEAR_ACCELERATION;
	memset(mPendingEvent[Linear_Accel].data, 0, sizeof(mPendingEvent[Linear_Accel].data));
	mPendingEvent[Linear_Accel].acceleration.status = SENSOR_STATUS_ACCURACY_HIGH;
  #endif
#endif
#if (SENSORS_GEOMAG_ROTATION_VECTOR_ENABLE == 1)
	mPendingEvent[GeoMagRotVect_Magnetic].version = sizeof(sensors_event_t);
	mPendingEvent[GeoMagRotVect_Magnetic].sensor = ID_GEOMAG_ROTATION_VECTOR;
	mPendingEvent[GeoMagRotVect_Magnetic].type = SENSOR_TYPE_GEOMAGNETIC_ROTATION_VECTOR;
	memset(mPendingEvent[GeoMagRotVect_Magnetic].data, 0, sizeof(mPendingEvent[GeoMagRotVect_Magnetic].data));
	mPendingEvent[GeoMagRotVect_Magnetic].magnetic.status = SENSOR_STATUS_UNRELIABLE;

	memset(&sData, 0, sizeof(iNemoGeoMagSensorsData));
	iNemoEngine_GeoMag_API_Initialization();
#endif
	if (data_fd) {
		STLOGI("MagnSensor::MagnSensor magn_device_sysfs_path:(%s)", sysfs_device_path);
	} else {
		STLOGE("MagnSensor::MagnSensor magn_device_sysfs_path:(%s) not found", sysfs_device_path);
	}

	memset(data_raw, 0, sizeof(data_raw));

#if ((MAG_CALIBRATION_ENABLE == 1) || (SENSOR_GEOMAG_ENABLE == 1))
	acc = new AccelSensor();
#endif

#if (MAG_CALIBRATION_ENABLE == 1)
	compass_API_Init(0, 0, HIGH_SENSITIVITY, DEFAULT_CALIB_DATA_FILE);
  #if (MAG_SI_COMPENSATION_ENABLED == 1)
	if (compass_API_loadSIMatrixFromFile(DEFAULT_SI_MATRIX_FILEPATH) != 0)
		ALOGE("MagnSensor: error while loading SI file %s\n",
							DEFAULT_SI_MATRIX_FILEPATH);
  #endif
#endif
}

MagnSensor::~MagnSensor() {
	if (mEnabled) {
		enable(SENSORS_MAGNETIC_FIELD_HANDLE, 0, 0);
		mEnabled = 0;
	}
	pthread_mutex_destroy(&dataMutex);
#if (SENSORS_ACCELEROMETER_ENABLE == 1)
	acc->~AccelSensor();
#endif
#if (MAG_CALIBRATION_ENABLE == 1)
	compass_API_DeInit();
#endif
}

int MagnSensor::setInitialState()
{
	struct input_absinfo absinfo_x;
	struct input_absinfo absinfo_y;
	struct input_absinfo absinfo_z;
	float value;

#if (MAG_CALIBRATION_ENABLE == 1)
	data_read = 0;
#endif

	if (!ioctl(data_fd, EVIOCGABS(EVENT_TYPE_MAG_X), &absinfo_x) &&
		!ioctl(data_fd, EVIOCGABS(EVENT_TYPE_MAG_Y), &absinfo_y) &&
		!ioctl(data_fd, EVIOCGABS(EVENT_TYPE_MAG_Z), &absinfo_z))
	{
		mHasPendingEvent = true;
	}

	setFullScale(SENSORS_MAGNETIC_FIELD_HANDLE, MAGN_DEFAULT_FULLSCALE);
	memset(DecimationCount, 0, sizeof(DecimationCount));

	return 0;
}

int MagnSensor::getWhatFromHandle(int32_t handle)
{
	int what = -1;

	switch(handle) {

		case SENSORS_MAGNETIC_FIELD_HANDLE:
			what = MagneticField;
			break;

#if (SENSORS_UNCALIB_MAGNETIC_FIELD_ENABLE == 1)
		case SENSORS_UNCALIB_MAGNETIC_FIELD_HANDLE:
			what = UncalibMagneticField;
			break;
#endif
#if (SENSOR_FUSION_ENABLE == 1)
		case SENSORS_SENSOR_FUSION_HANDLE:
			what = iNemoMagnetic;
			break;
#endif
#if (SENSORS_GEOMAG_ROTATION_VECTOR_ENABLE == 1)
		case SENSORS_GEOMAG_ROTATION_VECTOR_HANDLE:
			what = GeoMagRotVect_Magnetic;
			break;
#endif
#if ((SENSOR_FUSION_ENABLE == 0) && (MAG_CALIBRATION_ENABLE == 1))
  #if (GEOMAG_GRAVITY_ENABLE == 1)
		case SENSORS_GRAVITY_HANDLE:
			what = Gravity_Accel;
			break;
  #endif
  #if (GEOMAG_LINEAR_ACCELERATION_ENABLE == 1)
		case SENSORS_LINEAR_ACCELERATION_HANDLE:
			what = Linear_Accel;
			break;
  #endif
#endif
#if ((GEOMAG_COMPASS_ORIENTATION_ENABLE == 1) || (SENSORS_COMPASS_ORIENTATION_ENABLE == 1))
		case SENSORS_ORIENTATION_HANDLE:
			what = Orientation;
			break;
#endif
#if (SENSORS_VIRTUAL_GYROSCOPE_ENABLE == 1)
		case SENSORS_VIRTUAL_GYROSCOPE_HANDLE:
			what = VirtualGyro;
			break;
#endif
		default:
			what = -1;
	}

	return what;
}

int MagnSensor::enable(int32_t handle, int en, int __attribute__((unused))type)
{
	int err = 0;
	int flags = en ? 1 : 0;
	int what = -1;

	what = getWhatFromHandle(handle);
	if (what < 0)
		return what;

	if (flags) {
		if (!mEnabled) {
			setInitialState();
			err = writeEnable(SENSORS_MAGNETIC_FIELD_HANDLE, flags);
			if(err >= 0) {
				err = 0;
			}
		}

#if (MAG_CALIBRATION_ENABLE == 1)
		acc->enable(SENSORS_MAGNETIC_FIELD_HANDLE, flags, 2);
#endif
#if (SENSORS_GEOMAG_ROTATION_VECTOR_ENABLE == 1)
		if (what == GeoMagRotVect_Magnetic)
				acc->enable(SENSORS_GEOMAG_ROTATION_VECTOR_HANDLE, flags, 3);

#endif
#if (SENSOR_FUSION_ENABLE == 0)
  #if ((GEOMAG_COMPASS_ORIENTATION_ENABLE == 1) || (SENSORS_COMPASS_ORIENTATION_ENABLE == 1))
		if (what == Orientation)
				acc->enable(SENSORS_ORIENTATION_HANDLE, flags, 4);
  #endif
  #if (GEOMAG_GRAVITY_ENABLE == 1)
		if (what == Gravity_Accel)
				acc->enable(SENSORS_GRAVITY_HANDLE, flags, 5);
  #endif
  #if (GEOMAG_LINEAR_ACCELERATION_ENABLE == 1)
		if (what == Linear_Accel)
				acc->enable(SENSORS_LINEAR_ACCELERATION_HANDLE, flags, 6);
  #endif
#endif
		mEnabled |= (1<<what);
	} else {
		mEnabled &= ~(1<<what);

		if (!mEnabled) {
			err = writeEnable(SENSORS_MAGNETIC_FIELD_HANDLE, flags);
			if(err >= 0)
				err = 0;
		}
#if (MAG_CALIBRATION_ENABLE == 1)
			acc->enable(SENSORS_MAGNETIC_FIELD_HANDLE, flags, 2);
#endif
#if (SENSORS_GEOMAG_ROTATION_VECTOR_ENABLE == 1)
				if (what == GeoMagRotVect_Magnetic)
					acc->enable(SENSORS_GEOMAG_ROTATION_VECTOR_HANDLE, flags, 3);
#endif
#if (SENSOR_FUSION_ENABLE == 0)
  #if ((GEOMAG_COMPASS_ORIENTATION_ENABLE == 1) || (SENSORS_COMPASS_ORIENTATION_ENABLE == 1))
				if (what == Orientation)
					acc->enable(SENSORS_ORIENTATION_HANDLE, flags, 4);
  #endif
  #if (GEOMAG_GRAVITY_ENABLE == 1)
				if (what == Gravity_Accel)
					acc->enable(SENSORS_GRAVITY_HANDLE, flags, 5);
  #endif
  #if (GEOMAG_LINEAR_ACCELERATION_ENABLE == 1)
				if (what == Linear_Accel)
					acc->enable(SENSORS_ORIENTATION_HANDLE, flags, 6);
  #endif
#endif
		setDelay(handle, DELAY_OFF);
	}

	if(err >= 0 )
		STLOGD("MagSensor::enable(%d), handle: %d, what: %d, mEnabled: %x",
						flags, handle, what, mEnabled);
	else
		STLOGE("MagSensor::enable(%d), handle: %d, what: %d, mEnabled: %x",
						flags, handle, what, mEnabled);

	return err;
}

bool MagnSensor::hasPendingEvents() const
{
	return mHasPendingEvent;
}

int MagnSensor::setDelay(int32_t handle, int64_t delay_ns)
{
	int err = 0;
	int kk;
	int what = -1;
	int64_t delay_ms = NSEC_TO_MSEC(delay_ns);
	int64_t Min_delay_ms = 0;

	if(delay_ms == 0)
		return err;

	what = getWhatFromHandle(handle);
	if (what < 0)
		return what;

#if (MAG_CALIBRATION_ENABLE == 1)
				acc->setDelay(SENSORS_MAGNETIC_FIELD_HANDLE, SEC_TO_NSEC(1.0f / CALIBRATION_FREQUENCY));
#endif
#if (SENSORS_GEOMAG_ROTATION_VECTOR_ENABLE == 1)
				if (what == GeoMagRotVect_Magnetic)
					acc->setDelay(SENSORS_GEOMAG_ROTATION_VECTOR_HANDLE, SEC_TO_NSEC(1.0f / GEOMAG_FREQUENCY));
#endif
#if ((SENSOR_FUSION_ENABLE == 0) && (MAG_CALIBRATION_ENABLE == 1))
  #if ((GEOMAG_COMPASS_ORIENTATION_ENABLE == 1) || (SENSORS_COMPASS_ORIENTATION_ENABLE == 1))
				if (what == Orientation)
					acc->setDelay(SENSORS_ORIENTATION_HANDLE, SEC_TO_NSEC(1.0f / GEOMAG_FREQUENCY));
  #endif
  #if (GEOMAG_GRAVITY_ENABLE == 1)
				if (what == Gravity_Accel)
					acc->setDelay(SENSORS_GRAVITY_HANDLE, SEC_TO_NSEC(1.0f / GEOMAG_FREQUENCY));
  #endif
  #if (SENSORS_LINEAR_ACCELERATION_ENABLE == 1)
				if (what == Linear_Accel)
					acc->setDelay(SENSORS_LINEAR_ACCELERATION_HANDLE, SEC_TO_NSEC(1.0f / GEOMAG_FREQUENCY));
  #endif
#endif
	/**
	 * The handled sensor is disabled. Set 0 in its setDelayBuffer position
	 * and update decimation buffer.
	 */
	if (delay_ms == NSEC_TO_MSEC(DELAY_OFF))
		delay_ms = 0;

	// Min setDelay Definition
	setDelayBuffer[what] = delay_ms;
	for(kk = 0; kk < numSensors; kk++)
	{
		if (Min_delay_ms != 0) {
			if ((setDelayBuffer[kk] != 0) && (setDelayBuffer[kk] <= Min_delay_ms))
				Min_delay_ms = setDelayBuffer[kk];
		} else
			Min_delay_ms = setDelayBuffer[kk];
	}
#if (MAG_CALIBRATION_ENABLE == 1)
	if(Min_delay_ms > 1000 / CALIBRATION_FREQUENCY)
		Min_delay_ms = 1000 / CALIBRATION_FREQUENCY;
#endif
#if ((SENSORS_GEOMAG_ROTATION_VECTOR_ENABLE == 1) || (((GEOMAG_COMPASS_ORIENTATION_ENABLE == 1) ||\
	(SENSORS_COMPASS_ORIENTATION_ENABLE == 1) || (GEOMAG_LINEAR_ACCELERATION_ENABLE == 1) ||\
	(GEOMAG_GRAVITY_ENABLE == 1))  && (SENSOR_FUSION_ENABLE == 0)))
	if ((what == GeoMagRotVect_Magnetic) || (what == Orientation)
			 || (what == Linear_Accel) || (what == Gravity_Accel)){
		if(Min_delay_ms > (1000 / GEOMAG_FREQUENCY))
			Min_delay_ms = 1000 / GEOMAG_FREQUENCY;
	}
#endif
	if ((Min_delay_ms > 0) && (Min_delay_ms != delayms))
	{
		err = writeDelay(SENSORS_MAGNETIC_FIELD_HANDLE, Min_delay_ms);
		if(err >= 0) {
			err = 0;
			delayms = Min_delay_ms;
			freq = 1000/Min_delay_ms;
#if (MAG_CALIBRATION_ENABLE == 1)
			count_call_ecompass = freq/CALIBRATION_FREQUENCY;
#endif
			memset(DecimationCount, 0, sizeof(DecimationCount));
		}
	}

	// Decimation Definition
	for(kk = 0; kk < numSensors; kk++)
	{
		if (delayms)
			DecimationBuffer[kk] = setDelayBuffer[kk]/delayms;
		else
			DecimationBuffer[kk] = 0;
	}

#if DEBUG_MAGNETOMETER == 1
	STLOGD("MagSensor::setDelayBuffer[] = %lld, %lld, %lld, %lld, %lld, %lld, %lld",
				setDelayBuffer[0], setDelayBuffer[1], setDelayBuffer[2],
				setDelayBuffer[3], setDelayBuffer[4], setDelayBuffer[5],
				setDelayBuffer[6]);
	STLOGD("MagSensor::Min_delay_ms = %lld, delayms = %lld, mEnabled = %d",
				Min_delay_ms, delayms, mEnabled);
	STLOGD("MagSensor::DecimationBuffer = %d, %d, %d, %d, %d, %d, %d",
				DecimationBuffer[0], DecimationBuffer[1], DecimationBuffer[2],
				DecimationBuffer[3], DecimationBuffer[4], DecimationBuffer[5],
				DecimationBuffer[6]);
#endif

	return err;
}

int MagnSensor::setFullScale(int32_t __attribute__((unused))handle, int value)
{
	int err = -1;

	if(value <= 0)
		return err;
	else
		err = 0;

	if(value != current_fullscale)
	{
		err = writeFullScale(SENSORS_MAGNETIC_FIELD_HANDLE, value);
		if(err >= 0) {
			err = 0;
			current_fullscale = value;
		}
	}
	return err;
}

int MagnSensor::readEvents(sensors_event_t *data, int count)
{
	int err;
	float MagOffset[3];

	if (count < 1)
		return -EINVAL;

	if (mHasPendingEvent) {
		mHasPendingEvent = false;
	}

	ssize_t n = mInputReader.fill(data_fd);
	if (n < 0)
		return n;

	int numEventReceived = 0;
	input_event const* event;

#if FETCH_FULL_EVENT_BEFORE_RETURN
	again:
#endif

	while (count && mInputReader.readEvent(&event)) {

		if (event->type == EV_ABS) {
			float value = (float) event->value;

			if (event->code == EVENT_TYPE_MAG_X) {
				data_raw[0] = value * CONVERT_M_X;
			} else if (event->code == EVENT_TYPE_MAG_Y) {
				data_raw[1] = value * CONVERT_M_Y;
			} else if (event->code == EVENT_TYPE_MAG_Z) {
				data_raw[2] = value * CONVERT_M_Z;
			} else {
				STLOGE("MagnSensor: unknown event code (type = %d, code = %d)", event->type, event->code);
			}
		} else if (event->type == EV_SYN) {
			data_rot[0] =	data_raw[0] * matrix_mag[0][0] +
					data_raw[1] * matrix_mag[1][0] +
					data_raw[2] * matrix_mag[2][0];
			data_rot[1] = 	data_raw[0] * matrix_mag[0][1] +
					data_raw[1] * matrix_mag[1][1] +
					data_raw[2] * matrix_mag[2][1];
			data_rot[2] = 	data_raw[0] * matrix_mag[0][2] +
					data_raw[1] * matrix_mag[1][2] +
					data_raw[2] * matrix_mag[2][2];

#if (SENSORS_ACCELEROMETER_ENABLE == 1)
			AccelSensor::getBufferData(&mSensorsBufferedVectors[ID_ACCELEROMETER]);
#endif
#if (MAG_CALIBRATION_ENABLE == 1)
			compass_API_SaveMag(data_rot[0], data_rot[1], data_rot[2]);
  #if (MAG_SI_COMPENSATION_ENABLED == 1)
    #if (DEBUG_MAG_SI_COMPENSATION == 1)
			ALOGD("Mag RAW Data [uT]: %f  %f  %f", data_rot[0],
							data_rot[1], data_rot[2]);
    #endif
			compass_API_getSICalibratedData(data_rot);
  #endif
			compass_API_SaveAcc(mSensorsBufferedVectors[ID_ACCELEROMETER].x,
						mSensorsBufferedVectors[ID_ACCELEROMETER].y,
						mSensorsBufferedVectors[ID_ACCELEROMETER].z);
			calibration_running = compass_Calibration_Run();
  #if (DEBUG_CALIBRATION == 1)
				STLOGD("Accelerometer Data [m/s^2]:\t%f\t%f\t%f",
							mSensorsBufferedVectors[ID_ACCELEROMETER].x,
							mSensorsBufferedVectors[ID_ACCELEROMETER].y,
							mSensorsBufferedVectors[ID_ACCELEROMETER].z);
				STLOGD("Calibration Running: %d, MagData [uT] -> x:%f y:%f z:%f",
							calibration_running,
							data_rot[0], data_rot[1],
							data_rot[2]);
  #endif
#endif
#if (SENSORS_COMPASS_ORIENTATION_ENABLE == 1)
			if(data_read >= count_call_ecompass) {
				data_read = 0;

				compass_Run_ecompass();
			}
			data_read++;
#endif
			if (mEnabled & ((1 << MagneticField) |
						(1 << UncalibMagneticField) |
						(1 << GeoMagRotVect_Magnetic) |
						(1 << Orientation) |
						(1 << Linear_Accel) |
						(1 << Gravity_Accel) |
						(1 << iNemoMagnetic) |
						(1 << VirtualGyro))) {
				/**
				 * Get and apply Hard Iron calibration to raw mag data
				 */
#if (MAG_CALIBRATION_ENABLE == 1)
				compass_API_getCalibrationData(&cf);
				data_calibrated.v[0] = data_rot[0] - cf.magOffX;
				data_calibrated.v[1] = data_rot[1] - cf.magOffY;
				data_calibrated.v[2] = data_rot[2] - cf.magOffZ;
				data_calibrated.status = compass_API_Get_Calibration_Accuracy();
				MagOffset[0] = cf.magOffX;
				MagOffset[1] = cf.magOffY;
				MagOffset[2] = cf.magOffZ;

#if (DEBUG_MAGNETOMETER == 1)
				STLOGD("MagnSensor::MagCalibData: %f, %f, %f", data_calibrated.v[0], data_calibrated.v[1], data_calibrated.v[2]);
#endif


  #if (DEBUG_CALIBRATION == 1)
				STLOGD("Calibration accuracy = %d\tmag offset = %f\t%f\t%f",
						data_calibrated.status,
						cf.magOffX, cf.magOffY, cf.magOffZ);
  #endif
#else
				/**
				 * No calibration is available!
				 */
				memset(data_calibrated.v, 0, sizeof(data_calibrated.v));
				data_calibrated.status = SENSOR_STATUS_UNRELIABLE;
#endif

#if ((SENSORS_GEOMAG_ROTATION_VECTOR_ENABLE == 1) || (((GEOMAG_COMPASS_ORIENTATION_ENABLE == 1) ||\
	(GEOMAG_LINEAR_ACCELERATION_ENABLE == 1) ||\
	(GEOMAG_GRAVITY_ENABLE == 1))  && (SENSOR_FUSION_ENABLE == 0)))
				memcpy(sData.accel,
				       mSensorsBufferedVectors[ID_ACCELEROMETER].v,
							sizeof(sData.accel));
				memcpy(sData.magn, data_calibrated.v,
							sizeof(data_calibrated.v));
				iNemoEngine_GeoMag_API_Run(MagnSensor::delayms, &sData);
#endif
				DecimationCount[MagneticField]++;
				if((mEnabled & (1<<MagneticField)) && (DecimationCount[MagneticField] >= DecimationBuffer[MagneticField])) {
					DecimationCount[MagneticField] = 0;
					mPendingEvent[MagneticField].magnetic.status =
							data_calibrated.status;
					memcpy(mPendingEvent[MagneticField].data,
							data_calibrated.v,
							sizeof(data_calibrated.v));
					mPendingEvent[MagneticField].timestamp = timevalToNano(event->time);
					*data++ = mPendingEvent[MagneticField];
					count--;
					numEventReceived++;
				}
#if (SENSORS_UNCALIB_MAGNETIC_FIELD_ENABLE == 1)
				DecimationCount[UncalibMagneticField]++;
				if((mEnabled & (1<<UncalibMagneticField)) && (DecimationCount[UncalibMagneticField] >= DecimationBuffer[UncalibMagneticField])) {
					DecimationCount[UncalibMagneticField] = 0;
					mPendingEvent[UncalibMagneticField].magnetic.status = 
							data_calibrated.status;
					memcpy(mPendingEvent[UncalibMagneticField].uncalibrated_magnetic.uncalib,
							data_rot, sizeof(data_rot));
					memcpy(mPendingEvent[UncalibMagneticField].uncalibrated_magnetic.bias,
							MagOffset, sizeof(MagOffset));
					mPendingEvent[UncalibMagneticField].timestamp =
							timevalToNano(event->time);
					*data++ = mPendingEvent[UncalibMagneticField];
					count--;
					numEventReceived++;
				}
#endif
#if (SENSORS_GEOMAG_ROTATION_VECTOR_ENABLE == 1)
				DecimationCount[GeoMagRotVect_Magnetic]++;
				if((mEnabled & (1<<GeoMagRotVect_Magnetic)) && (DecimationCount[GeoMagRotVect_Magnetic] >= DecimationBuffer[GeoMagRotVect_Magnetic])) {
					DecimationCount[GeoMagRotVect_Magnetic] = 0;

					err = iNemoEngine_GeoMag_API_Get_Quaternion(mPendingEvent[GeoMagRotVect_Magnetic].data);
					if (err == 0) {
						mPendingEvent[GeoMagRotVect_Magnetic].magnetic.status =
							data_calibrated.status;
						mPendingEvent[GeoMagRotVect_Magnetic].data[4] = -1;
						mPendingEvent[GeoMagRotVect_Magnetic].timestamp =
							timevalToNano(event->time);
						*data++ = mPendingEvent[GeoMagRotVect_Magnetic];
						count--;
						numEventReceived++;
					}
				}
#endif
#if (SENSOR_FUSION_ENABLE == 0)
  #if (SENSOR_GEOMAG_ENABLE == 1)
    #if ((GEOMAG_LINEAR_ACCELERATION_ENABLE == 1))
				DecimationCount[Linear_Accel]++;
				if((mEnabled & (1<<Linear_Accel)) && (DecimationCount[Linear_Accel] >= DecimationBuffer[Linear_Accel])) {
					DecimationCount[Linear_Accel] = 0;
					err = iNemoEngine_GeoMag_API_Get_LinAcc(mPendingEvent[Linear_Accel].data);
					if (err == 0) {
						mPendingEvent[Linear_Accel].timestamp = timevalToNano(event->time);
						*data++ = mPendingEvent[Linear_Accel];
						count--;
						numEventReceived++;
					}
				}
    #endif
    #if ((GEOMAG_GRAVITY_ENABLE == 1))
				DecimationCount[Gravity_Accel]++;
				if((mEnabled & (1<<Gravity_Accel)) && (DecimationCount[Gravity_Accel] >= DecimationBuffer[Gravity_Accel])) {
					DecimationCount[Gravity_Accel] = 0;
					err = iNemoEngine_GeoMag_API_Get_Gravity(mPendingEvent[Gravity_Accel].data);
					if (err == 0) {
						mPendingEvent[Gravity_Accel].timestamp =
							timevalToNano(event->time);
						*data++ = mPendingEvent[Gravity_Accel];
						count--;
						numEventReceived++;
					}
				}
    #endif
  #endif
  #if ((GEOMAG_COMPASS_ORIENTATION_ENABLE == 1) || (SENSORS_COMPASS_ORIENTATION_ENABLE == 1))
				DecimationCount[Orientation]++;
				if((mEnabled & (1<<Orientation)) && (DecimationCount[Orientation] >= DecimationBuffer[Orientation])) {
					DecimationCount[Orientation] = 0;
    #if (SENSORS_COMPASS_ORIENTATION_ENABLE == 1)
					orientation_data odata;

					compass_API_OrientationValues(&odata);
					mPendingEvent[Orientation].orientation.azimuth =
							odata.azimuth;
					mPendingEvent[Orientation].orientation.pitch =
							odata.pitch;
					mPendingEvent[Orientation].orientation.roll =
							odata.roll;
					STLOGD("ORIENTATION:\t%f\t%f\t%f", odata.azimuth,
						odata.pitch, odata.roll);
    #else
					err = iNemoEngine_GeoMag_API_Get_Hpr(mPendingEvent[Orientation].data);
					if (err == 0)
    #endif
					{
						mPendingEvent[Orientation].orientation.status =
							data_calibrated.status;
						mPendingEvent[Orientation].timestamp =
							timevalToNano(event->time);
						*data++ = mPendingEvent[Orientation];
						count--;
						numEventReceived++;
					}
				}
  #endif
#endif
#if (SENSOR_FUSION_ENABLE == 1)
				if(mEnabled & (1<<iNemoMagnetic))
					setBufferData(&data_calibrated);
#endif
#if DEBUG_MAGNETOMETER == 1
				STLOGD("MagnSensor::readEvents (time = %lld),"
						"count(%d), received(%d)",
						mPendingEvent[MagneticField].timestamp,
						count, numEventReceived);
#endif
			}
		} else
			STLOGE("MagnSensor: unknown event (type = %d, code = %d)",
							event->type, event->code);

		mInputReader.next();
	}
#if FETCH_FULL_EVENT_BEFORE_RETURN
	/**
	 * if we didn't read a complete event, see if we can fill and
	 * try again instead of returning with nothing and redoing poll.
	 */
	if (numEventReceived == 0 && mEnabled != 0) {
		n = mInputReader.fill(data_fd);
		if (n)
			goto again;
	}
#endif
	return numEventReceived;
}

bool MagnSensor::setBufferData(sensors_vec_t *value)
{
	pthread_mutex_lock(&dataMutex);
	memcpy(&dataBuffer, value, sizeof(sensors_vec_t));
	pthread_mutex_unlock(&dataMutex);

	return true;
}

bool MagnSensor::getBufferData(sensors_vec_t *lastBufferedValues)
{
	pthread_mutex_lock(&dataMutex);
	memcpy(lastBufferedValues, &dataBuffer, sizeof(sensors_vec_t));
	pthread_mutex_unlock(&dataMutex);

	return true;
}

#endif /* SENSORS_MAGNETIC_FIELD_ENABLE */