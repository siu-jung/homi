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
homid_dev_xal_index(struct homid_dev *dev)
{
	bool expected = false;
	int err;

	// Set to true when indexing starts to ensure other threads do not start
	// indexing at the same time.
	if (atomic_compare_exchange_strong(&dev->homid_xal.indexed, &expected, true)) {
		err = xal_index(dev->homid_xal.xal);
		if (err) {
			homid_log(LOG_ERR, "xal_index(): %d", err);
			atomic_store(&dev->homid_xal.indexed, false);
			return err;
		}

		if (dev->homid_xal.watchstate == HOMID_DEV_XAL_WATCHSTATE_IDLE) {
			err = xal_watch_filesystem(dev->homid_xal.xal, on_xal_dirty, NULL);
			if (err) {
				homid_log(LOG_WARNING, "xal_watch_filesystem(): %d; filesystem watch unavailable", err);
			} else {
				dev->homid_xal.watchstate = HOMID_DEV_XAL_WATCHSTATE_WATCHING;
			}
		}
	}

	return 0;
}

/**
 * Setup xal for the homid_dev
 *
 * For the given homid_dev, open xal and retrieve extents through a full scan.
 * xnvme device must be initialized first.
 *
 * @param opts xal_opts parsed from config file.
 * @param device Output: device to setup.
 * @return 0 on success, negative errno on failure.
 */
static int
_xal_init(struct xal_opts *opts, struct homid_dev *dev)
{
	struct xal *xal;
	int err;

	if (!dev) {
		err = -EINVAL;
		homid_log(LOG_ERR, "No homid_dev for xal setup: %d", err);
		return err;
	}

	err = xal_open(dev->homid_xnvme.dev, &xal, opts);
	if (err) {
		homid_log(LOG_ERR, "xal_open(): %d", err);
		return err;
	}

	err = xal_dinodes_retrieve(xal);
	if (err) {
		homid_log(LOG_ERR, "xal_dinodes_retrieve(): %d", err);
		goto close_xal;
	}

	if (opts->watch_mode == XAL_WATCHMODE_DIRTY_DETECTION ||
	    opts->watch_mode == XAL_WATCHMODE_EXTENT_UPDATE) {
		dev->homid_xal.watchstate = HOMID_DEV_XAL_WATCHSTATE_IDLE;
	}

	dev->homid_xal.xal = xal;

	return 0;

close_xal:
	xal_close(xal);
	return err;
}

/**
 * Setup xnvme for the homid_dev
 *
 * For the given homid_dev, initialize xnvme.
 * Uses default xnvme_opts with "linux" as backend.
 *
 * @param uri URI of the device.
 * @param dev Output: device to setup.
 * @return 0 on success, negative errno on failure.
 */
static int
_xnvme_init(char *uri, struct homid_dev *dev)
{
	struct xnvme_opts opts = xnvme_opts_default();
	struct xnvme_dev *xnvme_dev;
	int err;

	opts.be = "linux";
	xnvme_dev = xnvme_dev_open(uri, &opts);
	if (!xnvme_dev) {
		err = -errno;
		homid_log(LOG_ERR, "xnvme_dev_open(): %d", err);
		return err;
	}

	dev->homid_xnvme.dev = xnvme_dev;
	return 0;
}

void
homid_dev_close(unsigned int ndevs, struct homid_dev *devices)
{
	if (!devices) {
		return;
	}

	for (unsigned int i = 0; i < ndevs; i++) {
		struct homid_dev *dev = &devices[i];

		if (!dev) {
			continue;
		}

		if (dev->homid_xal.watching) {
			xal_stop_watching_filesystem(dev->homid_xal.xal);
		}

		xal_close(dev->homid_xal.xal);

		xnvme_dev_close(dev->homid_xnvme.dev);
	}

	free(devices);
}

int
homid_dev_open(struct homid_opts *opts, struct homid_dev **devices)
{
	struct xal_opts *xal_opts = &opts->xal_opts;
	struct homid_dev *devs;
	unsigned int ndevs = opts->ndevs;
	int err;

	devs = calloc(ndevs, sizeof(struct homid_dev));
	if (!devs) {
		err = -errno;
		homid_log(LOG_ERR, "Failed to allocate devices: %d", err);
		return err;
	}

	for (unsigned int i = 0; i < ndevs; i++) {
		struct homid_dev *dev = &devs[i];
		char *uri = opts->dev_uris[i];

		strncpy(devs[i].uri, uri, sizeof(devs[i].uri) - 1);
		snprintf(dev->homid_xal.shm_name, sizeof(dev->homid_xal.shm_name), "/homid_dev%u", i);
		xal_opts->shm_name = dev->homid_xal.shm_name;

		err = _xnvme_init(uri, dev);
		if (err) {
			homid_log(LOG_ERR, "Failed to setup xNVMe for %s: %d", uri, err);
			goto failed;
		}

		err = _xal_init(xal_opts, dev);
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

struct homid_dev *
homid_dev_get(struct homid *homid, char *uri)
{
	struct homid_dev *found = NULL;

	for (unsigned int i = 0; i < homid->ndevs; i++) {
		if (!strcmp(homid->dev[i].uri, uri)) {
			found = &homid->dev[i];
			break;
		}
	}

	return found;
}
