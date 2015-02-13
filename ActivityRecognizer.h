/*
 * Copyright (C) 2014 STMicroelectronics
 * Alberto Marinoni, Giuseppe Barba
 * Motion MEMS Product Div.
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


#if (SENSORS_ACTIVITY_RECOGNIZER_ENABLE == 1)

#ifndef ANDROID_ACTIVITY_RECOGNIZER_SENSOR_H
#define ANDROID_ACTIVITY_RECOGNIZER_SENSOR_H

#include "configuration.h"
#include <stdint.h>
#include <errno.h>
#include <sys/cdefs.h>
#include <sys/types.h>


#include "sensors.h"
#include "SensorBase.h"
#include "InputEventReader.h"
#include "AccelSensor.h"

extern "C"
{
	#include "ActivityRecoLib.h"
};

/*****************************************************************************/

struct input_event;

class ActivityRecognizerSensor : public SensorBase
{
	sensors_event_t mPendingEvent;
	InputEventCircularReader mInputReader;

private:
	double last_activity;
	AccelSensor *acc;
	int mEnabled;
	int64_t timestamp;

public:
	ActivityRecognizerSensor();
	virtual ~ActivityRecognizerSensor();
	virtual int readEvents(sensors_event_t *data, int count);
	virtual bool hasPendingEvents() const;
	virtual int setDelay(int32_t handle, int64_t ns);
	virtual int enable(int32_t handle, int enabled, int type);
	static bool getBufferData(sensors_vec_t *lastBufferedValues);
	static void getGyroDelay(int64_t *Gyro_Delay_ms);
	virtual int getWhatFromHandle(int32_t handle);
};

#endif  // ANDROID_ACTIVITY_RECOGNIZER_SENSOR_H

#endif /* SENSORS_ACTIVITY_RECOGNIZER_ENABLE */
 
