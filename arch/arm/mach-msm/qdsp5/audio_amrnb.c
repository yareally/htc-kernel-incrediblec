/* linux/arch/arm/mach-msm/qdsp5/audio_amrnb.c
 *
 * amrnb audio decoder device
 *
 * Copyright (c) 2008 QUALCOMM USA, INC.
 *
 * Based on the mp3 native driver in arch/arm/mach-msm/qdsp5/audio_mp3.c
 *
 * Copyright (C) 2008 Google, Inc.
 * Copyright (C) 2008 HTC Corporation
 *
 * All source code in this file is licensed under the following license except
 * where indicated.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can find it at http://www.fsf.org
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/dma-mapping.h>

#include <linux/delay.h>

#include <asm/atomic.h>
#include <asm/ioctls.h>
#include <mach/msm_adsp.h>
#include <linux/msm_audio.h>
#include "audmgr.h"

#include <mach/qdsp5/qdsp5audppcmdi.h>
#include <mach/qdsp5/qdsp5audppmsg.h>
#include <mach/qdsp5/qdsp5audplaycmdi.h>
#include <mach/qdsp5/qdsp5audplaymsg.h>

/* for queue ids - should be relative to module number*/
#include "adsp.h"

#define DEBUG
#ifdef DEBUG
#define dprintk(format, arg...) \
printk(KERN_DEBUG format, ## arg)
#else
#define dprintk(format, arg...) do {} while (0)
#endif

#define BUFSZ 1024 /* Hold minimum 700ms voice data */
#define DMASZ (BUFSZ * 2)

#define AUDPLAY_INVALID_READ_PTR_OFFSET	0xFFFF
#define AUDDEC_DEC_AMRNB 10

#define PCM_BUFSZ_MIN 1600 /* 100ms worth of data */
#define AMRNB_DECODED_FRSZ 320 /* AMR-NB 20ms 8KHz mono PCM size */
#define PCM_BUF_MAX_COUNT 5	/* DSP only accepts 5 buffers at most
				   but support 2 buffers currently */
#define ROUTING_MODE_FTRT 1
#define ROUTING_MODE_RT 2
/* Decoder status received from AUDPPTASK */
#define  AUDPP_DEC_STATUS_SLEEP	0
#define	 AUDPP_DEC_STATUS_INIT  1
#define  AUDPP_DEC_STATUS_CFG   2
#define  AUDPP_DEC_STATUS_PLAY  3

struct buffer {
	void *data;
	unsigned size;
	unsigned used;		/* Input usage actual DSP produced PCM size  */
	unsigned addr;
};

struct audio {
	struct buffer out[2];

	spinlock_t dsp_lock;

	uint8_t out_head;
	uint8_t out_tail;
	uint8_t out_needed;	/* number of buffers the dsp is waiting for */

	atomic_t out_bytes;

	struct mutex lock;
	struct mutex write_lock;
	wait_queue_head_t write_wait;

	/* Host PCM section */
	struct buffer in[PCM_BUF_MAX_COUNT];
	struct mutex read_lock;
	wait_queue_head_t read_wait;	/* Wait queue for read */
	char *read_data;	/* pointer to reader buffer */
	dma_addr_t read_phys;	/* physical address of reader buffer */
	uint8_t read_next;	/* index to input buffers to be read next */
	uint8_t fill_next;	/* index to buffer that DSP should be filling */
	uint8_t pcm_buf_count;	/* number of pcm buffer allocated */
	/* ---- End of Host PCM section */

	struct msm_adsp_module *audplay;

	struct audmgr audmgr;

	/* data allocated for various buffers */
	char *data;
	dma_addr_t phys;

	uint8_t opened:1;
	uint8_t enabled:1;
	uint8_t running:1;
	uint8_t stopped:1;	/* set when stopped, cleared on flush */
	uint8_t pcm_feedback:1;
	uint8_t buf_refresh:1;

	unsigned volume;

	uint16_t dec_id;
	uint32_t read_ptr_offset;
};

struct audpp_cmd_cfg_adec_params_amrnb {
   audpp_cmd_cfg_adec_params_common     common;
   unsigned short                       stereo_cfg;
} __attribute__((packed)) ;

static int auddec_dsp_config(struct audio *audio, int enable);
static void audpp_cmd_cfg_adec_params(struct audio *audio);
static void audpp_cmd_cfg_routing_mode(struct audio *audio);
static void audamrnb_send_data(struct audio *audio, unsigned needed);
static void audamrnb_config_hostpcm(struct audio *audio);
static void audamrnb_buffer_refresh(struct audio *audio);
static void audamrnb_dsp_event(void *private, unsigned id, uint16_t *msg);

/* must be called with audio->lock held */
static int audamrnb_enable(struct audio *audio)
{
	struct audmgr_config cfg;
	int rc;

	dprintk("audamrnb_enable()\n");

	if (audio->enabled)
		return 0;

	audio->out_tail = 0;
	audio->out_needed = 0;

	cfg.tx_rate = RPC_AUD_DEF_SAMPLE_RATE_NONE;
	cfg.rx_rate = RPC_AUD_DEF_SAMPLE_RATE_48000;
	cfg.def_method = RPC_AUD_DEF_METHOD_PLAYBACK;
	cfg.codec = RPC_AUD_DEF_CODEC_AMR_NB;
	cfg.snd_method = RPC_SND_METHOD_MIDI;

	rc = audmgr_enable(&audio->audmgr, &cfg);
	if (rc < 0)
		return rc;

	if (msm_adsp_enable(audio->audplay)) {
		pr_err("audio: msm_adsp_enable(audplay) failed\n");
		audmgr_disable(&audio->audmgr);
		return -ENODEV;
	}

	if (audpp_enable(audio->dec_id, audamrnb_dsp_event, audio)) {
		pr_err("audio: audpp_enable() failed\n");
		msm_adsp_disable(audio->audplay);
		audmgr_disable(&audio->audmgr);
		return -ENODEV;
	}
	audio->enabled = 1;
	return 0;
}

/* must be called with audio->lock held */
static int audamrnb_disable(struct audio *audio)
{
	dprintk("audamrnb_disable()\n");
	if (audio->enabled) {
		audio->enabled = 0;
		auddec_dsp_config(audio, 0);
		wake_up(&audio->write_wait);
		wake_up(&audio->read_wait);
		msm_adsp_disable(audio->audplay);
		audpp_disable(audio->dec_id, audio);
		audmgr_disable(&audio->audmgr);
		audio->out_needed = 0;
	}
	return 0;
}

/* ------------------- dsp --------------------- */
static void audamrnb_update_pcm_buf_entry(struct audio *audio,
		uint32_t *payload)
{
	uint8_t index;
	unsigned long flags;

	spin_lock_irqsave(&audio->dsp_lock, flags);
	for (index = 0; index < payload[1]; index++) {
		if (audio->in[audio->fill_next].addr ==
		    payload[2 + index * 2]) {
			dprintk("audamrnb_update_pcm_buf_entry: in[%d] ready\n",
				audio->fill_next);
			audio->in[audio->fill_next].used =
			    payload[3 + index * 2];
			if ((++audio->fill_next) == audio->pcm_buf_count)
				audio->fill_next = 0;

		} else {
			pr_err
			  ("audamrnb_update_pcm_buf_entry: expected=%x ret=%x\n"
			   , audio->in[audio->fill_next].addr,
			   payload[1 + index * 2]);
			break;
		}
	}
	if (audio->in[audio->fill_next].used == 0) {
		audamrnb_buffer_refresh(audio);
	} else {
		dprintk("audamrnb_update_pcm_buf_entry: read cannot keep up\n");
		audio->buf_refresh = 1;
	}

	spin_unlock_irqrestore(&audio->dsp_lock, flags);
	wake_up(&audio->read_wait);
}

static void audplay_dsp_event(void *data, unsigned id, size_t len,
			      void (*getevent) (void *ptr, size_t len))
{
	struct audio *audio = data;
	uint32_t msg[28];
	getevent(msg, sizeof(msg));

	dprintk("audplay_dsp_event: msg_id=%x\n", id);

	switch (id) {
	case AUDPLAY_MSG_DEC_NEEDS_DATA:
		audamrnb_send_data(audio, 1);
		break;

	case AUDPLAY_MSG_BUFFER_UPDATE:
		audamrnb_update_pcm_buf_entry(audio, msg);
		break;

	default:
		pr_err("unexpected message from decoder \n");
	}
}

static void audamrnb_dsp_event(void *private, unsigned id, uint16_t *msg)
{
	struct audio *audio = private;

	switch (id) {
	case AUDPP_MSG_STATUS_MSG:{
			unsigned status = msg[1];

			switch (status) {
			case AUDPP_DEC_STATUS_SLEEP:
				dprintk("decoder status: sleep \n");
				break;

			case AUDPP_DEC_STATUS_INIT:
				dprintk("decoder status: init \n");
				audpp_cmd_cfg_routing_mode(audio);
				break;

			case AUDPP_DEC_STATUS_CFG:
				dprintk("decoder status: cfg \n");
				break;
			case AUDPP_DEC_STATUS_PLAY:
				dprintk("decoder status: play \n");
				if (audio->pcm_feedback) {
					audamrnb_config_hostpcm(audio);
					audamrnb_buffer_refresh(audio);
				}
				break;
			default:
				pr_err("unknown decoder status \n");
				break;
			}
			break;
		}
	case AUDPP_MSG_CFG_MSG:
		if (msg[0] == AUDPP_MSG_ENA_ENA) {
			dprintk("audamrnb_dsp_event: CFG_MSG ENABLE\n");
			auddec_dsp_config(audio, 1);
			audio->out_needed = 0;
			audio->running = 1;
			audpp_set_volume_and_pan(audio->dec_id, audio->volume,
						 0);
			audpp_avsync(audio->dec_id, 22050);
		} else if (msg[0] == AUDPP_MSG_ENA_DIS) {
			dprintk("audamrnb_dsp_event: CFG_MSG DISABLE\n");
			audpp_avsync(audio->dec_id, 0);
			audio->running = 0;
		} else {
			pr_err("audamrnb_dsp_event: CFG_MSG %d?\n", msg[0]);
		}
		break;
	case AUDPP_MSG_ROUTING_ACK:
		dprintk("audamrnb_dsp_event: ROUTING_ACK mode=%d\n", msg[1]);
		audpp_cmd_cfg_adec_params(audio);
		break;

	default:
		pr_err("audamrnb_dsp_event: UNKNOWN (%d)\n", id);
	}

}

struct msm_adsp_ops audplay_adsp_ops_amrnb = {
	.event = audplay_dsp_event,
};

#define audplay_send_queue0(audio, cmd, len) \
	msm_adsp_write(audio->audplay, QDSP_uPAudPlay0BitStreamCtrlQueue, \
		       cmd, len)

static int auddec_dsp_config(struct audio *audio, int enable)
{
	audpp_cmd_cfg_dec_type cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_id = AUDPP_CMD_CFG_DEC_TYPE;
	if (enable)
		cmd.dec0_cfg = AUDPP_CMD_UPDATDE_CFG_DEC |
		    AUDPP_CMD_ENA_DEC_V | AUDDEC_DEC_AMRNB;
	else
		cmd.dec0_cfg = AUDPP_CMD_UPDATDE_CFG_DEC | AUDPP_CMD_DIS_DEC_V;

	return audpp_send_queue1(&cmd, sizeof(cmd));
}

static void audpp_cmd_cfg_adec_params(struct audio *audio)
{
	struct audpp_cmd_cfg_adec_params_amrnb cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.common.cmd_id = AUDPP_CMD_CFG_ADEC_PARAMS;
	cmd.common.length = AUDPP_CMD_CFG_ADEC_PARAMS_V13K_LEN;
	cmd.common.dec_id = audio->dec_id;
	cmd.common.input_sampling_frequency = 8000;
	cmd.stereo_cfg = AUDPP_CMD_PCM_INTF_MONO_V;

	audpp_send_queue2(&cmd, sizeof(cmd));
}

static void audpp_cmd_cfg_routing_mode(struct audio *audio)
{
	struct audpp_cmd_routing_mode cmd;
	dprintk("audpp_cmd_cfg_routing_mode()\n");
	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_id = AUDPP_CMD_ROUTING_MODE;
	cmd.object_number = audio->dec_id;
	if (audio->pcm_feedback)
		cmd.routing_mode = ROUTING_MODE_FTRT;
	else
		cmd.routing_mode = ROUTING_MODE_RT;

	audpp_send_queue1(&cmd, sizeof(cmd));
}

static int audplay_dsp_send_data_avail(struct audio *audio,
				       unsigned idx, unsigned len)
{
	audplay_cmd_bitstream_data_avail cmd;

	cmd.cmd_id = AUDPLAY_CMD_BITSTREAM_DATA_AVAIL;
	cmd.decoder_id = audio->dec_id;
	cmd.buf_ptr = audio->out[idx].addr;
	cmd.buf_size = len / 2;
	cmd.partition_number = 0;
	return audplay_send_queue0(audio, &cmd, sizeof(cmd));
}

static void audamrnb_buffer_refresh(struct audio *audio)
{
	struct audplay_cmd_buffer_refresh refresh_cmd;

	refresh_cmd.cmd_id = AUDPLAY_CMD_BUFFER_REFRESH;
	refresh_cmd.num_buffers = 1;
	refresh_cmd.buf0_address = audio->in[audio->fill_next].addr;
	refresh_cmd.buf0_length = audio->in[audio->fill_next].size -
	  (audio->in[audio->fill_next].size % AMRNB_DECODED_FRSZ);
	refresh_cmd.buf_read_count = 0;
	dprintk("audplay_buffer_fresh: buf0_addr=%x buf0_len=%d\n",
		refresh_cmd.buf0_address, refresh_cmd.buf0_length);
	(void)audplay_send_queue0(audio, &refresh_cmd, sizeof(refresh_cmd));
}

static void audamrnb_config_hostpcm(struct audio *audio)
{
	struct audplay_cmd_hpcm_buf_cfg cfg_cmd;

	dprintk("audamrnb_config_hostpcm()\n");
	cfg_cmd.cmd_id = AUDPLAY_CMD_HPCM_BUF_CFG;
	cfg_cmd.max_buffers = audio->pcm_buf_count;
	cfg_cmd.byte_swap = 0;
	cfg_cmd.hostpcm_config = (0x8000) | (0x4000);
	cfg_cmd.feedback_frequency = 1;
	cfg_cmd.partition_number = 0;
	(void)audplay_send_queue0(audio, &cfg_cmd, sizeof(cfg_cmd));

}

static void audamrnb_send_data(struct audio *audio, unsigned needed)
{
	struct buffer *frame;
	unsigned long flags;

	spin_lock_irqsave(&audio->dsp_lock, flags);
	if (!audio->running)
		goto done;

	if (needed) {
		/* We were called from the callback because the DSP
		 * requested more data.  Note that the DSP does want
		 * more data, and if a buffer was in-flight, mark it
		 * as available (since the DSP must now be done with
		 * it).
		 */
		audio->out_needed = 1;
		frame = audio->out + audio->out_tail;
		if (frame->used == 0xffffffff) {
			frame->used = 0;
			audio->out_tail ^= 1;
			wake_up(&audio->write_wait);
		}
	}

	if (audio->out_needed) {
		/* If the DSP currently wants data and we have a
		 * buffer available, we will send it and reset
		 * the needed flag.  We'll mark the buffer as in-flight
		 * so that it won't be recycled until the next buffer
		 * is requested
		 */

		frame = audio->out + audio->out_tail;
		if (frame->used) {
			BUG_ON(frame->used == 0xffffffff);
/*                      printk("frame %d busy\n", audio->out_tail); */
			audplay_dsp_send_data_avail(audio, audio->out_tail,
						    frame->used);
			frame->used = 0xffffffff;
			audio->out_needed = 0;
		}
	}
 done:
	spin_unlock_irqrestore(&audio->dsp_lock, flags);
}

/* ------------------- device --------------------- */

static void audamrnb_flush(struct audio *audio)
{
	audio->out[0].used = 0;
	audio->out[1].used = 0;
	audio->out_head = 0;
	audio->out_tail = 0;
	audio->stopped = 0;
	atomic_set(&audio->out_bytes, 0);
}

static void audamrnb_flush_pcm_buf(struct audio *audio)
{
	uint8_t index;

	for (index = 0; index < PCM_BUF_MAX_COUNT; index++)
		audio->in[index].used = 0;

	audio->read_next = 0;
	audio->fill_next = 0;
}

static long audamrnb_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	struct audio *audio = file->private_data;
	int rc = 0;

	dprintk("audamrnb_ioctl() cmd = %d\n", cmd);

	if (cmd == AUDIO_GET_STATS) {
		struct msm_audio_stats stats;
		stats.byte_count = audpp_avsync_byte_count(audio->dec_id);
		stats.sample_count = audpp_avsync_sample_count(audio->dec_id);
		if (copy_to_user((void *)arg, &stats, sizeof(stats)))
			return -EFAULT;
		return 0;
	}
	if (cmd == AUDIO_SET_VOLUME) {
		unsigned long flags;
		spin_lock_irqsave(&audio->dsp_lock, flags);
		audio->volume = arg;
		if (audio->running)
			audpp_set_volume_and_pan(audio->dec_id, arg, 0);
		spin_unlock_irqrestore(&audio->dsp_lock, flags);
    return 0;
	}
	mutex_lock(&audio->lock);
	switch (cmd) {
	case AUDIO_START:
		rc = audamrnb_enable(audio);
		break;
	case AUDIO_STOP:
		rc = audamrnb_disable(audio);
		audio->stopped = 1;
		break;
	case AUDIO_FLUSH:
		if (audio->stopped) {
			/* Make sure we're stopped and we wake any threads
			 * that might be blocked holding the write_lock.
			 * While audio->stopped write threads will always
			 * exit immediately.
			 */
			wake_up(&audio->write_wait);
			mutex_lock(&audio->write_lock);
			audamrnb_flush(audio);
			mutex_unlock(&audio->write_lock);
			wake_up(&audio->read_wait);
			mutex_lock(&audio->read_lock);
			audamrnb_flush_pcm_buf(audio);
			mutex_unlock(&audio->read_lock);
			break;
		}

  case AUDIO_SET_CONFIG:{
      dprintk("AUDIO_SET_CONFIG not applicable \n");
			break;
		}
	case AUDIO_GET_CONFIG:{
			struct msm_audio_config config;
			config.buffer_size = BUFSZ;
			config.buffer_count = 2;
			config.sample_rate = 8000;
			config.channel_count = 1;
			config.unused[0] = 0;
			config.unused[1] = 0;
			config.unused[2] = 0;
			if (copy_to_user((void *)arg, &config,
					 sizeof(config)))
				rc = -EFAULT;
			else
				rc = 0;

			break;
		}
	case AUDIO_GET_PCM_CONFIG:{
			struct msm_audio_pcm_config config;
			config.pcm_feedback = 0;
			config.buffer_count = PCM_BUF_MAX_COUNT;
			config.buffer_size = PCM_BUFSZ_MIN;
			if (copy_to_user((void *)arg, &config,
					 sizeof(config)))
				rc = -EFAULT;
			else
				rc = 0;
			break;
		}
	case AUDIO_SET_PCM_CONFIG:{
		struct msm_audio_pcm_config config;
		if (copy_from_user
		    (&config, (void *)arg, sizeof(config))) {
			rc = -EFAULT;
			break;
		}
		if ((config.buffer_count > PCM_BUF_MAX_COUNT) ||
		    (config.buffer_count == 1))
			config.buffer_count = PCM_BUF_MAX_COUNT;

		if (config.buffer_size < PCM_BUFSZ_MIN)
			config.buffer_size = PCM_BUFSZ_MIN;

			/* Check if pcm feedback is required */
		if ((config.pcm_feedback) && (!audio->read_data)) {
			dprintk("audamrnb_ioctl: allocate PCM buf %d\n",
					config.buffer_count *
					config.buffer_size);
			audio->read_data =
				dma_alloc_coherent(NULL,
						       config.buffer_size *
						       config.buffer_count,
						       &audio->read_phys,
						       GFP_KERNEL);
			if (!audio->read_data) {
				pr_err("audamrnb_ioctl: no mem for pcm buf\n");
				rc = -1;
			} else {
				uint8_t index;
				uint32_t offset = 0;
				audio->pcm_feedback = 1;
				audio->buf_refresh = 0;
				audio->pcm_buf_count =
					config.buffer_count;
				audio->read_next = 0;
				audio->fill_next = 0;

				for (index = 0;
				index < config.buffer_count; index++) {
					audio->in[index].data =
						audio->read_data + offset;
					audio->in[index].addr =
					    audio->read_phys + offset;
					audio->in[index].size =
					    config.buffer_size;
					audio->in[index].used = 0;
					offset += config.buffer_size;
				}
				rc = 0;
			}
		} else {
			rc = 0;
		}
		break;
	}
	default:
		rc = -EINVAL;
	}
	mutex_unlock(&audio->lock);
	return rc;
}

static ssize_t audamrnb_read(struct file *file, char __user *buf, size_t count,
			  loff_t *pos)
{
	struct audio *audio = file->private_data;
	const char __user *start = buf;
	int rc = 0;

	if (!audio->pcm_feedback)
		return 0; /* PCM feedback is not enabled. Nothing to read */

	mutex_lock(&audio->read_lock);
	dprintk("audamrnb_read() %d \n", count);
	while (count > 0) {
		rc = wait_event_interruptible(audio->read_wait,
					      (audio->in[audio->read_next].
					       used > 0) || (audio->stopped));

		if (rc < 0)
			break;

		if (audio->stopped) {
			rc = -EBUSY;
			break;
		}

		if (count < audio->in[audio->read_next].used) {
			/* Read must happen in frame boundary. Since driver does
			 * not know frame size, read count must be greater or
			 * equal to size of PCM samples
			 */
			dprintk("audamrnb_read:read stop - partial frame\n");
			break;
		} else {
			dprintk("audamrnb_read: read from in[%d]\n",
				audio->read_next);
			if (copy_to_user
			    (buf, audio->in[audio->read_next].data,
			     audio->in[audio->read_next].used)) {
				pr_err("audamrnb_read: invalid addr %x \n",
				       (unsigned int)buf);
				rc = -EFAULT;
				break;
			}
			count -= audio->in[audio->read_next].used;
			buf += audio->in[audio->read_next].used;
			audio->in[audio->read_next].used = 0;
			if ((++audio->read_next) == audio->pcm_buf_count)
				audio->read_next = 0;
		}
	}

	if (audio->buf_refresh) {
		audio->buf_refresh = 0;
		dprintk("audamrnb_read: kick start pcm feedback again\n");
		audamrnb_buffer_refresh(audio);
	}

	mutex_unlock(&audio->read_lock);

	if (buf > start)
		rc = buf - start;

	dprintk("audamrnb_read: read %d bytes\n", rc);
	return rc;
}

static ssize_t audamrnb_write(struct file *file, const char __user *buf,
			   size_t count, loff_t *pos)
{
	struct audio *audio = file->private_data;
	const char __user *start = buf;
	struct buffer *frame;
	size_t xfer;
	int rc = 0;

	if (count & 1)
		return -EINVAL;
	dprintk("audamrnb_write() \n");
	mutex_lock(&audio->write_lock);
	while (count > 0) {
		frame = audio->out + audio->out_head;
		rc = wait_event_interruptible(audio->write_wait,
					      (frame->used == 0)
					      || (audio->stopped));
		dprintk("audamrnb_write() buffer available\n");
		if (rc < 0)
			break;
		if (audio->stopped) {
			rc = -EBUSY;
			break;
		}
		xfer = (count > frame->size) ? frame->size : count;
		if (copy_from_user(frame->data, buf, xfer)) {
			rc = -EFAULT;
			break;
		}

		frame->used = xfer;
		audio->out_head ^= 1;
		count -= xfer;
		buf += xfer;

		audamrnb_send_data(audio, 0);

	}
	mutex_unlock(&audio->write_lock);
	if (buf > start)
		return buf - start;
	return rc;
}

static int audamrnb_release(struct inode *inode, struct file *file)
{
	struct audio *audio = file->private_data;

	dprintk("audamrnb_release()\n");

	mutex_lock(&audio->lock);
	audamrnb_disable(audio);
	audamrnb_flush(audio);
	audamrnb_flush_pcm_buf(audio);
	msm_adsp_put(audio->audplay);
	audio->audplay = NULL;
	audio->opened = 0;
	dma_free_coherent(NULL, DMASZ, audio->data, audio->phys);
	audio->data = NULL;
	if (audio->read_data != NULL) {
		dma_free_coherent(NULL,
				  audio->in[0].size * audio->pcm_buf_count,
				  audio->read_data, audio->read_phys);
		audio->read_data = NULL;
	}
	audio->pcm_feedback = 0;
	mutex_unlock(&audio->lock);
	return 0;
}

static struct audio the_amrnb_audio;

static int audamrnb_open(struct inode *inode, struct file *file)
{
	struct audio *audio = &the_amrnb_audio;
	int rc;

	mutex_lock(&audio->lock);

	if (audio->opened) {
		pr_err("audio: busy\n");
		rc = -EBUSY;
		goto done;
	}

	if (!audio->data) {
		audio->data = dma_alloc_coherent(NULL, DMASZ,
						 &audio->phys, GFP_KERNEL);
		if (!audio->data) {
			pr_err("audio: could not allocate DMA buffers\n");
			rc = -ENOMEM;
			goto done;
		}
	}

	rc = audmgr_open(&audio->audmgr);
	if (rc)
		goto done;

	rc = msm_adsp_get("AUDPLAY0TASK", &audio->audplay,
		&audplay_adsp_ops_amrnb, audio);
	if (rc) {
		pr_err("audio: failed to get audplay0 dsp module\n");
		audmgr_disable(&audio->audmgr);
		dma_free_coherent(NULL, DMASZ, audio->data, audio->phys);
		audio->data = NULL;
		goto done;
	}

	audio->dec_id = 0;

	audio->out[0].data = audio->data + 0;
	audio->out[0].addr = audio->phys + 0;
	audio->out[0].size = BUFSZ;

	audio->out[1].data = audio->data + BUFSZ;
	audio->out[1].addr = audio->phys + BUFSZ;
	audio->out[1].size = BUFSZ;

	audio->volume = 0x2000;	/* Q13 1.0 */

	audamrnb_flush(audio);

	file->private_data = audio;
	audio->opened = 1;
	rc = 0;
done:
	mutex_unlock(&audio->lock);
	return rc;
}

static struct file_operations audio_amrnb_fops = {
	.owner = THIS_MODULE,
	.open = audamrnb_open,
	.release = audamrnb_release,
	.read = audamrnb_read,
	.write = audamrnb_write,
	.unlocked_ioctl = audamrnb_ioctl,
};

struct miscdevice audio_amrnb_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "msm_amrnb",
	.fops = &audio_amrnb_fops,
};

static int __init audamrnb_init(void)
{
	mutex_init(&the_amrnb_audio.lock);
	mutex_init(&the_amrnb_audio.write_lock);
	mutex_init(&the_amrnb_audio.read_lock);
	spin_lock_init(&the_amrnb_audio.dsp_lock);
	init_waitqueue_head(&the_amrnb_audio.write_wait);
	init_waitqueue_head(&the_amrnb_audio.read_wait);
	the_amrnb_audio.read_data = NULL;
	return misc_register(&audio_amrnb_misc);
}

static void __exit audamrnb_exit(void)
{
	misc_deregister(&audio_amrnb_misc);
}

module_init(audamrnb_init);
module_exit(audamrnb_exit);

MODULE_DESCRIPTION("MSM AMR-NB driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("QUALCOMM Inc");
