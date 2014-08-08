/*
 * Copyright (C) 2014 Intel Corporation.
 */

#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <utils/Log.h>
#include <hardware/sensors.h>
#include "control.h"
#include "enumeration.h"
#include "utils.h"
#include "transform.h"
#include "calibration.h"
#include "description.h"

/* Currently active sensors count, per device */
static int poll_sensors_per_dev[MAX_DEVICES];	/* poll-mode sensors */
static int trig_sensors_per_dev[MAX_DEVICES];	/* trigger, event based */

static int device_fd[MAX_DEVICES];   /* fd on the /dev/iio:deviceX file */

static int poll_fd; /* epoll instance covering all enabled sensors */

static int active_poll_sensors; /* Number of enabled poll-mode sensors */

/* We use pthread condition variables to get worker threads out of sleep */
static pthread_cond_t  thread_release_cond	[MAX_SENSORS];
static pthread_mutex_t thread_release_mutex	[MAX_SENSORS];

/*
 * We associate tags to each of our poll set entries. These tags have the
 * following values:
 * - a iio device number if the fd is a iio character device fd
 * - THREAD_REPORT_TAG_BASE + sensor handle if the fd is the receiving end of a
 *   pipe used by a sysfs data acquisition thread
 *  */
#define THREAD_REPORT_TAG_BASE	0x00010000


static int enable_buffer(int dev_num, int enabled)
{
	char sysfs_path[PATH_MAX];

	sprintf(sysfs_path, ENABLE_PATH, dev_num);

	/* Low level, non-multiplexed, enable/disable routine */
	return sysfs_write_int(sysfs_path, enabled);
}


static int setup_trigger(int dev_num, const char* trigger_val)
{
	char sysfs_path[PATH_MAX];

	sprintf(sysfs_path, TRIGGER_PATH, dev_num);

	return sysfs_write_str(sysfs_path, trigger_val);
}


void build_sensor_report_maps(int dev_num)
{
	/*
	 * Read sysfs files from a iio device's scan_element directory, and
	 * build a couple of tables from that data. These tables will tell, for
	 * each sensor, where to gather relevant data in a device report, i.e.
	 * the structure that we read from the /dev/iio:deviceX file in order to
	 * sensor report, itself being the data that we return to Android when a
	 * sensor poll completes. The mapping should be straightforward in the
	 * case where we have a single sensor active per iio device but, this is
	 * not the general case. In general several sensors can be handled
	 * through a single iio device, and the _en, _index and _type syfs
	 * entries all concur to paint a picture of what the structure of the
	 * device report is.
	 */

	int s;
	int c;
	int n;
	int i;
	int ch_index;
	char* ch_spec;
	char spec_buf[MAX_TYPE_SPEC_LEN];
	struct datum_info_t* ch_info;
	int size;
	char sysfs_path[PATH_MAX];
	int known_channels;
	int offset;
	int channel_size_from_index[MAX_SENSORS * MAX_CHANNELS] = { 0 };
	int sensor_handle_from_index[MAX_SENSORS * MAX_CHANNELS] = { 0 };
	int channel_number_from_index[MAX_SENSORS * MAX_CHANNELS] = { 0 };

	known_channels = 0;

	/* For each sensor that is linked to this device */
	for (s=0; s<sensor_count; s++) {
		if (sensor_info[s].dev_num != dev_num)
			continue;

		i = sensor_info[s].catalog_index;

		/* Read channel details through sysfs attributes */
		for (c=0; c<sensor_info[s].num_channels; c++) {

			/* Read _type file */
			sprintf(sysfs_path, CHANNEL_PATH "%s",
				sensor_info[s].dev_num,
				sensor_catalog[i].channel[c].type_path);

			n = sysfs_read_str(sysfs_path, spec_buf, 
						sizeof(spec_buf));

			if (n == -1) {
					ALOGW(	"Failed to read type: %s\n",
					sysfs_path);
					continue;
				}

			ch_spec = sensor_info[s].channel[c].type_spec;

			memcpy(ch_spec, spec_buf, sizeof(spec_buf));

			ch_info = &sensor_info[s].channel[c].type_info;

			size = decode_type_spec(ch_spec, ch_info);

			/* Read _index file */
			sprintf(sysfs_path, CHANNEL_PATH "%s",
				sensor_info[s].dev_num,
				sensor_catalog[i].channel[c].index_path);

			n = sysfs_read_int(sysfs_path, &ch_index);

			if (n == -1) {
					ALOGW(	"Failed to read index: %s\n",
						sysfs_path);
					continue;
				}

			if (ch_index >= MAX_SENSORS) {
				ALOGE("Index out of bounds!: %s\n", sysfs_path);
				continue;
			}

			/* Record what this index is about */

			sensor_handle_from_index [ch_index] = s;
			channel_number_from_index[ch_index] = c;
			channel_size_from_index  [ch_index] = size;

			known_channels++;
		}

		/* Stop sampling - if we are recovering from hal restart */
                enable_buffer(dev_num, 0);
                setup_trigger(dev_num, "\n");

		/* Turn on channels we're aware of */
		for (c=0;c<sensor_info[s].num_channels; c++) {
			sprintf(sysfs_path, CHANNEL_PATH "%s",
				sensor_info[s].dev_num,
				sensor_catalog[i].channel[c].en_path);
			sysfs_write_int(sysfs_path, 1);
		}
	}

	ALOGI("Found %d channels on iio device %d\n", known_channels, dev_num);

	/*
	 * Now that we know which channels are defined, their sizes and their
	 * ordering, update channels offsets within device report. Note: there
	 * is a possibility that several sensors share the same index, with
	 * their data fields being isolated by masking and shifting as specified
	 * through the real bits and shift values in type attributes. This case
	 * is not currently supported. Also, the code below assumes no hole in
	 * the sequence of indices, so it is dependent on discovery of all
	 * sensors.
	 */
	 offset = 0;
	 for (i=0; i<MAX_SENSORS * MAX_CHANNELS; i++) {
		s =	sensor_handle_from_index[i];
		c =	channel_number_from_index[i];
		size = 	channel_size_from_index[i];

		if (!size)
			continue;

		ALOGI("S%d C%d : offset %d, size %d, type %s\n",
		      s, c, offset, size, sensor_info[s].channel[c].type_spec);

		sensor_info[s].channel[c].offset	= offset;
		sensor_info[s].channel[c].size		= size;

		offset += size;
	 }
}


int adjust_counters (int s, int enabled)
{
	/*
	 * Adjust counters based on sensor enable action. Return values are:
	 * -1 if there's an inconsistency: abort action in this case
	 *  0 if the operation was completed and we're all set
	 *  1 if we toggled the state of the sensor and there's work left
	 */

	int dev_num = sensor_info[s].dev_num;
	int catalog_index = sensor_info[s].catalog_index;
	int sensor_type = sensor_catalog[catalog_index].type;

	/* Refcount per sensor, in terms of enable count */
	if (enabled) {
		ALOGI("Enabling sensor %d (iio device %d: %s)\n",
			s, dev_num, sensor_info[s].friendly_name);

		sensor_info[s].enable_count++;

		if (sensor_info[s].enable_count > 1)
			return 0; /* The sensor was, and remains, in use */

		switch (sensor_type) {
			case SENSOR_TYPE_MAGNETIC_FIELD:
				compass_read_data(&sensor_info[s]);
				break;

			case SENSOR_TYPE_GYROSCOPE:
			case SENSOR_TYPE_GYROSCOPE_UNCALIBRATED:
				gyro_cal_init(&sensor_info[s]);
				break;
		}
	} else {
		if (sensor_info[s].enable_count == 0)
			return -1; /* Spurious disable call */

		ALOGI("Disabling sensor %d (iio device %d: %s)\n", s, dev_num,
		      sensor_info[s].friendly_name);

		sensor_info[s].enable_count--;

		if (sensor_info[s].enable_count > 0)
			return 0; /* The sensor was, and remains, in use */

		/* Sensor disabled, lower report available flag */
		sensor_info[s].report_pending = 0;

		if (sensor_type == SENSOR_TYPE_MAGNETIC_FIELD)
			compass_store_data(&sensor_info[s]);
	}


	/* If uncalibrated type and pair is already active don't adjust counters */
	if (sensor_type == SENSOR_TYPE_GYROSCOPE_UNCALIBRATED &&
		sensor_info[sensor_info[s].pair_idx].enable_count != 0)
			return 0;

	/* We changed the state of a sensor - adjust per iio device counters */

	/* If this is a regular event-driven sensor */
	if (sensor_info[s].num_channels) {

			if (enabled)
				trig_sensors_per_dev[dev_num]++;
			else
				trig_sensors_per_dev[dev_num]--;

			return 1;
		}

	if (enabled) {
		active_poll_sensors++;
		poll_sensors_per_dev[dev_num]++;
		return 1;
	}

	active_poll_sensors--;
	poll_sensors_per_dev[dev_num]--;
	return 1;
}


static int get_field_count (int s)
{
	int catalog_index = sensor_info[s].catalog_index;
	int sensor_type	  = sensor_catalog[catalog_index].type;

	switch (sensor_type) {
		case SENSOR_TYPE_ACCELEROMETER:		/* m/s^2	*/
		case SENSOR_TYPE_MAGNETIC_FIELD:	/* micro-tesla	*/
		case SENSOR_TYPE_ORIENTATION:		/* degrees	*/
		case SENSOR_TYPE_GYROSCOPE_UNCALIBRATED:
		case SENSOR_TYPE_GYROSCOPE:		/* radians/s	*/
			return 3;

		case SENSOR_TYPE_LIGHT:			/* SI lux units */
		case SENSOR_TYPE_AMBIENT_TEMPERATURE:	/* °C		*/
		case SENSOR_TYPE_TEMPERATURE:		/* °C		*/
		case SENSOR_TYPE_PROXIMITY:		/* centimeters	*/
		case SENSOR_TYPE_PRESSURE:		/* hecto-pascal */
		case SENSOR_TYPE_RELATIVE_HUMIDITY:	/* percent */
			return 1;

		case SENSOR_TYPE_ROTATION_VECTOR:
			return  4;

		default:
			ALOGE("Unknown sensor type!\n");
			return 0;			/* Drop sample */
	}
}


static void time_add(struct timespec *out, struct timespec *in, int64_t ns)
{
	int64_t target_ts = 1000000000LL * in->tv_sec + in->tv_nsec + ns;

	out->tv_sec = target_ts / 1000000000;
	out->tv_nsec = target_ts % 1000000000;
}


static void* acquisition_routine (void* param)
{
	/*
	 * Data acquisition routine run in a dedicated thread, covering a single
	 * sensor. This loop will periodically retrieve sampling data through
	 * sysfs, then package it as a sample and transfer it to our master poll
	 * loop through a report fd. Checks for a cancellation signal quite
	 * frequently, as the thread may be disposed of at any time. Note that
	 * Bionic does not provide pthread_cancel / pthread_testcancel...
	 */

	int s = (int) param;
	int num_fields;
	struct sensors_event_t data = {0};
	int c;
	int ret;
	struct timespec entry_time;
	struct timespec target_time;
	int64_t period;

	ALOGV("Entering data acquisition thread for sensor %d\n", s);

	if (s < 0 || s >= sensor_count) {
		ALOGE("Invalid sensor handle!\n");
		return NULL;
	}

	if (!sensor_info[s].sampling_rate) {
		ALOGE("Zero rate in acquisition routine for sensor %d\n", s);
		return NULL;
	}

	num_fields = get_field_count(s);

	/*
	 * Each condition variable is associated to a mutex that has to be
	 * locked by the thread that's waiting on it. We use these condition
	 * variables to get the acquisition threads out of sleep quickly after
	 * the sampling rate is adjusted, or the sensor is disabled.
	 */
	pthread_mutex_lock(&thread_release_mutex[s]);

	while (1) {
		/* Pinpoint the moment we start sampling */
		clock_gettime(CLOCK_REALTIME, &entry_time);

		ALOGV("Acquiring sample data for sensor %d through sysfs\n", s);

		/* Read values through sysfs */
		for (c=0; c<num_fields; c++) {
			data.data[c] = acquire_immediate_value(s, c);

			/* Check and honor termination requests */
			if (sensor_info[s].thread_data_fd[1] == -1)
				goto exit;

			ALOGV("\tfield %d: %f\n", c, data.data[c]);

		}

		/* If the sample looks good */
		if (sensor_info[s].ops.finalize(s, &data)) {

			/* Pipe it for transmission to poll loop */
			ret = write(	sensor_info[s].thread_data_fd[1],
					data.data,
					num_fields * sizeof(float));
		}

		/* Check and honor termination requests */
		if (sensor_info[s].thread_data_fd[1] == -1)
			goto exit;


		period = (int64_t) (1000000000.0/ sensor_info[s].sampling_rate);

		time_add(&target_time, &entry_time, period);

		/*
		 * Wait until the sampling time elapses, or a rate change is
		 * signaled, or a thread exit is requested.
		 */
		ret = pthread_cond_timedwait(	&thread_release_cond[s],
						&thread_release_mutex[s],
						&target_time);

		/* Check and honor termination requests */
		if (sensor_info[s].thread_data_fd[1] == -1)
				goto exit;
	}

exit:
	ALOGV("Acquisition thread for S%d exiting\n", s);
	pthread_mutex_unlock(&thread_release_mutex[s]);
	pthread_exit(0);
	return NULL;
}


static void start_acquisition_thread (int s)
{
	int incoming_data_fd;
	int ret;

	struct epoll_event ev = {0};

	ALOGV("Initializing acquisition context for sensor %d\n", s);

	/* Create condition variable and mutex for quick thread release */
	ret = pthread_cond_init(&thread_release_cond[s], NULL);
	ret = pthread_mutex_init(&thread_release_mutex[s], NULL);

	/* Create a pipe for inter thread communication */
	ret = pipe(sensor_info[s].thread_data_fd);

	incoming_data_fd = sensor_info[s].thread_data_fd[0];

	ev.events = EPOLLIN;
	ev.data.u32 = THREAD_REPORT_TAG_BASE + s;

	/* Add incoming side of pipe to our poll set, with a suitable tag */
	ret = epoll_ctl(poll_fd, EPOLL_CTL_ADD, incoming_data_fd , &ev);

	/* Create and start worker thread */
	ret = pthread_create(	&sensor_info[s].acquisition_thread,
				NULL,
				acquisition_routine,
				(void*) s);
}


static void stop_acquisition_thread (int s)
{
	int incoming_data_fd = sensor_info[s].thread_data_fd[0];
	int outgoing_data_fd = sensor_info[s].thread_data_fd[1];

	ALOGV("Tearing down acquisition context for sensor %d\n", s);

	/* Delete the incoming side of the pipe from our poll set */
	epoll_ctl(poll_fd, EPOLL_CTL_DEL, incoming_data_fd, NULL);

	/* Mark the pipe ends as invalid ; that's a cheap exit flag */
	sensor_info[s].thread_data_fd[0] = -1;
	sensor_info[s].thread_data_fd[1] = -1;

	/* Close both sides of our pipe */
	close(incoming_data_fd);
	close(outgoing_data_fd);

	/* Stop acquisition thread and clean up thread handle */
	pthread_cond_signal(&thread_release_cond[s]);
	pthread_join(sensor_info[s].acquisition_thread, NULL);

	/* Clean up our sensor descriptor */
	sensor_info[s].acquisition_thread = -1;

	/* Delete condition variable and mutex */
	pthread_cond_destroy(&thread_release_cond[s]);
	pthread_mutex_destroy(&thread_release_mutex[s]);
}


int sensor_activate(int s, int enabled)
{
	char device_name[PATH_MAX];
	struct epoll_event ev = {0};
	int dev_fd;
	int ret;
	int dev_num = sensor_info[s].dev_num;
	int is_poll_sensor = !sensor_info[s].num_channels;

	/* If we want to activate gyro calibrated and gyro uncalibrated is activated
	 * Deactivate gyro uncalibrated - Uncalibrated releases handler
	 * Activate gyro calibrated     - Calibrated has handler
	 * Reactivate gyro uncalibrated - Uncalibrated gets data from calibrated */

	/* If we want to deactivate gyro calibrated and gyro uncalibrated is active
	 * Deactivate gyro uncalibrated - Uncalibrated no longer gets data from handler
	 * Deactivate gyro calibrated   - Calibrated releases handler
	 * Reactivate gyro uncalibrated - Uncalibrated has handler */

	if (sensor_catalog[sensor_info[s].catalog_index].type == SENSOR_TYPE_GYROSCOPE &&
		sensor_info[s].pair_idx && sensor_info[sensor_info[s].pair_idx].enable_count != 0) {

				sensor_activate(sensor_info[s].pair_idx, 0);
				ret = sensor_activate(s, enabled);
				sensor_activate(sensor_info[s].pair_idx, 1);
				return ret;
	}

	ret = adjust_counters(s, enabled);

	/* If the operation was neutral in terms of state, we're done */
	if (ret <= 0)
		return ret;


	if (!is_poll_sensor) {

		/* Stop sampling */
		enable_buffer(dev_num, 0);
		setup_trigger(dev_num, "\n");

		/* If there's at least one sensor enabled on this iio device */
		if (trig_sensors_per_dev[dev_num]) {

			/* Start sampling */
			setup_trigger(dev_num, sensor_info[s].trigger_name);
			enable_buffer(dev_num, 1);
		}
	}

	/*
	 * Make sure we have a fd on the character device ; conversely, close
	 * the fd if no one is using associated sensors anymore. The assumption
	 * here is that the underlying driver will power on the relevant
	 * hardware block while someone holds a fd on the device.
	 */
	dev_fd = device_fd[dev_num];

	if (!enabled) {
		if (is_poll_sensor)
			stop_acquisition_thread(s);

		if (dev_fd != -1 && !poll_sensors_per_dev[dev_num] &&
			!trig_sensors_per_dev[dev_num]) {
				/*
				 * Stop watching this fd. This should be a no-op
				 * in case this fd was not in the poll set.
				 */
				epoll_ctl(poll_fd, EPOLL_CTL_DEL, dev_fd, NULL);

				close(dev_fd);
				device_fd[dev_num] = -1;
			}
		return 0;
	}

	if (dev_fd == -1) {
		/* First enabled sensor on this iio device */
		sprintf(device_name, DEV_FILE_PATH, dev_num);
		dev_fd = open(device_name, O_RDONLY | O_NONBLOCK);

		device_fd[dev_num] = dev_fd;

		if (dev_fd == -1) {
			ALOGE("Could not open fd on %s (%s)\n",
			      device_name, strerror(errno));
			adjust_counters(s, 0);
			return -1;
		}

		ALOGV("Opened %s: fd=%d\n", device_name, dev_fd);

		if (!is_poll_sensor) {

			/* Add this iio device fd to the set of watched fds */
			ev.events = EPOLLIN;
			ev.data.u32 = dev_num;

			ret = epoll_ctl(poll_fd, EPOLL_CTL_ADD, dev_fd, &ev);

			if (ret == -1) {
				ALOGE(	"Failed adding %d to poll set (%s)\n",
					dev_fd, strerror(errno));
				return -1;
			}

			/* Note: poll-mode fds are not readable */
		}
	}

	/* Ensure that on-change sensors send at least one event after enable */
	sensor_info[s].prev_val = -1;

	if (is_poll_sensor)
		start_acquisition_thread(s);

	return 0;
}


static int integrate_device_report(int dev_num)
{
	int len;
	int s,c;
	unsigned char buf[MAX_SENSOR_REPORT_SIZE] = { 0 };
	int sr_offset;
	unsigned char *target;
	unsigned char *source;
	int size;
	int64_t ts;

	/* There's an incoming report on the specified iio device char dev fd */

	if (dev_num < 0 || dev_num >= MAX_DEVICES) {
		ALOGE("Event reported on unexpected iio device %d\n", dev_num);
		return -1;
	}

	if (device_fd[dev_num] == -1) {
		ALOGE("Ignoring stale report on iio device %d\n", dev_num);
		return -1;
	}

	ts = get_timestamp();

	len = read(device_fd[dev_num], buf, MAX_SENSOR_REPORT_SIZE);

	if (len == -1) {
		ALOGE("Could not read report from iio device %d (%s)\n",
		      dev_num, strerror(errno));
		return -1;
	}

	ALOGV("Read %d bytes from iio device %d\n", len, dev_num);

	/* Map device report to sensor reports */

	for (s=0; s<MAX_SENSORS; s++)
		if (sensor_info[s].dev_num == dev_num &&
		    sensor_info[s].enable_count) {

			sr_offset = 0;

			/* Copy data from device to sensor report buffer */
			for (c=0; c<sensor_info[s].num_channels; c++) {

				target = sensor_info[s].report_buffer +
					sr_offset;

				source = buf + sensor_info[s].channel[c].offset;

				size = sensor_info[s].channel[c].size;

				memcpy(target, source, size);

				sr_offset += size;
			}

			ALOGV("Sensor %d report available (%d bytes)\n", s,
			      sr_offset);

			sensor_info[s].report_ts = ts;
			sensor_info[s].report_pending = 1;
			sensor_info[s].report_initialized = 1;
		}

	return 0;
}


static int propagate_sensor_report(int s, struct sensors_event_t  *data)
{
	/* There's a sensor report pending for this sensor ; transmit it */

	int catalog_index = sensor_info[s].catalog_index;
	int sensor_type	  = sensor_catalog[catalog_index].type;
	int num_fields	  = get_field_count(s);
	int c;
	unsigned char* current_sample;

	/* If there's nothing to return... we're done */
	if (!num_fields)
		return 0;


	/* Only return uncalibrated event if also gyro active */
	if (sensor_type == SENSOR_TYPE_GYROSCOPE_UNCALIBRATED &&
		sensor_info[sensor_info[s].pair_idx].enable_count != 0)
			return 0;

	memset(data, 0, sizeof(sensors_event_t));

	data->version	= sizeof(sensors_event_t);
	data->sensor	= s;
	data->type	= sensor_type;
	data->timestamp = sensor_info[s].report_ts;

	ALOGV("Sample on sensor %d (type %d):\n", s, sensor_type);

	current_sample = sensor_info[s].report_buffer;

	/* If this is a poll sensor */
	if (!sensor_info[s].num_channels) {
		/* Use the data provided by the acquisition thread */
		ALOGV("Reporting data from worker thread for S%d\n", s);
		memcpy(data->data, current_sample, num_fields * sizeof(float));
		return 1;
	}

	/* Convert the data into the expected Android-level format */
	for (c=0; c<num_fields; c++) {

		data->data[c] = sensor_info[s].ops.transform
							(s, c, current_sample);

		ALOGV("\tfield %d: %f\n", c, data->data[c]);
		current_sample += sensor_info[s].channel[c].size;
	}

	/*
	 * The finalize routine, in addition to its late sample processing duty,
	 * has the final say on whether or not the sample gets sent to Android.
	 */
	return sensor_info[s].ops.finalize(s, data);
}


static void synthetize_duplicate_samples (void)
{
	/*
	 * Some sensor types (ex: gyroscope) are defined as continuously firing
	 * by Android, despite the fact that we can be dealing with iio drivers
	 * that only report events for new samples. For these we generate
	 * reports periodically, duplicating the last data we got from the
	 * driver. This is not necessary for polling sensors.
	 */

	int s;
	int64_t current_ts;
	int64_t target_ts;
	int64_t period;

	for (s=0; s<sensor_count; s++) {

		/* Ignore disabled sensors */
		if (!sensor_info[s].enable_count)
			continue;

		/* If the sensor can generate duplicates, leave it alone */
		if (!(sensor_info[s].quirks & QUIRK_TERSE_DRIVER))
			continue;

		/* If we haven't seen a sample, there's nothing to duplicate */
		if (!sensor_info[s].report_initialized)
			continue;

		/* If a sample was recently buffered, leave it alone too */
		if (sensor_info[s].report_pending)
			continue;

		/* We also need a valid sampling rate to be configured */
		if (!sensor_info[s].sampling_rate)
			continue;

		period = (int64_t) (1000000000.0/ sensor_info[s].sampling_rate);

		current_ts = get_timestamp();
		target_ts = sensor_info[s].report_ts + period;

		if (target_ts <= current_ts) {
			/* Mark the sensor for event generation */
			sensor_info[s].report_ts = current_ts;
			sensor_info[s].report_pending = 1;
		}
	}
}


static void integrate_thread_report (uint32_t tag)
{
	int s = tag - THREAD_REPORT_TAG_BASE;
	int len;
	int expected_len;

	expected_len = get_field_count(s) * sizeof(float);

	len = read(sensor_info[s].thread_data_fd[0],
		   sensor_info[s].report_buffer,
		   expected_len);

	if (len == expected_len) {
		sensor_info[s].report_ts = get_timestamp();
		sensor_info[s].report_pending = 1;
	}
}


static int get_poll_wait_timeout (void)
{
	/*
	 * Compute an appropriate timeout value, in ms, for the epoll_wait
	 * call that's going to await for iio device reports and incoming
	 * reports from our sensor sysfs data reader threads.
	 */

	int s;
	int64_t target_ts = INT64_MAX;
	int64_t ms_to_wait;
	int64_t period;

	/*
	 * Check if have have to deal with "terse" drivers that only send events
	 * when there is motion, despite the fact that the associated Android
	 * sensor type is continuous rather than on-change. In that case we have
	 * to duplicate events. Check deadline for the nearest upcoming event.
	 */
	for (s=0; s<sensor_count; s++)
		if (sensor_info[s].enable_count &&
		    (sensor_info[s].quirks & QUIRK_TERSE_DRIVER) &&
		    sensor_info[s].sampling_rate) {
			period = (int64_t) (1000000000.0 /
						sensor_info[s].sampling_rate);

			if (sensor_info[s].report_ts + period < target_ts)
				target_ts = sensor_info[s].report_ts + period;
		}

	/* If we don't have such a driver to deal with */
	if (target_ts == INT64_MAX)
		return -1; /* Infinite wait */

	ms_to_wait = (target_ts - get_timestamp()) / 1000000;

	/*
	 * If the target timestamp is very close or already behind us, wait
	 * nonetheless for a millisecond in order to a) avoid busy loops, and
	 * b) give a chance for the driver to report data before we repeat the
	 * last received sample.
	 */
	if (ms_to_wait <= 0)
		return 1;

	return ms_to_wait;
}


int sensor_poll(struct sensors_event_t* data, int count)
{
	int s;
	int i;
	int nfds;
	struct epoll_event ev[MAX_DEVICES];
	int returned_events;
	int event_count;

	/* Get one or more events from our collection of sensors */

return_available_sensor_reports:

	returned_events = 0;

	/* Check our sensor collection for available reports */
	for (s=0; s<sensor_count && returned_events < count; s++)
		if (sensor_info[s].report_pending) {
			event_count = 0;
			/* Lower flag */
			sensor_info[s].report_pending = 0;

			/* Report this event if it looks OK */
			event_count = propagate_sensor_report(s, &data[returned_events]);

			/* Duplicate only if both cal & uncal are active */
			if (sensor_catalog[sensor_info[s].catalog_index].type == SENSOR_TYPE_GYROSCOPE &&
					sensor_info[s].pair_idx && sensor_info[sensor_info[s].pair_idx].enable_count != 0) {
					struct gyro_cal* gyro_data = (struct gyro_cal*) sensor_info[s].cal_data;

					memcpy(&data[returned_events + event_count], &data[returned_events],
							sizeof(struct sensors_event_t) * event_count);
					for (i = 0; i < event_count; i++) {
						data[returned_events + i].type = SENSOR_TYPE_GYROSCOPE_UNCALIBRATED;
						data[returned_events + i].sensor = sensor_info[s].pair_idx;

						data[returned_events + i].data[0] = data[returned_events + i].data[0] + gyro_data->bias[0];
						data[returned_events + i].data[1] = data[returned_events + i].data[1] + gyro_data->bias[1];
						data[returned_events + i].data[2] = data[returned_events + i].data[2] + gyro_data->bias[2];

						data[returned_events + i].uncalibrated_gyro.bias[0] = gyro_data->bias[0];
						data[returned_events + i].uncalibrated_gyro.bias[1] = gyro_data->bias[1];
						data[returned_events + i].uncalibrated_gyro.bias[2] = gyro_data->bias[2];
					}
					event_count <<= 1;
			}
			sensor_info[sensor_info[s].pair_idx].report_pending = 0;
			returned_events += event_count;
			/*
			 * If the sample was deemed invalid or unreportable,
			 * e.g. had the same value as the previously reported
			 * value for a 'on change' sensor, silently drop it.
			 */
		}

	if (returned_events)
		return returned_events;

await_event:

	ALOGV("Awaiting sensor data\n");

	nfds = epoll_wait(poll_fd, ev, MAX_DEVICES, get_poll_wait_timeout());

	if (nfds == -1) {
		ALOGE("epoll_wait returned -1 (%s)\n", strerror(errno));
		goto await_event;
	}

	/* Synthetize duplicate samples if needed */
	synthetize_duplicate_samples();

	ALOGV("%d fds signalled\n", nfds);

	/* For each of the signalled sources */
	for (i=0; i<nfds; i++)
		if (ev[i].events == EPOLLIN)
			switch (ev[i].data.u32) {
				case 0 ... MAX_DEVICES-1:
					/* Read report from iio char dev fd */
					integrate_device_report(ev[i].data.u32);
					break;

				case THREAD_REPORT_TAG_BASE ...
				     THREAD_REPORT_TAG_BASE + MAX_SENSORS-1:
					/* Get report from acquisition thread */
					integrate_thread_report(ev[i].data.u32);
					break;

				default:
					ALOGW("Unexpected event source!\n");
					break;
			}

	goto return_available_sensor_reports;
}


int sensor_set_delay(int s, int64_t ns)
{
	/* Set the rate at which a specific sensor should report events */

	/* See Android sensors.h for indication on sensor trigger modes */

	char sysfs_path[PATH_MAX];
	char avail_sysfs_path[PATH_MAX];
	int dev_num		=	sensor_info[s].dev_num;
	int i			=	sensor_info[s].catalog_index;
	const char *prefix	=	sensor_catalog[i].tag;
	float new_sampling_rate; /* Granted sampling rate after arbitration   */
	float cur_sampling_rate; /* Currently used sampling rate	      */
	int per_sensor_sampling_rate;
	int per_device_sampling_rate;
	float max_supported_rate = 0;
	char freqs_buf[100];
	char* cursor;
	int n;
	float sr;

	if (!ns) {
		ALOGE("Rejecting zero delay request on sensor %d\n", s);
		return -EINVAL;
	}

	new_sampling_rate = 1000000000LL/ns;

	/*
	 * Artificially limit ourselves to 1 Hz or higher. This is mostly to
	 * avoid setting up the stage for divisions by zero.
	 */
	if (new_sampling_rate < 1)
		new_sampling_rate = 1;

	sensor_info[s].sampling_rate = new_sampling_rate;

	/* If we're dealing with a poll-mode sensor */
	if (!sensor_info[s].num_channels) {
		/* Interrupt current sleep so the new sampling gets used */
		pthread_cond_signal(&thread_release_cond[s]);
		return 0;
	}

	sprintf(sysfs_path, SENSOR_SAMPLING_PATH, dev_num, prefix);

	if (sysfs_read_float(sysfs_path, &cur_sampling_rate) != -1) {
		per_sensor_sampling_rate = 1;
		per_device_sampling_rate = 0;
	} else {
		per_sensor_sampling_rate = 0;

		sprintf(sysfs_path, DEVICE_SAMPLING_PATH, dev_num);

		if (sysfs_read_float(sysfs_path, &cur_sampling_rate) != -1)
			per_device_sampling_rate = 1;
		else
			per_device_sampling_rate = 0;
	}

	if (!per_sensor_sampling_rate && !per_device_sampling_rate) {
		ALOGE("No way to adjust sampling rate on sensor %d\n", s);
		return -ENOSYS;
	}

	/* Coordinate with others active sensors on the same device, if any */
	if (per_device_sampling_rate)
		for (n=0; n<sensor_count; n++)
			if (n != s && sensor_info[n].dev_num == dev_num &&
			    sensor_info[n].num_channels &&
			    sensor_info[n].enable_count &&
			    sensor_info[n].sampling_rate > new_sampling_rate)
				new_sampling_rate= sensor_info[n].sampling_rate;

	/* Check if we have contraints on allowed sampling rates */

	sprintf(avail_sysfs_path, DEVICE_AVAIL_FREQ_PATH, dev_num);

	if (sysfs_read_str(avail_sysfs_path, freqs_buf, sizeof(freqs_buf)) > 0){
		cursor = freqs_buf;

		/* Decode allowed sampling rates string, ex: "10 20 50 100" */

		/* While we're not at the end of the string */
		while (*cursor && cursor[0]) {

			/* Decode a single value */
			sr = strtod(cursor, NULL);

			if (sr > max_supported_rate)
				max_supported_rate = sr;

			/* If this matches the selected rate, we're happy */
			if (new_sampling_rate == sr)
				break;

			/*
			 * If we reached a higher value than the desired rate,
			 * adjust selected rate so it matches the first higher
			 * available one and stop parsing - this makes the
			 * assumption that rates are sorted by increasing value
			 * in the allowed frequencies string.
			 */
			if (sr > new_sampling_rate) {
				new_sampling_rate = sr;
				break;
			}

			/* Skip digits */
			while (cursor[0] && !isspace(cursor[0]))
				cursor++;

			/* Skip spaces */
			while (cursor[0] && isspace(cursor[0]))
					cursor++;
		}
	}


	if (max_supported_rate &&
		new_sampling_rate > max_supported_rate) {
		new_sampling_rate = max_supported_rate;
	}


	/* If the desired rate is already active we're all set */
	if (new_sampling_rate == cur_sampling_rate)
		return 0;

	ALOGI("Sensor %d sampling rate set to %g\n", s, new_sampling_rate);

	if (trig_sensors_per_dev[dev_num])
		enable_buffer(dev_num, 0);

	sysfs_write_float(sysfs_path, new_sampling_rate);

	if (trig_sensors_per_dev[dev_num])
		enable_buffer(dev_num, 1);

	return 0;
}


int allocate_control_data (void)
{
	int i;

	for (i=0; i<MAX_DEVICES; i++)
		device_fd[i] = -1;

	poll_fd = epoll_create(MAX_DEVICES);

	if (poll_fd == -1) {
		ALOGE("Can't create epoll instance for iio sensors!\n");
		return -1;
	}

	return poll_fd;
}


void delete_control_data (void)
{
}
