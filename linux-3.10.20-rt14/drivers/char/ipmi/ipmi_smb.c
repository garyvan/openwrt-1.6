/*
 * ipmi_smb.c
 *
 * The interface to the IPMI driver for SMBus access to a SMBus
 * compliant device.
 *
 * Author: Intel Corporation
 *         Todd Davis <todd.c.davis@intel.com>
 *
 * Rewritten by Corey Minyard <minyard@acm.org> to support the
 * non-blocking I2C interface, add support for multi-part
 * transactions, add PEC support, and general clenaup.
 *
 * Copyright 2003 Intel Corporation
 * Copyright 2005 MontaVista Software
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; either version 2 of the License, or (at your
 *  option) any later version.
 *
 *
 *  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 *  OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 *  TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 *  USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * This file holds the "policy" for the interface to the SMB state
 * machine.  It does the configuration, handles timers and interrupts,
 * and drives the real SMB state machine.
 */

/*
 * TODO: Figure out how to use SMB alerts.  This will require a new
 * interface into the I2C driver, I believe.
 */

#include <linux/version.h>
#if defined(MODVERSIONS)
#include <linux/modversions.h>
#endif

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/timer.h>
#include <linux/errno.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/i2c.h>
#include <linux/ipmi_smi.h>
#include <linux/init.h>
#include <linux/dmi.h>

#define PFX "ipmi_smb: "

#define IPMI_GET_SYSTEM_INTERFACE_CAPABILITIES_CMD	0x57

#define	SMB_IPMI_REQUEST			2
#define	SMB_IPMI_MULTI_PART_REQUEST_START	6
#define	SMB_IPMI_MULTI_PART_REQUEST_MIDDLE	7
#define	SMB_IPMI_RESPONSE			3
#define	SMB_IPMI_MULTI_PART_RESPONSE_MIDDLE	9

/* smb_debug is a bit-field
 *	SMB_DEBUG_MSG -	commands and their responses
 *	SMB_DEBUG_STATES -	message states
 *	SMB_DEBUG_TIMING -	 Measure times between events in the driver
 */
#define SMB_DEBUG_TIMING	4
#define SMB_DEBUG_STATE		2
#define SMB_DEBUG_MSG		1
#define SMB_NODEBUG		0
#define SMB_DEFAULT_DEBUG	(SMB_NODEBUG)

/*
 * Timer values
 */
#define SMB_MSG_USEC		20000	/* 20ms between message tries. */
#define SMB_MSG_PART_USEC	5000	/* 5ms for a message part */

/* How many times to we retry sending/receiving the message. */
#define	SMB_SEND_RETRIES	5
#define	SMB_RECV_RETRIES	250

#define SMB_MSG_MSEC		(SMB_MSG_USEC / 1000)
#define SMB_MSG_JIFFIES		((SMB_MSG_USEC * 1000) / TICK_NSEC)
#define SMB_MSG_PART_JIFFIES	((SMB_MSG_PART_USEC * 1000) / TICK_NSEC)

enum smb_intf_state {
	SMB_NORMAL,
	SMB_GETTING_FLAGS,
	SMB_GETTING_EVENTS,
	SMB_CLEARING_FLAGS,
	SMB_GETTING_MESSAGES,
	/* FIXME - add watchdog stuff. */
};

#define SMB_IDLE(smb)	 ((smb)->smb_state == SMB_NORMAL \
			  && (smb)->curr_msg == NULL)

/*
 * Indexes into stats[] in smb_info below.
 */
enum smb_stat_indexes {
	/* Number of total messages sent. */
	SMB_STAT_sent_messages = 0,

	/*
	 * Number of message parts sent.  Messages may be broken into
	 * parts if they are long.
	 */
	SMB_STAT_sent_messages_parts,

	/*
	 * Number of time a message was retried.
	 */
	SMB_STAT_send_retries,

	/*
	 * Number of times the send of a message failed.
	 */
	SMB_STAT_send_errors,

	/*
	 * Number of message responses received.
	 */
	SMB_STAT_received_messages,

	/*
	 * Number of message fragments received.
	 */
	SMB_STAT_received_message_parts,

	/*
	 * Number of times the receive of a message was retried.
	 */
	SMB_STAT_receive_retries,

	/*
	 * Number of errors receiving messages.
	 */
	SMB_STAT_receive_errors,

	/*
	 * Number of times a flag fetch was requested.
	 */
	SMB_STAT_flag_fetches,

	/*
	 * Number of times the hardware didn't follow the state machine.
	 */
	SMB_STAT_hosed,

	/*
	 * Number of received events.
	 */
	SMB_STAT_events,

	/* Number of asyncronous messages received. */
	SMB_STAT_incoming_messages,

	/* Always add statistics before this value, it must be last. */
	SMB_NUM_STATS
};

struct smb_info {
	ipmi_smi_t          intf;
	spinlock_t          msg_lock;
	struct list_head    xmit_msgs;
	struct list_head    hp_xmit_msgs;
	struct ipmi_smi_msg *curr_msg;
	enum smb_intf_state smb_state;
	unsigned long       smb_debug;

	/*
	 * Flags from the last GET_MSG_FLAGS command, used when an ATTN
	 * is set to hold the flags until we are done handling everything
	 * from the flags.
	 */
#define RECEIVE_MSG_AVAIL	0x01
#define EVENT_MSG_BUFFER_FULL	0x02
#define WDT_PRE_TIMEOUT_INT	0x08
	unsigned char       msg_flags;

	char		    has_event_buffer;

	/*
	 * If set to true, this will request events the next time the
	 * state machine is idle.
	 */
	int                 req_events;

	/*
	 * If set to true, this will request flags the next time the
	 * state machine is idle.
	 */
	int                 req_flags;

	/*
	 * If true, run the state machine to completion on every send
	 * call.  Generally used after a panic or shutdown to make
	 * sure stuff goes out.
	 */
	int                 run_to_completion;

	/*
	 * Used to perform timer operations when run-to-completion
	 * mode is on.  This is a countdown timer.
	 */
	int                 rtc_us_timer;

	/* Used for sending/receiving data.  +1 for the length. */
	unsigned char data[IPMI_MAX_MSG_LENGTH + 1];
	unsigned int  data_len;

	/* Temp receive buffer, gets copied into data. */
	unsigned char recv[I2C_SMBUS_BLOCK_MAX];

	struct i2c_client *client;
	struct i2c_op_q_entry i2c_q_entry;

	/* From the device id response. */
	struct ipmi_device_id device_id;

	/* Is the driver trying to stop? */
	int stopping;

	struct timer_list retry_timer;
	int retries_left;

	/* Info from SSIF cmd */
	unsigned char max_xmit_msg_size;
	unsigned char max_recv_msg_size;
	unsigned int  multi_support;
	int           supports_pec;

#define SMB_NO_MULTI		0
#define SMB_MULTI_2_PART	1
#define SMB_MULTI_n_PART	2
	unsigned char *multi_data;
	unsigned int  multi_len;
	unsigned int  multi_pos;

	atomic_t stats[SMB_NUM_STATS];
};

#define smb_inc_stat(smb, stat) \
	atomic_inc(&(smb)->stats[SMB_STAT_ ## stat])
#define smb_get_stat(smb, stat) \
	((unsigned int) atomic_read(&(smb)->stats[SMB_STAT_ ## stat]))

static int initialized;
static int smb_dbg_probe;

static void return_hosed_msg(struct smb_info *smb_info,
			     struct ipmi_smi_msg *msg);
static void start_next_msg(struct smb_info *smb_info, unsigned long *flags);
static int start_send(struct smb_info *smb_info,
		      unsigned char   *data,
		      unsigned int    len);

/*
 * If run_to_completion mode is on, return NULL to know the lock wasn't
 * taken.  Otherwise lock info->lock and return the flags.
 */
static unsigned long *ipmi_smb_lock_cond(struct smb_info *smb_info,
					 unsigned long *flags)
{
	if (smb_info->run_to_completion)
		return NULL;
	spin_lock_irqsave(&smb_info->msg_lock, *flags);
	return flags;
}

static void ipmi_smb_unlock_cond(struct smb_info *smb_info,
				 unsigned long *flags)
{
	if (!flags)
		return;
	spin_unlock_irqrestore(&smb_info->msg_lock, *flags);
}

static void deliver_recv_msg(struct smb_info *smb_info,
			     struct ipmi_smi_msg *msg)
{
	ipmi_smi_t    intf = smb_info->intf;

	if (!intf || (msg->rsp_size < 0)) {
		return_hosed_msg(smb_info, msg);
		printk(KERN_ERR
		       "malformed message in deliver_recv_msg:"
		       " rsp_size = %d\n", msg->rsp_size);
		ipmi_free_smi_msg(msg);
	} else {
		ipmi_smi_msg_received(intf, msg);
	}
}

static void return_hosed_msg(struct smb_info *smb_info,
			     struct ipmi_smi_msg *msg)
{
	smb_inc_stat(smb_info, hosed);

	/* Make it a reponse */
	msg->rsp[0] = msg->data[0] | 4;
	msg->rsp[1] = msg->data[1];
	msg->rsp[2] = 0xFF; /* Unknown error. */
	msg->rsp_size = 3;

	deliver_recv_msg(smb_info, msg);
}

/*
 * Must be called with the message lock held.  This will release the
 * message lock.  Note that the caller will check SMB_IDLE and start a
 * new operation, so there is no need to check for new messages to
 * start in here.
 */
static void start_clear_flags(struct smb_info *smb_info, unsigned long *flags)
{
	unsigned char msg[3];

	smb_info->msg_flags &= ~WDT_PRE_TIMEOUT_INT;
	smb_info->smb_state = SMB_CLEARING_FLAGS;
	ipmi_smb_unlock_cond(smb_info, flags);

	/* Make sure the watchdog pre-timeout flag is not set at startup. */
	msg[0] = (IPMI_NETFN_APP_REQUEST << 2);
	msg[1] = IPMI_CLEAR_MSG_FLAGS_CMD;
	msg[2] = WDT_PRE_TIMEOUT_INT;

	if (start_send(smb_info, msg, 3) != 0) {
		/* Error, just go to normal state. */
		smb_info->smb_state = SMB_NORMAL;
	}
}

static void start_flag_fetch(struct smb_info *smb_info, unsigned long *flags)
{
	unsigned char mb[2];

	smb_info->req_flags = 0;
	smb_info->smb_state = SMB_GETTING_FLAGS;
	ipmi_smb_unlock_cond(smb_info, flags);

	mb[0] = (IPMI_NETFN_APP_REQUEST << 2);
	mb[1] = IPMI_GET_MSG_FLAGS_CMD;
	if (start_send(smb_info, mb, 2) != 0)
		smb_info->smb_state = SMB_NORMAL;
}

static void start_event_fetch(struct smb_info *smb_info, unsigned long *flags)
{
	struct ipmi_smi_msg *msg;

	smb_info->req_events = 0;

	msg = ipmi_alloc_smi_msg();
	if (!msg) {
		smb_info->smb_state = SMB_NORMAL;
		return;
	}

	smb_info->curr_msg = msg;
	smb_info->smb_state = SMB_GETTING_EVENTS;
	ipmi_smb_unlock_cond(smb_info, flags);

	msg->data[0] = (IPMI_NETFN_APP_REQUEST << 2);
	msg->data[1] = IPMI_READ_EVENT_MSG_BUFFER_CMD;
	msg->data_size = 2;

	if (start_send(smb_info, msg->data, msg->data_size) != 0) {
		unsigned long oflags;
		flags = ipmi_smb_lock_cond(smb_info, &oflags);
		smb_info->curr_msg = NULL;
		smb_info->smb_state = SMB_NORMAL;
		ipmi_smb_unlock_cond(smb_info, flags);
		ipmi_free_smi_msg(msg);
	}
}

static void start_recv_msg_fetch(struct smb_info *smb_info,
				 unsigned long *flags)
{
	struct ipmi_smi_msg *msg;

	msg = ipmi_alloc_smi_msg();
	if (!msg) {
		smb_info->smb_state = SMB_NORMAL;
		return;
	}

	smb_info->curr_msg = msg;
	smb_info->smb_state = SMB_GETTING_MESSAGES;
	ipmi_smb_unlock_cond(smb_info, flags);

	msg->data[0] = (IPMI_NETFN_APP_REQUEST << 2);
	msg->data[1] = IPMI_GET_MSG_CMD;
	msg->data_size = 2;

	if (start_send(smb_info, msg->data, msg->data_size) != 0) {
		unsigned long oflags;
		flags = ipmi_smb_lock_cond(smb_info, &oflags);
		smb_info->curr_msg = NULL;
		smb_info->smb_state = SMB_NORMAL;
		ipmi_smb_unlock_cond(smb_info, flags);
		ipmi_free_smi_msg(msg);
	}
}

/*
 * Must be called with the message lock held.  This will release the
 * message lock.  Note that the caller will check SMB_IDLE and start a
 * new operation, so there is no need to check for new messages to
 * start in here.
 */
static void handle_flags(struct smb_info *smb_info, unsigned long *flags)
{
	if (smb_info->msg_flags & WDT_PRE_TIMEOUT_INT)
		/* Watchdog pre-timeout */
		start_clear_flags(smb_info, flags);
	else if (smb_info->msg_flags & RECEIVE_MSG_AVAIL)
		/* Messages available. */
		start_recv_msg_fetch(smb_info, flags);
	else if (smb_info->msg_flags & EVENT_MSG_BUFFER_FULL)
		/* Events available. */
		start_event_fetch(smb_info, flags);
	else {
		smb_info->smb_state = SMB_NORMAL;
		ipmi_smb_unlock_cond(smb_info, flags);
	}
}

static void msg_done_handler(struct i2c_op_q_entry *i2ce);
static void retry_timeout(unsigned long data)
{
	struct smb_info     *smb_info = (void *) data;
	struct i2c_op_q_entry *i2ce;

	smb_info->rtc_us_timer = 0;

	i2ce = &smb_info->i2c_q_entry;
	i2ce->xfer_type = I2C_OP_SMBUS;
	i2ce->handler = msg_done_handler;
	i2ce->handler_data = smb_info;
	i2ce->smbus.read_write = I2C_SMBUS_READ;
	i2ce->smbus.command = SMB_IPMI_RESPONSE;
	i2ce->smbus.data = (union i2c_smbus_data *) smb_info->recv;
	i2ce->smbus.size = I2C_SMBUS_BLOCK_DATA;

	if (i2c_non_blocking_op(smb_info->client, i2ce)) {
		/* request failed, just return the error. */
		if (smb_info->smb_debug & SMB_DEBUG_MSG) {
			printk(KERN_INFO
			       "Error from i2c_non_blocking_op(5)\n");
		}
		i2ce->result = -EIO;
		msg_done_handler(i2ce);
	}
}

static int start_resend(struct smb_info *smb_info);
static void msg_done_handler(struct i2c_op_q_entry *i2ce)
{
	struct smb_info     *smb_info = i2ce->handler_data;
	unsigned char       *data = smb_info->recv;
	int                 len;
	int                 result = i2ce->result;
	struct ipmi_smi_msg *msg;
	unsigned long       oflags, *flags;

	/*
	 * We are single-threaded here, so no need for a lock until we
	 * start messing with driver states or the queues.
	 */

	if (result < 0) {
		smb_info->retries_left--;
		if (smb_info->retries_left > 0) {
			smb_inc_stat(smb_info, receive_retries);

			mod_timer(&smb_info->retry_timer,
				  jiffies + SMB_MSG_JIFFIES);
			smb_info->rtc_us_timer = SMB_MSG_USEC;
			return;
		}

		smb_inc_stat(smb_info, receive_errors);

		if  (smb_info->smb_debug & SMB_DEBUG_MSG)
			printk(KERN_INFO
			       "Error in msg_done_handler: %d\n", i2ce->result);
		len = 0;
		goto continue_op;
	}

	len = data[0]; /* Number of bytes *after* data[0]. */
	data++;
	if ((len > 1) && (smb_info->multi_pos == 0)
				&& (data[0] == 0x00) && (data[1] == 0x01)) {
		/* Start of multi-part read.  Start the next transaction. */
		int i;

		smb_inc_stat(smb_info, received_message_parts);

		/* Remove the multi-part read marker. */
		for (i = 0; i < (len-2); i++)
			smb_info->data[i] = data[i+2];
		len -= 2;
		smb_info->multi_len = len;
		smb_info->multi_pos = 1;

		i2ce->xfer_type = I2C_OP_SMBUS;
		i2ce->handler = msg_done_handler;
		i2ce->handler_data = smb_info;
		i2ce->smbus.read_write = I2C_SMBUS_READ;
		i2ce->smbus.command = SMB_IPMI_MULTI_PART_RESPONSE_MIDDLE;
		i2ce->smbus.data = ((union i2c_smbus_data *) smb_info->recv);
		i2ce->smbus.size = I2C_SMBUS_BLOCK_DATA;
		if (i2c_non_blocking_op(smb_info->client, i2ce)) {
			if (smb_info->smb_debug & SMB_DEBUG_MSG) {
				printk(KERN_INFO
				       "Error from i2c_non_blocking_op(1)\n");
			}
			result = -EIO;
		} else
			return;
	} else if (smb_info->multi_pos) {
		/* Middle of multi-part read.  Start the next transaction. */
		int i;
		unsigned char blocknum;

		if (len == 0) {
			result = -EIO;
			if (smb_info->smb_debug & SMB_DEBUG_MSG) {
				printk(KERN_INFO
				       "Received middle message with no"
				       " data\n");
			}
			goto continue_op;
		}

		blocknum = data[smb_info->multi_len];

		if (smb_info->multi_len+len-1 > IPMI_MAX_MSG_LENGTH) {
			/* Received message too big, abort the operation. */
			result = -E2BIG;
			if (smb_info->smb_debug & SMB_DEBUG_MSG) {
				printk(KERN_INFO
				       "Received message too big\n");
			}
			goto continue_op;
		}

		/* Remove the blocknum from the data. */
		for (i = 0; i < (len-1); i++)
			smb_info->data[i+smb_info->multi_len] = data[i+1];
		len--;
		smb_info->multi_len += len;
		if (blocknum == 0xff) {
			/* End of read */
			len = smb_info->multi_len;
			data = smb_info->data;
		} else if ((blocknum+1) != smb_info->multi_pos) {
			/*
			 * Out of sequence block, just abort.  Block
			 * numbers start at zero for the second block,
			 * but multi_pos starts at one, so the +1.
			 */
			result = -EIO;
		} else {
			smb_inc_stat(smb_info, received_message_parts);

			smb_info->multi_pos++;
			i2ce->xfer_type = I2C_OP_SMBUS;
			i2ce->handler = msg_done_handler;
			i2ce->handler_data = smb_info;
			i2ce->smbus.read_write = I2C_SMBUS_READ;
			i2ce->smbus.command
				= SMB_IPMI_MULTI_PART_RESPONSE_MIDDLE;
			i2ce->smbus.data = ((union i2c_smbus_data *)
					    smb_info->recv);
			i2ce->smbus.size = I2C_SMBUS_BLOCK_DATA;

			if (i2c_non_blocking_op(smb_info->client, i2ce)) {
				if (smb_info->smb_debug & SMB_DEBUG_MSG) {
					printk(KERN_INFO "Error from"
					       " i2c_non_blocking_op(2)\n");
				}
				result = -EIO;
			} else
				return;
		}
	}

	if (result < 0) {
		smb_inc_stat(smb_info, receive_errors);
	} else {
		smb_inc_stat(smb_info, received_messages);
		smb_inc_stat(smb_info, received_message_parts);
	}


 continue_op:
	if (smb_info->smb_debug & SMB_DEBUG_STATE)
		printk(KERN_INFO "DONE 1: state = %d, result=%d.\n",
		       smb_info->smb_state, result);

	flags = ipmi_smb_lock_cond(smb_info, &oflags);
	msg = smb_info->curr_msg;
	if (msg) {
		msg->rsp_size = len;
		if (msg->rsp_size > IPMI_MAX_MSG_LENGTH)
			msg->rsp_size = IPMI_MAX_MSG_LENGTH;
		memcpy(msg->rsp, data, msg->rsp_size);
		smb_info->curr_msg = NULL;
	}

	switch (smb_info->smb_state) {
	case SMB_NORMAL:
		ipmi_smb_unlock_cond(smb_info, flags);
		if (!msg)
			break;

		if (result < 0)
			return_hosed_msg(smb_info, msg);
		else
			deliver_recv_msg(smb_info, msg);
		break;

	case SMB_GETTING_FLAGS:
		/* We got the flags from the SMB, now handle them. */
		if ((result < 0) || (len < 4) || (data[2] != 0)) {
			/*
			 * Error fetching flags, or invalid length,
			 * just give up for now.
			 */
			smb_info->smb_state = SMB_NORMAL;
			ipmi_smb_unlock_cond(smb_info, flags);
			printk(KERN_WARNING
			       PFX "Error getting flags: %d %d, %2.2x\n",
			       result, len, data[2]);
		} else if (data[0] != (IPMI_NETFN_APP_REQUEST | 1) << 2
			   || data[1] != IPMI_GET_MSG_FLAGS_CMD) {
			printk(KERN_WARNING
			       PFX "Invalid response getting flags: "
			       "%2.2x %2.2x\n", data[0], data[1]);
		} else {
			smb_inc_stat(smb_info, flag_fetches);
			smb_info->msg_flags = data[3];
			handle_flags(smb_info, flags);
		}
		break;

	case SMB_CLEARING_FLAGS:
		/* We cleared the flags. */
		if ((result < 0) || (len < 3) || (data[2] != 0)) {
			/* Error clearing flags */
			printk(KERN_WARNING
			       PFX "Error clearing flags: %d %d, %2.2x\n",
			       result, len, data[2]);
		} else if (data[0] != (IPMI_NETFN_APP_REQUEST | 1) << 2
			   || data[1] != IPMI_CLEAR_MSG_FLAGS_CMD) {
			printk(KERN_WARNING
			       PFX "Invalid response clearing flags: "
			       "%2.2x %2.2x\n", data[0], data[1]);
		}
		smb_info->smb_state = SMB_NORMAL;
		ipmi_smb_unlock_cond(smb_info, flags);
		break;

	case SMB_GETTING_EVENTS:
		if ((result < 0) || (len < 3) || (msg->rsp[2] != 0)) {
			/* Error getting event, probably done. */
			msg->done(msg);

			/* Take off the event flag. */
			smb_info->msg_flags &= ~EVENT_MSG_BUFFER_FULL;
			handle_flags(smb_info, flags);
		} else if (msg->rsp[0] != (IPMI_NETFN_APP_REQUEST | 1) << 2
			   || msg->rsp[1] != IPMI_READ_EVENT_MSG_BUFFER_CMD) {
			printk(KERN_WARNING
			       PFX "Invalid response getting events: "
			       "%2.2x %2.2x\n", msg->rsp[0], msg->rsp[1]);
			msg->done(msg);
			/* Take off the event flag. */
			smb_info->msg_flags &= ~EVENT_MSG_BUFFER_FULL;
			handle_flags(smb_info, flags);
		} else {
			handle_flags(smb_info, flags);
			smb_inc_stat(smb_info, events);
			deliver_recv_msg(smb_info, msg);
		}
		break;

	case SMB_GETTING_MESSAGES:
		if ((result < 0) || (len < 3) || (msg->rsp[2] != 0)) {
			/* Error getting event, probably done. */
			msg->done(msg);

			/* Take off the msg flag. */
			smb_info->msg_flags &= ~RECEIVE_MSG_AVAIL;
			handle_flags(smb_info, flags);
		} else if (msg->rsp[0] != (IPMI_NETFN_APP_REQUEST | 1) << 2
			   || msg->rsp[1] != IPMI_GET_MSG_CMD) {
			printk(KERN_WARNING
			       PFX "Invalid response clearing flags: "
			       "%2.2x %2.2x\n", msg->rsp[0], msg->rsp[1]);
			msg->done(msg);

			/* Take off the msg flag. */
			smb_info->msg_flags &= ~RECEIVE_MSG_AVAIL;
			handle_flags(smb_info, flags);
		} else {
			smb_inc_stat(smb_info, incoming_messages);
			handle_flags(smb_info, flags);
			deliver_recv_msg(smb_info, msg);
		}
		break;
	}

	flags = ipmi_smb_lock_cond(smb_info, &oflags);
	if (SMB_IDLE(smb_info)) {
		if (smb_info->req_events)
			start_event_fetch(smb_info, flags);
		else if (smb_info->req_flags)
			start_flag_fetch(smb_info, flags);
		else
			start_next_msg(smb_info, flags);
	} else
		ipmi_smb_unlock_cond(smb_info, flags);

	if (smb_info->smb_debug & SMB_DEBUG_STATE)
		printk(KERN_INFO "DONE 2: state = %d.\n", smb_info->smb_state);
}

static void msg_written_handler(struct i2c_op_q_entry *i2ce)
{
	struct smb_info *smb_info = i2ce->handler_data;

	/* We are single-threaded here, so no need for a lock. */
	if (i2ce->result < 0) {
		smb_info->retries_left--;
		if (smb_info->retries_left > 0) {
			if (!start_resend(smb_info)) {
				smb_inc_stat(smb_info, send_retries);
				return;
			}
			/* request failed, just return the error. */
			smb_inc_stat(smb_info, send_errors);

			if (smb_info->smb_debug & SMB_DEBUG_MSG) {
				printk(KERN_INFO "Out of retries in"
				       " msg_written_handler\n");
			}
			i2ce->result = -EIO;
			msg_done_handler(i2ce);
			return;
		}

		smb_inc_stat(smb_info, send_errors);

		/*
		 * Got an error on transmit, let the done routine
		 * handle it.
		 */
		if (smb_info->smb_debug & SMB_DEBUG_MSG) {
			printk(KERN_INFO
			       "Error in msg_written_handler: %d\n",
			       i2ce->result);
		}
		msg_done_handler(i2ce);
		return;
	}

	if (smb_info->multi_data) {
		/* In the middle of a multi-data write. */
		int left;

		smb_inc_stat(smb_info, sent_messages_parts);

		left = smb_info->multi_len - smb_info->multi_pos;
		if (left > 32)
			left = 32;
		i2ce->xfer_type = I2C_OP_SMBUS;
		i2ce->handler = msg_written_handler;
		i2ce->handler_data = smb_info;
		i2ce->smbus.read_write = I2C_SMBUS_WRITE;
		i2ce->smbus.data
			= ((union i2c_smbus_data *)
			   (smb_info->multi_data + smb_info->multi_pos));
		/* Length byte. */
		smb_info->multi_data[smb_info->multi_pos] = left;
		i2ce->smbus.size = I2C_SMBUS_BLOCK_DATA;
		smb_info->multi_pos += left;
		i2ce->smbus.command = SMB_IPMI_MULTI_PART_REQUEST_MIDDLE;
		if (left < 32)
			/*
			 * Write is finished.  Note that we must end
			 * with a write of less than 32 bytes to
			 * complete the transaction, even if it is
			 * zero bytes.
			 */
			smb_info->multi_data = NULL;
	} else {
		smb_inc_stat(smb_info, sent_messages);
		smb_inc_stat(smb_info, sent_messages_parts);

		/* Wait a jiffie then request the next message */
		smb_info->retries_left = SMB_RECV_RETRIES;
		mod_timer(&smb_info->retry_timer,
			  jiffies + SMB_MSG_PART_JIFFIES);
		smb_info->rtc_us_timer = SMB_MSG_PART_USEC;
		return;
	}

	if (i2c_non_blocking_op(smb_info->client, i2ce)) {
		/* request failed, just return the error. */
		smb_inc_stat(smb_info, send_errors);

		if (smb_info->smb_debug & SMB_DEBUG_MSG) {
			printk(KERN_INFO
			       "Error from i2c_non_blocking_op(3)\n");
		}
		i2ce->result = -EIO;
		msg_done_handler(i2ce);
	}
}

static int start_resend(struct smb_info *smb_info)
{
	struct i2c_op_q_entry *i2ce;
	int                   rv;

	i2ce = &smb_info->i2c_q_entry;
	i2ce->xfer_type = I2C_OP_SMBUS;
	i2ce->handler = msg_written_handler;
	i2ce->handler_data = smb_info;
	i2ce->smbus.read_write = I2C_SMBUS_WRITE;

	i2ce->smbus.data = (union i2c_smbus_data *) smb_info->data;
	i2ce->smbus.size = I2C_SMBUS_BLOCK_DATA;

	if (smb_info->data_len > 32) {
		i2ce->smbus.command = SMB_IPMI_MULTI_PART_REQUEST_START;
		smb_info->multi_data = smb_info->data;
		smb_info->multi_len = smb_info->data_len;
		/*
		 * Subtle thing, this is 32, not 33, because we will
		 * overwrite the thing at position 32 (which was just
		 * transmitted) with the new length.
		 */
		smb_info->multi_pos = 32;
		smb_info->data[0] = 32;
	} else {
		smb_info->multi_data = NULL;
		i2ce->smbus.command = SMB_IPMI_REQUEST;
		smb_info->data[0] = smb_info->data_len;
	}

	rv = i2c_non_blocking_op(smb_info->client, i2ce);
	if (rv && (smb_info->smb_debug & SMB_DEBUG_MSG)) {
		printk(KERN_INFO
		       "Error from i2c_non_blocking_op(4)\n");
	}
	return rv;
}

static int start_send(struct smb_info *smb_info,
		      unsigned char   *data,
		      unsigned int    len)
{
	if (len > IPMI_MAX_MSG_LENGTH)
		return -E2BIG;
	if (len > smb_info->max_xmit_msg_size)
		return -E2BIG;

	smb_info->retries_left = SMB_SEND_RETRIES;
	memcpy(smb_info->data+1, data, len);
	smb_info->data_len = len;
	return start_resend(smb_info);
}

/* Must be called with the message lock held. */
static void start_next_msg(struct smb_info *smb_info, unsigned long *flags)
{
	struct list_head    *entry = NULL;
	struct ipmi_smi_msg *msg;
	unsigned long oflags;

 restart:
	if (!SMB_IDLE(smb_info)) {
		ipmi_smb_unlock_cond(smb_info, flags);
		return;
	}

	/* Pick the high priority queue first. */
	if (!list_empty(&smb_info->hp_xmit_msgs))
		entry = smb_info->hp_xmit_msgs.next;
	else if (!list_empty(&smb_info->xmit_msgs))
		entry = smb_info->xmit_msgs.next;

	if (!entry) {
		smb_info->curr_msg = NULL;
		ipmi_smb_unlock_cond(smb_info, flags);
	} else {
		int rv;

		list_del(entry);
		msg = list_entry(entry, struct ipmi_smi_msg, link);
		smb_info->curr_msg = msg;
		ipmi_smb_unlock_cond(smb_info, flags);
		rv = start_send(smb_info,
				smb_info->curr_msg->data,
				smb_info->curr_msg->data_size);
		if (rv) {
			smb_info->curr_msg = NULL;
			return_hosed_msg(smb_info, msg);
			flags = ipmi_smb_lock_cond(smb_info, &oflags);
			goto restart;
		}
	}
}

static void sender(void                *send_info,
		   struct ipmi_smi_msg *msg,
		   int                 priority)
{
	struct smb_info *smb_info = (struct smb_info *) send_info;
	unsigned long oflags, *flags;

	flags = ipmi_smb_lock_cond(smb_info, &oflags);
	if (smb_info->run_to_completion) {
		/*
		 * If we are running to completion, then throw it in
		 * the list and run transactions until everything is
		 * clear.  Priority doesn't matter here.
		 */
		list_add_tail(&msg->link, &smb_info->xmit_msgs);
		start_next_msg(smb_info, flags);

		i2c_poll(smb_info->client, 0);
		while (!SMB_IDLE(smb_info)) {
			udelay(500);
			if (smb_info->rtc_us_timer > 0) {
				smb_info->rtc_us_timer -= 500;
				if (smb_info->rtc_us_timer <= 0) {
					retry_timeout((unsigned long) smb_info);
					del_timer(&smb_info->retry_timer);
				}
			}
			i2c_poll(smb_info->client, 500000);
		}
		return;
	}


	if (priority > 0)
		list_add_tail(&msg->link, &smb_info->hp_xmit_msgs);
	else
		list_add_tail(&msg->link, &smb_info->xmit_msgs);
	start_next_msg(smb_info, flags);

	if (smb_info->smb_debug & SMB_DEBUG_TIMING) {
		struct timeval     t;
		do_gettimeofday(&t);
		printk(KERN_INFO
		       "**Enqueue %02x %02x: %ld.%6.6ld\n",
		       msg->data[0], msg->data[1], t.tv_sec, t.tv_usec);
	}
}

/*
 * Instead of having our own timer to periodically check the message
 * flags, we let the message handler drive us.
 */
static void request_events(void *send_info)
{
	struct smb_info *smb_info = (struct smb_info *) send_info;
	unsigned long oflags, *flags;

	/*
	 * If we are stopping, just ignore requests for events.  It's
	 * not a big deal if the stop fails and we miss one of
	 * these.
	 */
	if (smb_info->stopping || !smb_info->has_event_buffer)
		return;

	flags = ipmi_smb_lock_cond(smb_info, &oflags);
	/*
	 * Request flags first, not events, because the lower layer
	 * doesn't have a way to send an attention.  But make sure
	 * event checking still happens.
	 */
	smb_info->req_events = 1;
	if (SMB_IDLE(smb_info))
		start_flag_fetch(smb_info, flags);
	else {
		smb_info->req_flags = 1;
		ipmi_smb_unlock_cond(smb_info, flags);
	}
}

static void set_run_to_completion(void *send_info, int i_run_to_completion)
{
	struct smb_info *smb_info = (struct smb_info *) send_info;

	smb_info->run_to_completion = i_run_to_completion;
	/*
	 * Note that if this does not compile, there are some I2C
	 * changes that you need to handle this properly.
	 */
	if (i_run_to_completion) {
		i2c_poll(smb_info->client, 0);
		while (!SMB_IDLE(smb_info)) {
			udelay(500);
			if (smb_info->rtc_us_timer > 0) {
				smb_info->rtc_us_timer -= 500;
				if (smb_info->rtc_us_timer <= 0) {
					retry_timeout((unsigned long) smb_info);
					del_timer(&smb_info->retry_timer);
				}
			}
			i2c_poll(smb_info->client, 500000);
		}
	}
}

static void poll(void *send_info)
{
	struct smb_info *smb_info = send_info;
	i2c_poll(smb_info->client, 10000);
}

static int inc_usecount(void *send_info)
{
	struct smb_info *smb_info = send_info;
	i2c_use_client(smb_info->client);
	return 0;
}

static void dec_usecount(void *send_info)
{
	struct smb_info *smb_info = send_info;
	i2c_release_client(smb_info->client);
}

static int smb_start_processing(void       *send_info,
				ipmi_smi_t intf)
{
	struct smb_info *smb_info = send_info;

	smb_info->intf = intf;

	return 0;
}

static struct ipmi_smi_handlers handlers = {
	.owner                 = THIS_MODULE,
	.start_processing      = smb_start_processing,
	.sender		       = sender,
	.request_events        = request_events,
	.set_run_to_completion = set_run_to_completion,
	.poll		       = poll,
	.inc_usecount	       = inc_usecount,
	.dec_usecount	       = dec_usecount
};

#define MAX_SMB_BMCS 4

static unsigned short addr[MAX_SMB_BMCS];
static int num_addrs;

module_param_array(addr, ushort, &num_addrs, 0);
MODULE_PARM_DESC(addr, "Sets the addresses to scan for IPMI BMCs on the SMBus."
		 " By default the driver will scan for anything it finds in"
		 " DMI or ACPI tables.  Otherwise you have to hand-specify"
		 " the address.  This is a list of addresses to scan.  If you"
		 " don't provide this and don't have DMI/ACPI, it probably"
		 " won't work.");

/* FIXME - this is broken */
static int smb_defaultprobe;
module_param_named(defaultprobe, smb_defaultprobe, int, 0);
MODULE_PARM_DESC(defaultprobe, "Normally the driver will not scan anything"
		 " but the specified values and DMI/ACPI values.  If you set"
		 " this to non-zero it will scan all addresses except for"
		 " certain dangerous ones.");

static int slave_addrs[MAX_SMB_BMCS];
static int num_slave_addrs;
module_param_array(slave_addrs, int, &num_slave_addrs, 0);
MODULE_PARM_DESC(slave_addrs, "Set the default IPMB slave address for"
		 " the controller.  Normally this is 0x20, but can be"
		 " overridden by this parm.  This is an array indexed"
		 " by interface number.");

static int dbg[MAX_SMB_BMCS];
static int num_dbg;
module_param_array(dbg, int, &num_dbg, 0);
MODULE_PARM_DESC(dbg, "Turn on debugging.  Bit 0 enables message debugging,"
		 " bit 1 enables state debugging, and bit 2 enables timing"
		 " debugging.  This is an array indexed by interface number");

module_param_named(dbg_probe, smb_dbg_probe, int, 0);
MODULE_PARM_DESC(dbg_probe, "Enable debugging of probing of adapters.");

static unsigned int next_smb_if = 0;

static int smb_remove(struct i2c_client *client)
{
	struct smb_info *smb_info = i2c_get_clientdata(client);
	int rv;

	if (!smb_info)
		return 0;

	/* Don't allow the upper layer to request events any more. */
	smb_info->stopping = 1;

	/* make sure the driver is not looking for flags any more. */
	while (smb_info->smb_state != SMB_NORMAL)
		msleep(1);

	/*
	 * After this point, we won't deliver anything asychronously
	 * to the message handler.  We can unregister ourself.
	 */
	rv = ipmi_unregister_smi(smb_info->intf);
	if (rv) {
		smb_info->stopping = 0;
		printk(KERN_ERR
		       PFX "Unable to unregister device: errno=%d\n",
		       rv);
		return rv;
	}

	/*
	 * No message can be outstanding now, we have removed the
	 * upper layer and it permitted us to do so.
	 */

	kfree(smb_info);
	return 0;
}

static int do_cmd(struct i2c_client *client, int len, unsigned char *msg,
		  int *resp_len, unsigned char *resp)
{
	int retry_cnt;
	int ret;

	retry_cnt = SMB_SEND_RETRIES;
 retry1:
	ret = i2c_smbus_write_block_data(client, SMB_IPMI_REQUEST, len, msg);
	if (ret) {
		retry_cnt--;
		if (retry_cnt > 0)
			goto retry1;
		return -ENODEV;
	}

	ret = -ENODEV;
	retry_cnt = SMB_RECV_RETRIES;
	while (retry_cnt > 0) {
		ret = i2c_smbus_read_block_data(client, SMB_IPMI_RESPONSE,
						resp);
		if (ret > 0)
			break;
		msleep(SMB_MSG_MSEC);
		retry_cnt--;
		if (retry_cnt <= 0)
			break;
	}

	if (ret > 0) {
		/* Validate that the response is correct. */
		if (ret < 3 ||
		    (resp[0] != (msg[0] | (1 << 2))) ||
		    (resp[1] != msg[1]))
			ret = -EINVAL;
		else {
			*resp_len = ret;
			ret = 0;
		}
	}

	return ret;
}

static int smb_detect(struct i2c_client *client, struct i2c_board_info *info)
{
	unsigned char *resp;
	unsigned char msg[3];
	int           rv;
	int           len;

	resp = kmalloc(IPMI_MAX_MSG_LENGTH, GFP_KERNEL);
	if (!resp)
		return -ENOMEM;

	/* Do a Get Device ID command, since it is required. */
	msg[0] = IPMI_NETFN_APP_REQUEST << 2;
	msg[1] = IPMI_GET_DEVICE_ID_CMD;
	rv = do_cmd(client, 2, msg, &len, resp);
	if (rv)
		rv = -ENODEV;
	else
		strlcpy(info->type, "ipmi_smb", I2C_NAME_SIZE);
	kfree(resp);
	return rv;
}

static int smi_type_proc_show(struct seq_file *m, void *v)
{
	return seq_printf(m, "smb\n");
}

static int smi_type_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, smi_type_proc_show, inode->i_private);
}

static const struct file_operations smi_type_proc_ops = {
	.open		= smi_type_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int smi_stats_proc_show(struct seq_file *m, void *v)
{
	struct smb_info *smb_info = m->private;

	seq_printf(m, "sent_messages:          %u\n",
		   smb_get_stat(smb_info, sent_messages));
	seq_printf(m, "sent_messages_parts:    %u\n",
		   smb_get_stat(smb_info, sent_messages_parts));
	seq_printf(m, "send_retries:           %u\n",
		   smb_get_stat(smb_info, send_retries));
	seq_printf(m, "send_errors:            %u\n",
		   smb_get_stat(smb_info, send_errors));
	seq_printf(m, "received_messages:      %u\n",
		   smb_get_stat(smb_info, received_messages));
	seq_printf(m, "received_message_parts: %u\n",
		   smb_get_stat(smb_info, received_message_parts));
	seq_printf(m, "receive_retries:        %u\n",
		   smb_get_stat(smb_info, receive_retries));
	seq_printf(m, "receive_errors:         %u\n",
		   smb_get_stat(smb_info, receive_errors));
	seq_printf(m, "flag_fetches:           %u\n",
		   smb_get_stat(smb_info, flag_fetches));
	seq_printf(m, "hosed:                  %u\n",
		   smb_get_stat(smb_info, hosed));
	seq_printf(m, "events:                 %u\n",
		   smb_get_stat(smb_info, events));
	return 0;
}

static int smi_stats_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, smi_stats_proc_show, inode->i_private);
}

static const struct file_operations smi_stats_proc_ops = {
	.open		= smi_stats_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int smb_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	unsigned char     msg[3];
	unsigned char     *resp;
	struct smb_info   *smb_info;
	int               rv = 0;
	int               len;
	int               nr;
	int               debug;
	int               i;
	int               slave_addr;

	/* I belive these can only happen one at a time, so no lock... */
	nr = next_smb_if++;

	if (nr >= MAX_SMB_BMCS) {
		debug = 0;
		slave_addr = 0;
	} else {
		debug = dbg[nr];
		slave_addr = slave_addrs[nr];
	}

	resp = kmalloc(IPMI_MAX_MSG_LENGTH, GFP_KERNEL);
	if (!resp)
		return -ENOMEM;

	smb_info = kzalloc(sizeof(*smb_info), GFP_KERNEL);
	if (!smb_info) {
		kfree(resp);
		return -ENOMEM;
	}

	/* Do a Get Device ID command, since it comes back with some
	   useful info. */
	msg[0] = IPMI_NETFN_APP_REQUEST << 2;
	msg[1] = IPMI_GET_DEVICE_ID_CMD;
	rv = do_cmd(client, 2, msg, &len, resp);
	if (rv)
		goto out;

	rv = ipmi_demangle_device_id(resp, len, &smb_info->device_id);
	if (rv)
		goto out;

	smb_info->client = client;
	i2c_set_clientdata(client, smb_info);
	smb_info->smb_debug = debug;

	/* Now check for system interface capabilities */
	msg[0] = IPMI_NETFN_APP_REQUEST << 2;
	msg[1] = IPMI_GET_SYSTEM_INTERFACE_CAPABILITIES_CMD;
	msg[2] = 0; /* SSIF */
	rv = do_cmd(client, 3, msg, &len, resp);
	if (!rv && (len >= 3) && (resp[2] == 0)) {
		if (len < 7) {
			if (smb_dbg_probe)
				printk(KERN_INFO
				       PFX "SSIF info too short: %d\n", len);
			goto no_support;
		}

		/* Got a good SSIF response, handle it. */
		smb_info->max_xmit_msg_size = resp[5];
		smb_info->max_recv_msg_size = resp[6];
		smb_info->multi_support = (resp[4] >> 6) & 0x3;
		smb_info->supports_pec = (resp[4] >> 3) & 0x1;

		/* Sanitize the data */
		switch (smb_info->multi_support) {
		case SMB_NO_MULTI:
			if (smb_info->max_xmit_msg_size > 32)
				smb_info->max_xmit_msg_size = 32;
			if (smb_info->max_recv_msg_size > 32)
				smb_info->max_recv_msg_size = 32;
			break;

		case SMB_MULTI_2_PART:
			if (smb_info->max_xmit_msg_size > 64)
				smb_info->max_xmit_msg_size = 64;
			if (smb_info->max_recv_msg_size > 62)
				smb_info->max_recv_msg_size = 62;
			break;

		case SMB_MULTI_n_PART:
			break;

		default:
			/* Data is not sane, just give up. */
			goto no_support;
		}
	} else {
 no_support:
		/* Assume no multi-part or PEC support */
		printk(KERN_INFO PFX "Error fetching SSIF: %d %d %2.2x, "
		       "your system probably doesn't support this command so "
		       "using defaults\n",
		       rv, len, resp[2]);

		smb_info->max_xmit_msg_size = 32;
		smb_info->max_recv_msg_size = 32;
		smb_info->multi_support = SMB_NO_MULTI;
		smb_info->supports_pec = 0;
	}

	/* Make sure the NMI timeout is cleared. */
	msg[0] = IPMI_NETFN_APP_REQUEST << 2;
	msg[1] = IPMI_CLEAR_MSG_FLAGS_CMD;
	msg[2] = WDT_PRE_TIMEOUT_INT;
	rv = do_cmd(client, 3, msg, &len, resp);
	if (rv || (len < 3) || (resp[2] != 0))
		printk(KERN_WARNING
		       PFX "Unable to clear message flags: %d %d %2.2x\n",
		       rv, len, resp[2]);

	/* Attempt to enable the event buffer. */
	msg[0] = IPMI_NETFN_APP_REQUEST << 2;
	msg[1] = IPMI_GET_BMC_GLOBAL_ENABLES_CMD;
	rv = do_cmd(client, 2, msg, &len, resp);
	if (rv || (len < 4) || (resp[2] != 0)) {
		printk(KERN_WARNING
		       PFX "Error getting global enables: %d %d %2.2x\n",
		       rv, len, resp[2]);
		rv = 0; /* Not fatal */
		goto found;
	}

	if (resp[3] & IPMI_BMC_EVT_MSG_BUFF) {
		smb_info->has_event_buffer = 1;
		/* buffer is already enabled, nothing to do. */
		goto found;
	}

	msg[0] = IPMI_NETFN_APP_REQUEST << 2;
	msg[1] = IPMI_SET_BMC_GLOBAL_ENABLES_CMD;
	msg[2] = resp[3] | IPMI_BMC_EVT_MSG_BUFF;
	rv = do_cmd(client, 3, msg, &len, resp);
	if (rv || (len < 2)) {
		printk(KERN_WARNING
		       PFX "Error getting global enables: %d %d %2.2x\n",
		       rv, len, resp[2]);
		rv = 0; /* Not fatal */
		goto found;
	}

	if (resp[2] == 0)
		/* A successful return means the event buffer is supported. */
		smb_info->has_event_buffer = 1;

 found:
	if (smb_dbg_probe) {
		printk(KERN_INFO
		       "smb_probe: i2c_probe found device at"
		       " i2c address %x\n", client->addr);
	}

	spin_lock_init(&smb_info->msg_lock);
	INIT_LIST_HEAD(&smb_info->xmit_msgs);
	INIT_LIST_HEAD(&smb_info->hp_xmit_msgs);
	smb_info->smb_state = SMB_NORMAL;
	init_timer(&smb_info->retry_timer);
	smb_info->retry_timer.data = (unsigned long) smb_info;
	smb_info->retry_timer.function = retry_timeout;

	for (i = 0; i < SMB_NUM_STATS; i++)
		atomic_set(&smb_info->stats[i], 0);

	if (smb_info->supports_pec)
		smb_info->client->flags |= I2C_CLIENT_PEC;

	rv = ipmi_register_smi(&handlers,
			       smb_info,
			       &smb_info->device_id,
			       &smb_info->client->dev,
			       "bmc",
			       slave_addr);
	 if (rv) {
		printk(KERN_ERR
		       PFX "Unable to register device: error %d\n",
		       rv);
		goto out;
	}

	rv = ipmi_smi_add_proc_entry(smb_info->intf, "type",
				     &smi_type_proc_ops,
				     smb_info);
	if (rv) {
		printk(KERN_ERR PFX "Unable to create proc entry: %d\n",
		       rv);
		goto out_err_unreg;
	}

	rv = ipmi_smi_add_proc_entry(smb_info->intf, "smb_stats",
				     &smi_stats_proc_ops,
				     smb_info);
	if (rv) {
		printk(KERN_ERR PFX "Unable to create proc entry: %d\n",
		       rv);
		goto out_err_unreg;
	}

 out:
	if (rv)
		kfree(smb_info);
	kfree(resp);
	return rv;

 out_err_unreg:
	ipmi_unregister_smi(smb_info->intf);
	goto out;
}

#if 0
/*
 * This was provided (but not used by the kernel) before 2.6.33.  I've kept
 * it here as a list of addresses to *not* try.  Maybe it will be handy
 * in the future.
 */
static unsigned short reserved[] =
{
/* As defined by SMBus Spec. Appendix C */
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x28,
	0x37,
/* As defined by SMBus Spec. Sect. 5.2 */
	0x01, 0x02, 0x03, 0x04, 0x05,
	0x06, 0x07, 0x78, 0x79, 0x7a, 0x7b,
	0x7c, 0x7d, 0x7e, 0x7f,
/* Common PC addresses (bad idea) */
	0x2d, 0x48, 0x49, /* sensors */
	0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, /* eeproms */
	0x69, /* clock chips */

	I2C_CLIENT_END
};
#endif

static const struct i2c_device_id smb_id[] = {
	{ "ipmi_smb", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, smb_id);

static short address_list[MAX_SMB_BMCS + 1];

static struct i2c_driver smb_i2c_driver = {
	.class		= I2C_CLASS_HWMON,
	.driver         = {
		.owner	= THIS_MODULE,
		.name  = "ipmi_smb"
	},
	.probe        = smb_probe,
	.remove       = smb_remove,
	.id_table     = smb_id,
	.detect       = smb_detect,
	.address_list = address_list
};

#ifdef CONFIG_ACPI_INTERPRETER

#include <linux/acpi.h>

/*
 * Defined in the IPMI 2.0 spec.
 */
struct SPMITable {
	s8	Signature[4];
	u32	Length;
	u8	Revision;
	u8	Checksum;
	s8	OEMID[6];
	s8	OEMTableID[8];
	s8	OEMRevision[4];
	s8	CreatorID[4];
	s8	CreatorRevision[4];
	u8	InterfaceType;
	u8	IPMIlegacy;
	s16	SpecificationRevision;

	/*
	 * Bit 0 - SCI interrupt supported
	 * Bit 1 - I/O APIC/SAPIC
	 */
	u8	InterruptType;

	/*
	 * If bit 0 of InterruptType is set, then this is the SCI
	 * interrupt in the GPEx_STS register.
	 */
	u8	GPE;

	s16	Reserved;

	/*
	 * If bit 1 of InterruptType is set, then this is the I/O
	 * APIC/SAPIC interrupt.
	 */
	u32	GlobalSystemInterrupt;

	/* The actual register address. */
	struct acpi_generic_address addr;

	u8	UID[4];

	s8      spmi_id[1]; /* A '\0' terminated array starts here. */
};

static int __devinit decode_acpi(int acpi_num)
{
	acpi_status      status;
	struct SPMITable *spmi;

	if (num_addrs >= MAX_SMB_BMCS)
		return -1;

	i = num_addrs;

	status = acpi_get_firmware_table("SPMI", acpi+1,
					 ACPI_LOGICAL_ADDRESSING,
					 (struct acpi_table_header **) &spmi);
	if (status != AE_OK)
		return -ENODEV;

	if (spmi->IPMIlegacy != 1) {
		printk(KERN_WARNING "IPMI: Bad SPMI legacy %d\n",
		       spmi->IPMIlegacy);
		return -ENODEV;
	}

	if (spmi->InterfaceType != 4)
		return -ENODEV;

	if (spmi->addr.address_space_id != 4) {
		printk(KERN_WARNING
		       PFX "Invalid ACPI SSIF I/O Address type\n");
		return -EIO;
	}

	num_addrs = i + 1;

	/* User didn't specify, so use this one. */
	addr[i] = spmi->addr.address >> 1;
	printk(KERN_INFO PFX "ACPI/SPMI specifies SSIF @ 0x%x\n",
	       addr[i]);

	return 0;
}
#endif

#ifdef CONFIG_DMI
static int decode_dmi(struct dmi_header *dm)
{
	int           addr_in_slave_addr = 0;
	u8            *data = (u8 *)dm;
	u8            len = dm->length;
	int           i;

	if (num_addrs >= MAX_SMB_BMCS)
		return -1;

	i = num_addrs;

	if (len < 9)
		return -1;

	if (data[0x04] != 4) /* Not SSIF */
		return -1;

	num_addrs = i + 1;

	if ((data[8] >> 1) == 0) {
		/*
		 * Some broken systems put the I2C address in
		 * the slave address field.  We try to
		 * accommodate them here.
		 */
		addr[i] = data[6] >> 1;
		addr_in_slave_addr = 1;
	} else
		addr[i] = data[8] >> 1;

	if (!addr_in_slave_addr) {
		slave_addrs[i] = data[6];
		printk(KERN_INFO PFX "DMI specifies SSIF @ 0x%x, "
		       "slave address 0x%x\n",
		       addr[i], slave_addrs[i]);
	} else
		printk(KERN_INFO PFX "DMI specifies SSIF @ 0x%x\n",
		       addr[i]);

	return 0;
}

static void dmi_iterator(void)
{
	const struct dmi_device *dev = NULL;

	while ((dev = dmi_find_device(DMI_DEV_TYPE_IPMI, NULL, dev)))
		decode_dmi((struct dmi_header *) dev->device_data);
}
#endif

static int init_ipmi_smb(void)
{
	int i;
	int rv;

	if (initialized)
		return 0;

	printk(KERN_INFO "IPMI SMB Interface driver\n");

#ifdef CONFIG_DMI
	dmi_iterator();
#endif
#ifdef CONFIG_ACPI_INTERPRETER
	for (i = 0; i < MAX_SMB_BMCS; i++)
		decode_acpi(i);
#endif

	/* build list for i2c from addr list */
	for (i = 0; i < num_addrs; i++)
		address_list[i] = addr[i];
	address_list[i] = I2C_CLIENT_END;

	rv = i2c_add_driver(&smb_i2c_driver);
	if (!rv)
		initialized = 1;

	return rv;
}
module_init(init_ipmi_smb);

static void cleanup_ipmi_smb(void)
{
	if (!initialized)
		return;

	initialized = 0;

	i2c_del_driver(&smb_i2c_driver);
}
module_exit(cleanup_ipmi_smb);

MODULE_AUTHOR("Todd C Davis <todd.c.davis@intel.com>, "
	      "Corey Minyard <minyard@acm.org>");
MODULE_DESCRIPTION("IPMI driver for management controllers on a SMBus");
MODULE_LICENSE("GPL");
