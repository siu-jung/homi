#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include <libxal.h>
#include <libxnvme.h>

#include <homid.h>
#include <homid_log.h>
#include <homid_dev.h>
#include <homid_opts.h>

static void
on_xal_dirty(struct xal *xal, void *cb_args)
{
	int err;

	(void)cb_args;

	err = xal_index(xal);
	if (err) {
		homid_log(LOG_CRIT, "xal_index(): %d; pools are stale, daemon restart required", err);
	}
}

int
homid_dev_xal_index(struct homid_device *device)
{
	bool expected = false;
	int err;

	// Set to true when indexing starts to ensure other threads do not start
	// indexing at the same time.
	if (atomic_compare_exchange_strong(&device->indexed, &expected, true)) {
		err = xal_index(device->xal);
		if (err) {
			homid_log(LOG_ERR, "xal_index(): %d", err);
			atomic_store(&device->indexed, false);
			return err;
		}

		if (device->watchstate == HOMID_DEV_XAL_WATCHSTATE_IDLE) {
			err = xal_watch_filesystem(device->xal, on_xal_dirty, NULL);
			if (err) {
				homid_log(LOG_WARNING, "xal_watch_filesystem(): %d; filesystem watch unavailable", err);
			} else {
				device->watchstate = HOMID_DEV_XAL_WATCHSTATE_WATCHING;
			}
		}
	}

	return 0;
}

/**
 * Setup xal for the homid_device
 *
 * For the given homid_device, open xal and retrieve extents through a full scan.
 * xnvme device must be initialized first.
 *
 * @param opts xal_opts parsed from config file.
 * @param device Output: device to setup.
 * @return 0 on success, negative errno on failure.
 */
static int
_xal_init(struct xal_opts *opts, struct homid_device *device)
{
	struct xal *xal;
	int err;

	if (!device) {
		err = -EINVAL;
		homid_log(LOG_ERR, "No homid_device for xal setup: %d", err);
		return err;
	}

	err = xal_open(device->dev, &xal, opts);
	if (err) {
		homid_log(LOG_ERR, "xal_open(): %d", err);
		return err;
	}

	err = xal_dinodes_retrieve(xal);
	if (err) {
		homid_log(LOG_ERR, "xal_dinodes_retrieve(): %d", err);
		goto close_xal;
	}

	if (opts->watch_mode) {
		device->watchstate = HOMID_DEV_XAL_WATCHSTATE_IDLE;
	}

	device->xal = xal;

	return 0;

close_xal:
	xal_close(xal);
	return err;
}

/**
 * Setup xnvme for the homid_device
 *
 * For the given homid_device, initialize xnvme.
 * Uses default xnvme_opts with "linux" as backend.
 *
 * @param uri URI of the device.
 * @param device Output: device to setup.
 * @return 0 on success, negative errno on failure.
 */
static int
_xnvme_init(char *uri, struct homid_device *device)
{
	struct xnvme_opts opts = xnvme_opts_default();
	struct xnvme_dev *dev;
	int err;

	opts.be = "linux";
	dev = xnvme_dev_open(uri, &opts);
	if (!dev) {
		err = -errno;
		homid_log(LOG_ERR, "xnvme_dev_open(): %d", err);
		return err;
	}

	device->dev = dev;
	return 0;
}

void
homid_dev_close(unsigned int ndevs, struct homid_device *devices)
{
	if (!devices) {
		return;
	}

	for (unsigned int i = 0; i < ndevs; i++) {
		struct homid_device *dev = &devices[i];

		if (!dev) {
			continue;
		}

		if (dev->watching) {
			xal_stop_watching_filesystem(dev->xal);
		}

		xal_close(dev->xal);

		xnvme_dev_close(dev->dev);
	}

	free(devices);
}

int
homid_dev_open(struct homid_opts *opts, struct homid_device **devices)
{
	struct xal_opts *xal_opts = &opts->xal_opts;
	struct homid_device *devs;
	unsigned int ndevs = opts->ndevs;
	int err;

	devs = calloc(ndevs, sizeof(struct homid_device));
	if (!devs) {
		err = -errno;
		homid_log(LOG_ERR, "Failed to allocate devices: %d", err);
		return err;
	}

	for (unsigned int i = 0; i < ndevs; i++) {
		char *uri = opts->dev_uris[i];

		strncpy(devs[i].uri, uri, sizeof(devs[i].uri) - 1);
		snprintf(devs[i].shm_name, sizeof(devs[i].shm_name), "/homid_dev%u", i);
		xal_opts->shm_name = devs[i].shm_name;

		err = _xnvme_init(uri, &devs[i]);
		if (err) {
			homid_log(LOG_ERR, "Failed to setup xNVMe for %s: %d", uri, err);
			goto failed;
		}

		err = _xal_init(xal_opts, &devs[i]);
		if (err) {
			homid_log(LOG_ERR, "Failed to setup XAL for %s: %d", uri, err);
			goto failed;
		}
	}

	*devices = devs;
	return 0;

failed:
	homid_dev_close(ndevs, devs);
	return err;
}

struct homid_device *
homid_dev_get(struct homid *homid, char *uri)
{
	struct homid_device *found = NULL;

	for (unsigned int i = 0; i < homid->ndevs; i++) {
		if (!strcmp(homid->dev[i].uri, uri)) {
			found = &homid->dev[i];
			break;
		}
	}

	return found;
}
