/*
 * Copyright 2011-2012 Con Kolivas
 * Copyright 2011-2012 Luke Dashjr
 * Copyright 2010 Jeff Garzik
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "config.h"

#ifdef HAVE_CURSES
#include <curses.h>
#endif

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include <sys/types.h>

#ifndef WIN32
#include <sys/resource.h>
#endif
#include <ccan/opt/opt.h>

#include "compat.h"
#include "miner.h"
#include "driver-opencl.h"
#include "findnonce.h"
#include "ocl.h"
#include "adl.h"
#include "util.h"

/* TODO: cleanup externals ********************/

#ifdef HAVE_CURSES
extern WINDOW *mainwin, *statuswin, *logwin;
extern void enable_curses(void);
#endif

extern int mining_threads;
extern double total_secs;
extern int opt_g_threads;
extern bool opt_loginput;
extern char *opt_kernel_path;
extern int gpur_thr_id;
extern bool opt_noadl;

extern void *miner_thread(void *userdata);
extern int dev_from_id(int thr_id);
extern void decay_time(double *f, double fadd);

/**********************************************/

#ifdef HAVE_ADL
extern float gpu_temp(int gpu);
extern int gpu_fanspeed(int gpu);
extern int gpu_fanpercent(int gpu);
#endif

void print_ndevs(int *ndevs)
{
	opt_log_output = true;
	opencl_drv.drv_detect(false);
	clear_adl(*ndevs);
	applog(LOG_INFO, "%i GPU devices max detected", *ndevs);
}

struct cgpu_info gpus[MAX_GPUDEVICES]; /* Maximum number apparently possible */
struct cgpu_info *cpus;

/* In dynamic mode, only the first thread of each device will be in use.
 * This potentially could start a thread that was stopped with the start-stop
 * options if one were to disable dynamic from the menu on a paused GPU */
void pause_dynamic_threads(int gpu)
{
	struct cgpu_info *cgpu = &gpus[gpu];
	int i;

	for (i = 1; i < cgpu->threads; i++) {
		struct thr_info *thr;

		thr = get_thread(i);
		if (!thr->pause && cgpu->dynamic) {
			applog(LOG_WARNING, "Disabling extra threads due to dynamic mode.");
			applog(LOG_WARNING, "Tune dynamic intensity with --gpu-dyninterval");
		}

		thr->pause = cgpu->dynamic;
		if (!cgpu->dynamic && cgpu->deven != DEV_DISABLED)
			cgsem_post(&thr->sem);
	}
}

#if defined(HAVE_CURSES)
void manage_gpu(void)
{
	struct thr_info *thr;
	int selected, gpu, i;
	char checkin[40];
	char input;

	if (!opt_g_threads) {
		applog(LOG_ERR, "opt_g_threads not set in manage_gpu()");
		return;
	}

	opt_loginput = true;
	immedok(logwin, true);
	clear_logwin();
retry: // TODO: refactor

	for (gpu = 0; gpu < nDevs; gpu++) {
		struct cgpu_info *cgpu = &gpus[gpu];
		double displayed_rolling, displayed_total;
		bool mhash_base = true;

		displayed_rolling = cgpu->rolling;
		displayed_total = cgpu->total_mhashes / total_secs;
		if (displayed_rolling < 1) {
			displayed_rolling *= 1000;
			displayed_total *= 1000;
			mhash_base = false;
		}

		wlog("GPU %d: %.1f / %.1f %sh/s | A:%d  R:%d  HW:%d  U:%.2f/m  I:%d  xI:%d  rI:%d\n",
			gpu, displayed_rolling, displayed_total, mhash_base ? "M" : "K",
			cgpu->accepted, cgpu->rejected, cgpu->hw_errors,
			cgpu->utility, cgpu->intensity, cgpu->xintensity, cgpu->rawintensity);
#ifdef HAVE_ADL
		if (gpus[gpu].has_adl) {
			int engineclock = 0, memclock = 0, activity = 0, fanspeed = 0, fanpercent = 0, powertune = 0;
			float temp = 0, vddc = 0;

			if (gpu_stats(gpu, &temp, &engineclock, &memclock, &vddc, &activity, &fanspeed, &fanpercent, &powertune)) {
				char logline[255];

				strcpy(logline, ""); // In case it has no data
				if (temp != -1)
					sprintf(logline, "%.1f C  ", temp);
				if (fanspeed != -1 || fanpercent != -1) {
					tailsprintf(logline, sizeof(logline), "F: ");
					if (fanpercent != -1)
						tailsprintf(logline, sizeof(logline), "%d%% ", fanpercent);
					if (fanspeed != -1)
						tailsprintf(logline, sizeof(logline), "(%d RPM) ", fanspeed);
					tailsprintf(logline, sizeof(logline), " ");
				}
				if (engineclock != -1)
					tailsprintf(logline, sizeof(logline), "E: %d MHz  ", engineclock);
				if (memclock != -1)
					tailsprintf(logline, sizeof(logline), "M: %d Mhz  ", memclock);
				if (vddc != -1)
					tailsprintf(logline, sizeof(logline), "V: %.3fV  ", vddc);
				if (activity != -1)
					tailsprintf(logline, sizeof(logline), "A: %d%%  ", activity);
				if (powertune != -1)
					tailsprintf(logline, sizeof(logline), "P: %d%%", powertune);
				tailsprintf(logline, sizeof(logline), "\n");
				_wlog(logline);
			}
		}
#endif
		wlog("Last initialised: %s\n", cgpu->init);
		for (i = 0; i < mining_threads; i++) {
			thr = get_thread(i);
			if (thr->cgpu != cgpu)
				continue;
			get_datestamp(checkin, sizeof(checkin), &thr->last);
			displayed_rolling = thr->rolling;
			if (!mhash_base)
				displayed_rolling *= 1000;
			wlog("Thread %d: %.1f %sh/s %s ", i, displayed_rolling, mhash_base ? "M" : "K" , cgpu->deven != DEV_DISABLED ? "Enabled" : "Disabled");
			switch (cgpu->status) {
				default:
				case LIFE_WELL:
					wlog("ALIVE");
					break;
				case LIFE_SICK:
					wlog("SICK reported in %s", checkin);
					break;
				case LIFE_DEAD:
					wlog("DEAD reported in %s", checkin);
					break;
				case LIFE_INIT:
				case LIFE_NOSTART:
					wlog("Never started");
					break;
			}
			if (thr->pause)
				wlog(" paused");
			wlog("\n");
		}
		wlog("\n");
	}

	wlogprint("[E]nable  [D]isable  [R]estart GPU  %s\n",adl_active ? "[C]hange settings" : "");
	wlogprint("[I]ntensity  E[x]perimental intensity  R[a]w Intensity\n");

	wlogprint("Or press any other key to continue\n");
	logwin_update();
	input = getch();

	if (nDevs == 1)
		selected = 0;
	else
		selected = -1;
	if (!strncasecmp(&input, "e", 1)) {
		struct cgpu_info *cgpu;

		if (selected)
			selected = curses_int("Select GPU to enable");
		if (selected < 0 || selected >= nDevs) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		if (gpus[selected].deven != DEV_DISABLED) {
			wlogprint("Device already enabled\n");
			goto retry;
		}
		gpus[selected].deven = DEV_ENABLED;
		for (i = 0; i < mining_threads; ++i) {
			thr = get_thread(i);
			cgpu = thr->cgpu;
			if (cgpu->drv->drv_id != DRIVER_opencl)
				continue;
			if (dev_from_id(i) != selected)
				continue;
			if (cgpu->status != LIFE_WELL) {
				wlogprint("Must restart device before enabling it");
				goto retry;
			}
			applog(LOG_DEBUG, "Pushing sem post to thread %d", thr->id);

			cgsem_post(&thr->sem);
		}
		goto retry;
	} if (!strncasecmp(&input, "d", 1)) {
		if (selected)
			selected = curses_int("Select GPU to disable");
		if (selected < 0 || selected >= nDevs) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		if (gpus[selected].deven == DEV_DISABLED) {
			wlogprint("Device already disabled\n");
			goto retry;
		}
		gpus[selected].deven = DEV_DISABLED;
		goto retry;
	} else if (!strncasecmp(&input, "i", 1)) {
		int intensity;
		char *intvar;

		if (selected)
			selected = curses_int("Select GPU to change intensity on");
		if (selected < 0 || selected >= nDevs) {
			wlogprint("Invalid selection\n");
			goto retry;
		}

		intvar = curses_input("Set GPU scan intensity (d or "
							  MIN_INTENSITY_STR " -> "
							  MAX_INTENSITY_STR ")");
		if (!intvar) {
			wlogprint("Invalid input\n");
			goto retry;
		}
		if (!strncasecmp(intvar, "d", 1)) {
			wlogprint("Dynamic mode enabled on gpu %d\n", selected);
			gpus[selected].dynamic = true;
			pause_dynamic_threads(selected);
			free(intvar);
			goto retry;
		}
		intensity = atoi(intvar);
		free(intvar);
		if (intensity < MIN_INTENSITY || intensity > MAX_INTENSITY) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		gpus[selected].dynamic = false;
		gpus[selected].intensity = intensity;
		gpus[selected].xintensity = 0; // Disable xintensity when enabling intensity
		gpus[selected].rawintensity = 0; // Disable raw intensity when enabling intensity
		wlogprint("Intensity on gpu %d set to %d\n", selected, intensity);
		pause_dynamic_threads(selected);
		goto retry;
	} else if (!strncasecmp(&input, "x", 1)) {
		int xintensity;
		char *intvar;

		if (selected)
			selected = curses_int("Select GPU to change experimental intensity on");
		if (selected < 0 || selected >= nDevs) {
			wlogprint("Invalid selection\n");
			goto retry;
		}

		intvar = curses_input("Set experimental GPU scan intensity (" MIN_XINTENSITY_STR " -> " MAX_XINTENSITY_STR ")");
		if (!intvar) {
			wlogprint("Invalid input\n");
			goto retry;
		}
		xintensity = atoi(intvar);
		free(intvar);
		if (xintensity < MIN_XINTENSITY || xintensity > MAX_XINTENSITY) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		gpus[selected].dynamic = false;
		gpus[selected].intensity = 0; // Disable intensity when enabling xintensity
		gpus[selected].rawintensity = 0; // Disable raw intensity when enabling xintensity
		gpus[selected].xintensity = xintensity;
		wlogprint("Experimental intensity on gpu %d set to %d\n", selected, xintensity);
		pause_dynamic_threads(selected);
		goto retry;
	} else if (!strncasecmp(&input, "a", 1)) {
		int rawintensity;
		char *intvar;

		if (selected)
		  selected = curses_int("Select GPU to change raw intensity on");
		if (selected < 0 || selected >= nDevs) {
		  wlogprint("Invalid selection\n");
		  goto retry;
		}

		intvar = curses_input("Set raw GPU scan intensity (" MIN_RAWINTENSITY_STR " -> " MAX_RAWINTENSITY_STR ")");
		if (!intvar) {
		  wlogprint("Invalid input\n");
		  goto retry;
		}
		rawintensity = atoi(intvar);
		free(intvar);
		if (rawintensity < MIN_RAWINTENSITY || rawintensity > MAX_RAWINTENSITY) {
		  wlogprint("Invalid selection\n");
		  goto retry;
		}
		gpus[selected].dynamic = false;
		gpus[selected].intensity = 0; // Disable intensity when enabling raw intensity
		gpus[selected].xintensity = 0; // Disable xintensity when enabling raw intensity
		gpus[selected].rawintensity = rawintensity;
		wlogprint("Raw intensity on gpu %d set to %d\n", selected, rawintensity);
		pause_dynamic_threads(selected);
		goto retry;
	} else if (!strncasecmp(&input, "r", 1)) {
		if (selected)
			selected = curses_int("Select GPU to attempt to restart");
		if (selected < 0 || selected >= nDevs) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		wlogprint("Attempting to restart threads of GPU %d\n", selected);
		reinit_device(&gpus[selected]);
		goto retry;
	} else if (adl_active && (!strncasecmp(&input, "c", 1))) {
		if (selected)
			selected = curses_int("Select GPU to change settings on");
		if (selected < 0 || selected >= nDevs) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		change_gpusettings(selected);
		goto retry;
	} else
		clear_logwin();

	immedok(logwin, false);
	opt_loginput = false;
}
#else
void manage_gpu(void)
{
}
#endif

static _clState *clStates[MAX_GPUDEVICES];

#define CL_SET_BLKARG(blkvar) status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->blkvar)
#define CL_SET_ARG(var) status |= clSetKernelArg(*kernel, num++, sizeof(var), (void *)&var)
#define CL_SET_VARG(args, var) status |= clSetKernelArg(*kernel, num++, args * sizeof(uint), (void *)var)

static cl_int queue_kernel(_clState *clState, dev_blk_ctx *blk, __maybe_unused cl_uint threads)
{
	unsigned char *midstate = blk->work->midstate;
	cl_kernel *kernel = &clState->kernel;
	unsigned int num = 0;
	cl_uint le_target;
	cl_int status = 0;

	le_target = *(cl_uint *)(blk->work->device_target + 28);
	clState->cldata = blk->work->data;
	status = clEnqueueWriteBuffer(clState->commandQueue, clState->CLbuffer0, true, 0, 80, clState->cldata, 0, NULL,NULL);

	CL_SET_ARG(clState->CLbuffer0);
	CL_SET_ARG(clState->outputBuffer);
	CL_SET_ARG(clState->padbuffer8);
	CL_SET_VARG(4, &midstate[0]);
	CL_SET_VARG(4, &midstate[16]);
	CL_SET_ARG(le_target);

	return status;
}

static void set_threads_hashes(unsigned int vectors, unsigned int compute_shaders, int64_t *hashes, size_t *globalThreads,
			       unsigned int minthreads, __maybe_unused int *intensity, __maybe_unused int *xintensity, __maybe_unused int *rawintensity)
{
	unsigned int threads = 0;
	while (threads < minthreads) {
		if (*rawintensity > 0) {
			threads = *rawintensity;
		} else if (*xintensity > 0) {
			threads = compute_shaders * *xintensity;
		} else {
			threads = 1 << *intensity;
		}
		if (threads < minthreads) {
			if (likely(*intensity < MAX_INTENSITY))
				(*intensity)++;
			else
				threads = minthreads;
		}
	}

	*globalThreads = threads;
	*hashes = threads * vectors;
}

/* We have only one thread that ever re-initialises GPUs, thus if any GPU
 * init command fails due to a completely wedged GPU, the thread will never
 * return, unable to harm other GPUs. If it does return, it means we only had
 * a soft failure and then the reinit_gpu thread is ready to tackle another
 * GPU */
void *reinit_gpu(void *userdata)
{
	struct thr_info *mythr = (struct thr_info *)userdata;
	struct cgpu_info *cgpu;
	struct thr_info *thr;
	struct timeval now;
	char name[256];
	int thr_id;
	int gpu;

	pthread_detach(pthread_self());

select_cgpu:
	cgpu = (struct cgpu_info *)tq_pop(mythr->q, NULL);
	if (!cgpu)
		goto out;

	if (clDevicesNum() != nDevs) {
		applog(LOG_WARNING, "Hardware not reporting same number of active devices, will not attempt to restart GPU");
		goto out;
	}

	gpu = cgpu->device_id;

	for (thr_id = 0; thr_id < mining_threads; ++thr_id) {
		thr = get_thread(thr_id);
		cgpu = thr->cgpu;
		if (cgpu->drv->drv_id != DRIVER_opencl)
			continue;
		if (dev_from_id(thr_id) != gpu)
			continue;

		thr = get_thread(thr_id);
		if (!thr) {
			applog(LOG_WARNING, "No reference to thread %d exists", thr_id);
			continue;
		}

		thr->rolling = thr->cgpu->rolling = 0;
		/* Reports the last time we tried to revive a sick GPU */
		cgtime(&thr->sick);
		if (!pthread_cancel(thr->pth)) {
			applog(LOG_WARNING, "Thread %d still exists, killing it off", thr_id);
			pthread_join(thr->pth, NULL);
			thr->cgpu->drv->thread_shutdown(thr);
		} else
			applog(LOG_WARNING, "Thread %d no longer exists", thr_id);
	}

	for (thr_id = 0; thr_id < mining_threads; ++thr_id) {
		int virtual_gpu;

		thr = get_thread(thr_id);
		cgpu = thr->cgpu;
		if (cgpu->drv->drv_id != DRIVER_opencl)
			continue;
		if (dev_from_id(thr_id) != gpu)
			continue;

		virtual_gpu = cgpu->virtual_gpu;
		/* Lose this ram cause we may get stuck here! */
		//tq_freeze(thr->q);

		thr->q = tq_new();
		if (!thr->q)
			quit(1, "Failed to tq_new in reinit_gpu");

		/* Lose this ram cause we may dereference in the dying thread! */
		//free(clState);

		applog(LOG_INFO, "Reinit GPU thread %d", thr_id);
		clStates[thr_id] = initCl(virtual_gpu, name, sizeof(name), &cgpu->algorithm);
		if (!clStates[thr_id]) {
			applog(LOG_ERR, "Failed to reinit GPU thread %d", thr_id);
			goto select_cgpu;
		}
		applog(LOG_INFO, "initCl() finished. Found %s", name);

		if (unlikely(thr_info_create(thr, NULL, miner_thread, thr))) {
			applog(LOG_ERR, "thread %d create failed", thr_id);
			return NULL;
		}
		applog(LOG_WARNING, "Thread %d restarted", thr_id);
	}

	cgtime(&now);
	get_datestamp(cgpu->init, sizeof(cgpu->init), &now);

	for (thr_id = 0; thr_id < mining_threads; ++thr_id) {
		thr = get_thread(thr_id);
		cgpu = thr->cgpu;
		if (cgpu->drv->drv_id != DRIVER_opencl)
			continue;
		if (dev_from_id(thr_id) != gpu)
			continue;

		cgsem_post(&thr->sem);
	}

	goto select_cgpu;
out:
	return NULL;
}

static void opencl_detect(bool hotplug)
{
	int i;

	nDevs = clDevicesNum();
	if (nDevs < 0) {
		applog(LOG_ERR, "clDevicesNum returned error, no GPUs usable");
		nDevs = 0;
	}

	if (!nDevs)
		return;

	/* If opt_g_threads is not set, use default 1 thread */
	if (opt_g_threads == -1)
		opt_g_threads = 1;

	opencl_drv.max_diff = 65536;

	for (i = 0; i < nDevs; ++i) {
		struct cgpu_info *cgpu;

		cgpu = &gpus[i];
		cgpu->deven = DEV_ENABLED;
		cgpu->drv = &opencl_drv;
		cgpu->device_id = i;
#ifndef HAVE_ADL
		cgpu->threads = opt_g_threads;
#else
		if (cgpu->threads < 1)
			cgpu->threads = 1;
#endif
		cgpu->virtual_gpu = i;
		cgpu->algorithm = *opt_algorithm;
		add_cgpu(cgpu);
	}

	if (!opt_noadl)
		init_adl(nDevs);
}

static void reinit_opencl_device(struct cgpu_info *gpu)
{
	tq_push(control_thr[gpur_thr_id].q, gpu);
}

#ifdef HAVE_ADL
static void get_opencl_statline_before(char *buf, size_t bufsiz, struct cgpu_info *gpu)
{
	if (gpu->has_adl) {
		int gpuid = gpu->device_id;
		float gt = gpu_temp(gpuid);
		int gf = gpu_fanspeed(gpuid);
		int gp;

		if (gt != -1)
			tailsprintf(buf, bufsiz, "%5.1fC ", gt);
		else
			tailsprintf(buf, bufsiz, "       ");
		if (gf != -1)
			// show invalid as 9999
			tailsprintf(buf, bufsiz, "%4dRPM ", gf > 9999 ? 9999 : gf);
		else if ((gp = gpu_fanpercent(gpuid)) != -1)
			tailsprintf(buf, bufsiz, "%3d%%    ", gp);
		else
			tailsprintf(buf, bufsiz, "        ");
		tailsprintf(buf, bufsiz, "| ");
	} else
		gpu->drv->get_statline_before = &blank_get_statline_before;
}
#endif

static void get_opencl_statline(char *buf, size_t bufsiz, struct cgpu_info *gpu)
{
	if (gpu->rawintensity > 0)
		tailsprintf(buf, bufsiz, " rI:%3d", gpu->rawintensity);
	else if (gpu->xintensity > 0)
		tailsprintf(buf, bufsiz, " xI:%3d", gpu->xintensity);
	else
		tailsprintf(buf, bufsiz, " I:%2d", gpu->intensity);
}

struct opencl_thread_data {
	cl_int (*queue_kernel_parameters)(_clState *, dev_blk_ctx *, cl_uint);
	uint32_t *res;
};

static uint32_t *blank_res;

static bool opencl_thread_prepare(struct thr_info *thr)
{
	char name[256];
	struct timeval now;
	struct cgpu_info *cgpu = thr->cgpu;
	int gpu = cgpu->device_id;
	int virtual_gpu = cgpu->virtual_gpu;
	int i = thr->id;
	static bool failmessage = false;
	int buffersize = BUFFERSIZE;

	if (!blank_res)
		blank_res = (uint32_t *)calloc(buffersize, 1);
	if (!blank_res) {
		applog(LOG_ERR, "Failed to calloc in opencl_thread_init");
		return false;
	}

	strcpy(name, "");
	applog(LOG_INFO, "Init GPU thread %i GPU %i virtual GPU %i", i, gpu, virtual_gpu);
	clStates[i] = initCl(virtual_gpu, name, sizeof(name), &cgpu->algorithm);
	if (!clStates[i]) {
#ifdef HAVE_CURSES
		if (use_curses)
			enable_curses();
#endif
		applog(LOG_ERR, "Failed to init GPU thread %d, disabling device %d", i, gpu);
		if (!failmessage) {
			applog(LOG_ERR, "Restarting the GPU from the menu will not fix this.");
			applog(LOG_ERR, "Re-check your configuration and try restarting.");
			failmessage = true;
#ifdef HAVE_CURSES
			char *buf;
			if (use_curses) {
				buf = curses_input("Press enter to continue");
				if (buf)
					free(buf);
			}
#endif
		}
		cgpu->deven = DEV_DISABLED;
		cgpu->status = LIFE_NOSTART;

		dev_error(cgpu, REASON_DEV_NOSTART);

		return false;
	}
	if (!cgpu->name)
		cgpu->name = strdup(name);
	if (!cgpu->kernelname)
		cgpu->kernelname = strdup("ckolivas");

	applog(LOG_INFO, "initCl() finished. Found %s", name);
	cgtime(&now);
	get_datestamp(cgpu->init, sizeof(cgpu->init), &now);

	return true;
}

static bool opencl_thread_init(struct thr_info *thr)
{
	const int thr_id = thr->id;
	struct cgpu_info *gpu = thr->cgpu;
	struct opencl_thread_data *thrdata;
	_clState *clState = clStates[thr_id];
	cl_int status = 0;
	thrdata = (struct opencl_thread_data *)calloc(1, sizeof(*thrdata));
	thr->cgpu_data = thrdata;
	int buffersize = BUFFERSIZE;

	if (!thrdata) {
		applog(LOG_ERR, "Failed to calloc in opencl_thread_init");
		return false;
	}

	thrdata->queue_kernel_parameters = &queue_kernel;

	thrdata->res = (uint32_t *)calloc(buffersize, 1);

	if (!thrdata->res) {
		free(thrdata);
		applog(LOG_ERR, "Failed to calloc in opencl_thread_init");
		return false;
	}

	status |= clEnqueueWriteBuffer(clState->commandQueue, clState->outputBuffer, CL_TRUE, 0,
				       buffersize, blank_res, 0, NULL, NULL);
	if (unlikely(status != CL_SUCCESS)) {
		applog(LOG_ERR, "Error: clEnqueueWriteBuffer failed.");
		return false;
	}

	gpu->status = LIFE_WELL;

	gpu->device_last_well = time(NULL);

	return true;
}


static bool opencl_prepare_work(struct thr_info __maybe_unused *thr, struct work *work)
{
	work->blk.work = work;
	return true;
}

extern int opt_dynamic_interval;

static int64_t opencl_scanhash(struct thr_info *thr, struct work *work,
				int64_t __maybe_unused max_nonce)
{
	const int thr_id = thr->id;
	struct opencl_thread_data *thrdata = (struct opencl_thread_data *)thr->cgpu_data;
	struct cgpu_info *gpu = thr->cgpu;
	_clState *clState = clStates[thr_id];
	const cl_kernel *kernel = &clState->kernel;
	const int dynamic_us = opt_dynamic_interval * 1000;

	cl_int status;
	size_t globalThreads[1];
	size_t localThreads[1] = { clState->wsize };
	int64_t hashes;
	int found = FOUND;
	int buffersize = BUFFERSIZE;

	/* Windows' timer resolution is only 15ms so oversample 5x */
	if (gpu->dynamic && (++gpu->intervals * dynamic_us) > 70000) {
		struct timeval tv_gpuend;
		double gpu_us;

		cgtime(&tv_gpuend);
		gpu_us = us_tdiff(&tv_gpuend, &gpu->tv_gpustart) / gpu->intervals;
		if (gpu_us > dynamic_us) {
			if (gpu->intensity > MIN_INTENSITY)
				--gpu->intensity;
		} else if (gpu_us < dynamic_us / 2) {
			if (gpu->intensity < MAX_INTENSITY)
				++gpu->intensity;
		}
		memcpy(&(gpu->tv_gpustart), &tv_gpuend, sizeof(struct timeval));
		gpu->intervals = 0;
	}

	set_threads_hashes(clState->vwidth, clState->compute_shaders, &hashes, globalThreads, localThreads[0],
			   &gpu->intensity, &gpu->xintensity, &gpu->rawintensity);
	if (hashes > gpu->max_hashes)
		gpu->max_hashes = hashes;

	status = thrdata->queue_kernel_parameters(clState, &work->blk, globalThreads[0]);
	if (unlikely(status != CL_SUCCESS)) {
		applog(LOG_ERR, "Error: clSetKernelArg of all params failed.");
		return -1;
	}

	if (clState->goffset) {
		size_t global_work_offset[1];

		global_work_offset[0] = work->blk.nonce;
		status = clEnqueueNDRangeKernel(clState->commandQueue, *kernel, 1, global_work_offset,
						globalThreads, localThreads, 0,  NULL, NULL);
	} else
		status = clEnqueueNDRangeKernel(clState->commandQueue, *kernel, 1, NULL,
						globalThreads, localThreads, 0,  NULL, NULL);
	if (unlikely(status != CL_SUCCESS)) {
		applog(LOG_ERR, "Error %d: Enqueueing kernel onto command queue. (clEnqueueNDRangeKernel)", status);
		return -1;
	}

	status = clEnqueueReadBuffer(clState->commandQueue, clState->outputBuffer, CL_FALSE, 0,
				     buffersize, thrdata->res, 0, NULL, NULL);
	if (unlikely(status != CL_SUCCESS)) {
		applog(LOG_ERR, "Error: clEnqueueReadBuffer failed error %d. (clEnqueueReadBuffer)", status);
		return -1;
	}

	/* The amount of work scanned can fluctuate when intensity changes
	 * and since we do this one cycle behind, we increment the work more
	 * than enough to prevent repeating work */
	work->blk.nonce += gpu->max_hashes;

	/* This finish flushes the readbuffer set with CL_FALSE in clEnqueueReadBuffer */
	clFinish(clState->commandQueue);

	/* FOUND entry is used as a counter to say how many nonces exist */
	if (thrdata->res[found]) {
		/* Clear the buffer again */
		status = clEnqueueWriteBuffer(clState->commandQueue, clState->outputBuffer, CL_FALSE, 0,
					      buffersize, blank_res, 0, NULL, NULL);
		if (unlikely(status != CL_SUCCESS)) {
			applog(LOG_ERR, "Error: clEnqueueWriteBuffer failed.");
			return -1;
		}
		applog(LOG_DEBUG, "GPU %d found something?", gpu->device_id);
		postcalc_hash_async(thr, work, thrdata->res);
		memset(thrdata->res, 0, buffersize);
		/* This finish flushes the writebuffer set with CL_FALSE in clEnqueueWriteBuffer */
		clFinish(clState->commandQueue);
	}

	return hashes;
}

// Cleanup OpenCL memory on the GPU
// Note: This function is not thread-safe (clStates modification not atomic)
static void opencl_thread_shutdown(struct thr_info *thr)
{
	const int thr_id = thr->id;
	_clState *clState = clStates[thr_id];
	clStates[thr_id] = NULL;

	if (clState) {
		clFinish(clState->commandQueue);
		clReleaseMemObject(clState->outputBuffer);
		clReleaseMemObject(clState->CLbuffer0);
		clReleaseMemObject(clState->padbuffer8);
		clReleaseKernel(clState->kernel);
		clReleaseProgram(clState->program);
		clReleaseCommandQueue(clState->commandQueue);
		clReleaseContext(clState->context);
		free(clState);
	}
}

struct device_drv opencl_drv = {
	/*.drv_id = */			DRIVER_opencl,
	/*.dname = */			"opencl",
	/*.name = */			"GPU",
	/*.drv_detect = */		opencl_detect,
	/*.reinit_device = */		reinit_opencl_device,
#ifdef HAVE_ADL
	/*.get_statline_before = */	get_opencl_statline_before,
#else
					NULL,
#endif
	/*.get_statline = */		get_opencl_statline,
	/*.api_data = */		NULL,
	/*.get_stats = */		NULL,
	/*.identify_device = */		NULL,
	/*.set_device = */		NULL,

	/*.thread_prepare = */		opencl_thread_prepare,
	/*.can_limit_work = */		NULL,
	/*.thread_init = */		opencl_thread_init,
	/*.prepare_work = */		opencl_prepare_work,
	/*.hash_work = */		NULL,
	/*.scanhash = */		opencl_scanhash,
	/*.scanwork = */		NULL,
	/*.queue_full = */		NULL,
	/*.flush_work = */		NULL,
	/*.update_work = */		NULL,
	/*.hw_error = */		NULL,
	/*.thread_shutdown = */		opencl_thread_shutdown,
	/*.thread_enable =*/		NULL,
					false,
					0,
					0
};
