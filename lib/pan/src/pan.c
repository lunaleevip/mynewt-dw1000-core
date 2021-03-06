/*
 * Copyright 2018, Decawave Limited, All Rights Reserved
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/**
 * @file dw1000_pan.c
 * @autjor paul kettle
 * @date 2018
 * @brief Personal Area Network
 *
 * @details This is the pan base class which utilises the functions to allocate/deallocate the resources on pan_master,sets callbacks, enables  * blink_requests.
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <os/os.h>
#include <hal/hal_spi.h>
#include <hal/hal_gpio.h>
#include "bsp/bsp.h"


#include <dw1000/dw1000_regs.h>
#include <dw1000/dw1000_dev.h>
#include <dw1000/dw1000_hal.h>
#include <dw1000/dw1000_mac.h>
#include <dw1000/dw1000_phy.h>
#include <dw1000/dw1000_ftypes.h>

#if MYNEWT_VAL(PAN_ENABLED)
#include <pan/pan.h>

//! Buffers for pan frames
static pan_frame_t frames[] = {
    [0] = {
        .fctrl = FCNTL_IEEE_BLINK_TAG_64,    // frame control (FCNTL_IEEE_BLINK_64 to indicate a data frame using 16-bit addressing).
        .seq_num = 0x0,
    },
    [1] = {
        .fctrl = FCNTL_IEEE_BLINK_TAG_64,    // frame control (FCNTL_IEEE_BLINK_64 to indicate a data frame using 16-bit addressing).
    }
};

static dw1000_pan_config_t g_config = {
    .tx_holdoff_delay = MYNEWT_VAL(PAN_TX_HOLDOFF),         // Send Time delay in usec.
    .rx_timeout_period = MYNEWT_VAL(PAN_RX_TIMEOUT)         // Receive response timeout in usec.
};

static bool pan_rx_complete_cb(dw1000_dev_instance_t * inst, dw1000_mac_interface_t * cbs);
static bool pan_tx_complete_cb(dw1000_dev_instance_t * inst, dw1000_mac_interface_t * cbs);
static bool pan_rx_timeout_cb(dw1000_dev_instance_t * inst, dw1000_mac_interface_t * cbs);
static bool pan_rx_error_cb(dw1000_dev_instance_t * inst, dw1000_mac_interface_t * cbs);
static bool pan_tx_error_cb(dw1000_dev_instance_t * inst, dw1000_mac_interface_t * cbs);
static bool pan_reset_cb(dw1000_dev_instance_t * inst, dw1000_mac_interface_t * cbs);
static void pan_postprocess(struct os_event * ev);

/**
 * API to initialise the pan package.
 *
 * @return void
 */
void pan_pkg_init(void){

    printf("{\"utime\": %lu,\"msg\": \"pan_pkg_init\"}\n",os_cputime_ticks_to_usecs(os_cputime_get32()));

#if MYNEWT_VAL(DW1000_DEVICE_0)
    dw1000_pan_init(hal_dw1000_inst(0), &g_config);
#endif
#if MYNEWT_VAL(DW1000_DEVICE_1)
    dw1000_pan_init(hal_dw1000_inst(1), &g_config);
#endif
#if MYNEWT_VAL(DW1000_DEVICE_2)
    dw1000_pan_init(hal_dw1000_inst(2), &g_config);
#endif
}


/**
 * API to initialise pan parameters.
 *
 * @param inst     Pointer to dw1000_dev_instance_t.
 * @param config   Pointer to dw1000_pan_config_t.
 *
 * @return dw1000_pan_instance_t
 */
dw1000_pan_instance_t *
dw1000_pan_init(dw1000_dev_instance_t * inst,  dw1000_pan_config_t * config){
    assert(inst);

    uint16_t nframes = sizeof(frames)/sizeof(pan_frame_t);
    if (inst->pan == NULL ) {
        inst->pan = (dw1000_pan_instance_t *) malloc(sizeof(dw1000_pan_instance_t) + nframes * sizeof(pan_frame_t *));
        assert(inst->pan);
        memset(inst->pan, 0, sizeof(dw1000_pan_instance_t));
        inst->pan->status.selfmalloc = 1;
        inst->pan->nframes = nframes;
    }

    inst->pan->parent = inst;
    inst->pan->period = MYNEWT_VAL(PAN_PERIOD);
    inst->pan->config = config;
    inst->pan->control = (dw1000_pan_control_t){
        .postprocess = false,
    };

    os_error_t err = os_sem_init(&inst->pan->sem, 0x1);
    err |= os_sem_init(&inst->pan->sem_waitforsucess, 0x1);
    assert(err == OS_OK);

    for (uint16_t i = 0; i < inst->pan->nframes; i++)
        inst->pan->frames[i] = &frames[i];

    dw1000_pan_set_postprocess(inst, pan_postprocess);
    inst->pan->cbs = (dw1000_mac_interface_t){
        .id = DW1000_PAN,
            .tx_complete_cb = pan_tx_complete_cb,
            .rx_complete_cb = pan_rx_complete_cb,
            .rx_timeout_cb = pan_rx_timeout_cb,
            .rx_error_cb = pan_rx_error_cb,
            .tx_error_cb = pan_tx_error_cb,
            .reset_cb = pan_reset_cb,
    };
    dw1000_mac_append_interface(inst, &inst->pan->cbs);

    dw1000_pan_instance_t * pan = inst->pan;
    pan_frame_t * frame = pan->frames[(pan->idx)%pan->nframes];
    frame->transmission_timestamp = dw1000_read_systime(inst);
    inst->pan->status.initialized = 1;
    return inst->pan;
}

/**
 * API to free pan resources.
 *
 * @param inst  Pointer to dw1000_dev_instance_t.
 *
 * @return void
 */
void
dw1000_pan_free(dw1000_dev_instance_t * inst){
    assert(inst->pan);
    dw1000_mac_remove_interface(inst, DW1000_PAN);
    if (inst->status.selfmalloc)
        free(inst->pan);
    else
        inst->status.initialized = 0;
}

/**
 * API to set pan_postprocess.
 *
 * @param inst              Pointer to dw1000_dev_instance_t.
 * @param pan_postprocess   Pointer to os_event_fn.
 *
 * @return void
 */
void
dw1000_pan_set_postprocess(dw1000_dev_instance_t * inst, os_event_fn * pan_postprocess){
    dw1000_pan_instance_t * pan = inst->pan;
    os_callout_init(&pan->pan_callout_postprocess, os_eventq_dflt_get(),
                    pan_postprocess, (void *) inst);
    pan->control.postprocess = true;
}

/**
 * This a template which should be replaced by the pan_master by a event that tracks UUIDs
 * and allocated PANIDs and SLOTIDs.
 *
 * @param ev  Pointer to os_events.
 *
 * @return void
 */
static void
pan_postprocess(struct os_event * ev){
    assert(ev != NULL);
    assert(ev->ev_arg != NULL);
    dw1000_dev_instance_t * inst = (dw1000_dev_instance_t *)ev->ev_arg;
    dw1000_pan_instance_t * pan = inst->pan;
    pan_frame_t * frame = pan->frames[(pan->idx)%pan->nframes];

    if(pan->status.valid && frame->long_address == inst->my_long_address)
        printf("{\"utime\":%lu,\"UUID\":\"%llX\",\"ID\":\"%X\",\"PANID\":\"%X\",\"slot\":%d}\n",
            os_cputime_ticks_to_usecs(os_cputime_get32()),
            frame->long_address,
            frame->short_address,
            frame->pan_id,
            frame->slot_id
        );
    else if (inst->frame_len == sizeof(struct _ieee_blink_frame_t))
        printf("{\"utime\":%lu,\"UUID\":\"%llX\",\"seq_num\":%d}\n",
            os_cputime_ticks_to_usecs(os_cputime_get32()),
            frame->long_address,
            frame->seq_num
        );
    else if (inst->frame_len == sizeof(struct _pan_frame_resp_t))
        printf("{\"utime\":%lu,\"UUID\":\"%llX\",\"ID\":\"%X\",\"PANID\":\"%X\",\"slot\":%d}\n",
            os_cputime_ticks_to_usecs(os_cputime_get32()),
            frame->long_address,
            frame->short_address,
            frame->pan_id,
            frame->slot_id
        );
}

/**
 * This is an internal static function that executes on both the pan_master Node and the TAG/ANCHOR
 * that initiated the blink. On the pan_master the postprocess function should allocate a PANID and a SLOTID,
 * while on the TAG/ANCHOR the returned allocations are assigned and the PAN discover event is stopped. The pan
 * discovery resources can be released.
 *
 * @param inst    Pointer to dw1000_dev_instance_t.
 *
 * @return bool
 */
static bool
pan_rx_complete_cb(dw1000_dev_instance_t * inst, dw1000_mac_interface_t * cbs)
{
    if(inst->fctrl_array[0] != FCNTL_IEEE_BLINK_TAG_64) {
        if (inst->pan->status.valid == false) {
            /* Grab all packets if we're not provisioned */
            return true;
        }
        return false;
    }
    dw1000_pan_instance_t * pan = inst->pan;
    pan_frame_t * frame = pan->frames[(pan->idx)%pan->nframes];

    if (inst->frame_len == sizeof(struct _ieee_blink_frame_t)){
        dw1000_read_rx(inst, frame->array, 0, sizeof(struct _ieee_blink_frame_t));
        frame->reception_timestamp = dw1000_read_rxtime(inst);
        int32_t tracking_interval = (int32_t) dw1000_read_reg(inst, RX_TTCKI_ID, 0, sizeof(int32_t));
        int32_t tracking_offset = (int32_t) dw1000_read_reg(inst, RX_TTCKO_ID, 0, sizeof(int32_t)) & RX_TTCKO_RXTOFS_MASK;
        frame->correction_factor = 1.0f + ((float)tracking_offset) / tracking_interval;
    }
    else if (inst->frame_len == sizeof(struct _pan_frame_resp_t)) {
        dw1000_read_rx(inst, frame->array, 0, sizeof(struct _pan_frame_resp_t));

        if(frame->long_address == inst->my_long_address){
            // TAG/ANCHOR side
            inst->my_short_address = frame->short_address;
            inst->PANID = frame->pan_id;
            inst->slot_id = frame->slot_id;
            pan->status.valid = true;
            os_sem_release(&pan->sem);
            os_sem_release(&pan->sem_waitforsucess);
        }
    }
    // both pan_master and TAG/ANCHOR
    if (pan->control.postprocess)
        os_eventq_put(os_eventq_dflt_get(), &pan->pan_callout_postprocess.c_ev);

    /* Release sem */
    os_error_t err = os_sem_release(&inst->pan->sem);
    assert(err == OS_OK);
    return true;
}

/**
 * API for transmit complete callback.
 *
 * @param inst  Pointer to dw1000_dev_instance_t.
 *
 * @return bool
 */
static bool
pan_tx_complete_cb(dw1000_dev_instance_t * inst, dw1000_mac_interface_t * cbs){
    if(inst->fctrl_array[0] != FCNTL_IEEE_BLINK_TAG_64){
        return false;
    }
    dw1000_pan_instance_t * pan = inst->pan;
    pan->idx++;
    return true;
}

/**
 * API for receive complete callback.
 *
 * @param inst   Pointer to dw1000_dev_instance_t.
 *
 * @return bool
 */
static bool
pan_rx_error_cb(dw1000_dev_instance_t * inst, dw1000_mac_interface_t * cbs){
    /* Place holder */
    if(inst->fctrl_array[0] != FCNTL_IEEE_BLINK_TAG_64){
        return false;
    }
    os_sem_release(&inst->pan->sem);
    return true;
}

/**
 * API for reset callback.
 *
 * @param inst   Pointer to dw1000_dev_instance_t.
 *
 * @return bool
 */
static bool
pan_reset_cb(dw1000_dev_instance_t * inst, dw1000_mac_interface_t * cbs){
    if (os_sem_get_count(&inst->pan->sem) == 0){
        //printf("{\"utime\": %lu,\"log\": \"ccp_rx_timeout_cb\",\"%s\":%d}\n",os_cputime_ticks_to_usecs(os_cputime_get32()),__FILE__, __LINE__); 
        os_sem_release(&inst->pan->sem);  
        return false;   
    }
    return true;
}


/**
 * API for transmit error callback.
 *
 * @param inst   Pointer to dw1000_dev_instance_t.
 *
 * @return bool
 */

static bool
pan_tx_error_cb(dw1000_dev_instance_t * inst, dw1000_mac_interface_t * cbs)
{
    /* Place holder */
    if(inst->fctrl_array[0] != FCNTL_IEEE_BLINK_TAG_64){
        return false;
    }
    return true;
}
/**
 * API for receive timeout callback.
 *
 * @param inst    Pointer to dw1000_dev_instance_t.
 *
 * @return bool
 */
static bool
pan_rx_timeout_cb(dw1000_dev_instance_t * inst, dw1000_mac_interface_t * cbs)
{
    if (os_sem_get_count(&inst->pan->sem) == 0){
        //printf("{\"utime\": %lu,\"log\": \"pan_rx_timeout_cb\",\"%s\":%d}\n",os_cputime_ticks_to_usecs(os_cputime_get32()),__FILE__, __LINE__);
        os_sem_release(&inst->pan->sem);  
        return true;
    }
    return false;
}

/**
 * Listen for PAN requests
 *
 * @param inst          Pointer to dw1000_dev_instance_t.
 *
 * @return dw1000_dev_status_t 
 */
dw1000_dev_status_t 
dw1000_pan_listen(dw1000_dev_instance_t * inst, dw1000_dev_modes_t mode)
{
    os_error_t err = os_sem_pend(&inst->pan->sem,  OS_TIMEOUT_NEVER);
    assert(err == OS_OK);

    /* We're listening for others, hence we have to have a valid pan */
    inst->pan->status.valid = true;

    if(dw1000_start_rx(inst).start_rx_error){
        err = os_sem_release(&inst->pan->sem);
        assert(err == OS_OK);
    }

    if (mode == DWT_BLOCKING){
        err = os_sem_pend(&inst->pan->sem, OS_TIMEOUT_NEVER); // Wait for completion of transactions 
        assert(err == OS_OK);
        err = os_sem_release(&inst->pan->sem);
        assert(err == OS_OK);
    }

    return inst->status;
}


/**
 * A Personal Area Network blink request is a discovery phase in which a TAG/ANCHOR seeks to discover
 * an available PAN Master. The outcome of this process is a PANID and SLOTID assignment.
 *
 * @param inst     Pointer to dw1000_dev_instance_t.
 * @param mode     BLOCKING and NONBLOCKING modes.
 *
 * @return dw1000_pan_status_t
 */
dw1000_pan_status_t
dw1000_pan_blink(dw1000_dev_instance_t * inst, dw1000_dev_modes_t mode, uint64_t delay)
{
    os_error_t err = os_sem_pend(&inst->pan->sem,  OS_TIMEOUT_NEVER);
    assert(err == OS_OK);

    dw1000_pan_instance_t * pan = inst->pan;
    pan_frame_t * frame = pan->frames[(pan->idx)%pan->nframes];

    frame->seq_num += inst->pan->nframes;
    frame->long_address = inst->my_long_address;

    dw1000_write_tx(inst, frame->array, 0, sizeof(ieee_blink_frame_t));
    dw1000_write_tx_fctrl(inst, sizeof(ieee_blink_frame_t), 0, true);
    dw1000_set_wait4resp(inst, true);
    dw1000_set_delay_start(inst, delay);
    uint16_t timeout = dw1000_phy_frame_duration(&inst->attrib, sizeof(ieee_blink_frame_t))
                    + pan->config->rx_timeout_period
                    + pan->config->tx_holdoff_delay;
    dw1000_set_rx_timeout(inst, timeout);
    pan->status.start_tx_error = dw1000_start_tx(inst).start_tx_error;
    if (pan->status.start_tx_error){
        // Half Period Delay Warning occured try for the next epoch
        // Use seq_num to detect this on receiver size
        frame->transmission_timestamp += ((uint64_t)inst->pan->period << 15);
        os_sem_release(&inst->pan->sem);
    }
    else if(mode == DWT_BLOCKING){
        err = os_sem_pend(&inst->pan->sem, OS_TIMEOUT_NEVER); // Wait for completion of transactions
        os_sem_release(&inst->pan->sem);
        assert(err == OS_OK);
    }
   return pan->status;
}


/**
 * A Personal Area Network blink is a discovery phase in which a TAG/ANCHOR seeks to discover
 * an available PAN Master. The pan_master does not
 * need to call this function.
 *
 * @param inst    Pointer to dw1000_dev_instance_t.
 *
 * @return void
 */
void
dw1000_pan_start(dw1000_dev_instance_t * inst)
{
    dw1000_pan_instance_t * pan = inst->pan;

    os_error_t err = os_sem_pend(&pan->sem_waitforsucess, OS_TIMEOUT_NEVER);
    assert(err == OS_OK);

    pan->idx = 0x1;
    pan->status.valid = false;

    printf("{\"utime\":%lu,\"PAN\":\"%s\"}\n",
            os_cputime_ticks_to_usecs(os_cputime_get32()),
            "Provisioning"
    );
}

#endif // PAN_ENABLED
