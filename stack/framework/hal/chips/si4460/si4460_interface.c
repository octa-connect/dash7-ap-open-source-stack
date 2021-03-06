/* * OSS-7 - An opensource implementation of the DASH7 Alliance Protocol for ultra
 * lowpower wireless sensor communication
 *
 * Copyright 2015 University of Antwerp
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// *************************************************************************************************
// Radio core access functions. Taken from TI reference code for CC430.
// *************************************************************************************************

#include <stdbool.h>
#include <stdint.h>


#include "si4460_interface.h"
#include "si4460_configuration.h"
#include "ezradio_api_lib.h"
#include "ezradio_hal.h"
#include "ezradio_api_lib_add.h"

#include "ecode.h"


#include "log.h"
#include "debug.h"
#include "scheduler.h"

#include "hwsystem.h"

#define RADIO_CONFIG_DATA_RADIO_DELAY_AFTER_RESET_US (10000)

// turn on/off the debug prints
#ifdef FRAMEWORK_LOG_ENABLED // TODO more granular
    #define DPRINT(...) log_print_stack_string(LOG_STACK_PHY, __VA_ARGS__)
#else
    #define DPRINT(...)
#endif

/* Radio configuration data array. */
static const uint8_t Radio_Configuration_Data_Array[]  = RADIO_CONFIGURATION_DATA_ARRAY;

/* Radio interrupt receive flag */
static bool ezradioIrqReceived = false;

static task_t int_callback = NULL;

static void ezradioPowerUp(void);

static void GPIO_EZRadio_INT_IRQHandler( uint8_t pin );

const char *byte_to_binary(uint8_t x)
{
    static char b[9];
    b[0] = '\0';

    uint8_t z;
    for (z = 128; z > 0; z >>= 1)
    {
        strcat(b, ((x & z) == z) ? "1" : "0");
    }

    return b;
}


void ezradioInit(task_t cb)
{
  uint16_t wDelay;

  //(void)handle;

  /* Initialize radio GPIOs and SPI port */
  ezradio_hal_GpioInit( GPIO_EZRadio_INT_IRQHandler, true );
  //ezradio_hal_GpioInit( GPIO_EZRadio_INT_IRQHandler, false );
  ezradio_hal_SpiInit();

  /* Power Up the radio chip */
  ezradioPowerUp();

  /* Load radio configuration */
  while (EZRADIO_CONFIG_SUCCESS != ezradio_configuration_init(Radio_Configuration_Data_Array))
  {
    /* Error hook */
#ifdef ERROR_HOOK
    ERROR_HOOK;
#else
    DPRINT("ERROR: Radio configuration failed!\n");
#endif
    for (wDelay = 0x7FFF; wDelay--; ) ;

    /* Power Up the radio chip */
    ezradioPowerUp();
  }

  /* Read ITs, clear pending ones */
  ezradio_get_int_status(0u, 0u, 0u, NULL);

  if (cb != NULL)
  {
	  int_callback = cb;
	  sched_register_task(int_callback);
  }
}

void ezradioResetTRxFifo(void)
{
	ezradio_fifo_info(EZRADIO_CMD_FIFO_INFO_ARG_FIFO_RX_BIT, NULL);
	ezradio_fifo_info(EZRADIO_CMD_FIFO_INFO_ARG_FIFO_TX_BIT, NULL);
}


Ecode_t ezradioStartRx(uint8_t channel)
{
	ezradio_get_int_status(0u, 0u, 0u, NULL);

	// reset length of first field (can be corrupted by TX)
	ezradio_set_property(0x12, 0x02, 0x0D, 0x00, 0x01);

  /* Start Receiving packet, channel 0, START immediately, Packet n bytes long */
	//timeout: nochange
	//valid: go to ready state, if rx -> latched rssi can be overwritten or reset
	//invalid: = CRC error, ready state handled here or upper layer?
    ezradio_start_rx(channel, 0u, 0u,
                  EZRADIO_CMD_START_RX_ARG_NEXT_STATE1_RXTIMEOUT_STATE_ENUM_NOCHANGE,
                  //EZRADIO_CMD_START_RX_ARG_NEXT_STATE2_RXVALID_STATE_ENUM_RX,
                  EZRADIO_CMD_START_RX_ARG_NEXT_STATE2_RXVALID_STATE_ENUM_READY,
                  //EZRADIO_CMD_START_RX_ARG_NEXT_STATE3_RXINVALID_STATE_ENUM_RX,
                  EZRADIO_CMD_START_RX_ARG_NEXT_STATE3_RXINVALID_STATE_ENUM_READY);

    return ECODE_OK;
}

Ecode_t ezradioStartTx(hw_radio_packet_t* packet, uint8_t channel_id, bool rx_after)
{
	ezradio_cmd_reply_t ezradioReply;

	/* Request and check radio device state */
	ezradio_request_device_state(&ezradioReply);

	//TODO: what to do if already in TX
	if (ezradioReply.REQUEST_DEVICE_STATE.CURR_STATE == EZRADIO_CMD_REQUEST_DEVICE_STATE_REP_CURR_STATE_MAIN_STATE_ENUM_TX) {
	    return ECODE_EMDRV_EZRADIODRV_TRANSMIT_FAILED;
	}

	ezradio_change_state(EZRADIO_CMD_CHANGE_STATE_ARG_NEXT_STATE1_NEW_STATE_ENUM_READY);

	// clear pending interrupts
	ezradio_get_int_status_fast_clear();


	//Reset TX FIFO
	ezradio_fifo_info(EZRADIO_CMD_FIFO_INFO_ARG_FIFO_TX_BIT, NULL);

	/* Fill the TX fifo with data, CRC is added by HW*/
#ifdef HAL_RADIO_USE_HW_CRC
	ezradio_write_tx_fifo(packet->length-1, packet->data);
#else
	ezradio_write_tx_fifo(packet->length+1, packet->data);
#endif
	/* Start sending packet*/
	// RX state or idle state
	uint8_t next_state = rx_after ? 8 << 4 : 1 << 4;
#ifdef HAL_RADIO_USE_HW_CRC
	ezradio_start_tx(channel_id, next_state,  packet->length-1);
#else
	ezradio_start_tx(channel_id, next_state,  packet->length+1);
#endif

	return ECODE_EMDRV_EZRADIODRV_OK;
}

Ecode_t ezradioStartTxUnmodelated(uint8_t channel_id)
{
	ezradio_get_int_status(0u, 0u, 0u, NULL);

	/* Start sending packet, channel 0, START immediately, Packet n bytes long, go READY when done */
	ezradio_start_tx(channel_id, 0u,  0u);
}


static void ezradioPowerUp(void)
{
  /* Hardware reset the chip */
  ezradio_reset();

  /* Delay for preconfigured time */
  hw_busy_wait(RADIO_CONFIG_DATA_RADIO_DELAY_AFTER_RESET_US);

}

static void GPIO_EZRadio_INT_IRQHandler( uint8_t pin )
{
  (void)pin;

  /* Sign radio interrupt received */
  ezradioIrqReceived = true;

  if (int_callback)
	  sched_post_task(int_callback);



//  #ifdef FRAMEWORK_LOG_ENABLED // TODO more granular
//  	DPRINT("EZRadio_INT Pin: %d", pin);
//  #endif
//
//  uint8_t nirq = ezradio_hal_NirqLevel();
//
//  ezradio_cmd_reply_t ezradioReply;
//  ezradio_frr_a_read(4, &ezradioReply);
//
//  //
//  //ezradio_get_chip_status(0, &ezradioReply);
//
//  if (int_callback)
//  {
//	  ezradio_cmd_reply_t ezradioReply;
//	  ezradio_get_chip_status(0, &ezradioReply);
//
//	  int_callback();
////	  ezradio_cmd_reply_t radioReplyData;
////	  /* Read ITs, clear all pending ones */
////	  ezradio_get_int_status(0x0, 0x0, 0x0, &radioReplyData);
//
////#ifdef FRAMEWORK_LOG_ENABLED // TODO more granular
////	log_print_stack_string(LOG_STACK_PHY, "EZRadio INT");
////	log_print_data(radioReplyData.RAW, 16);
////#endif
//
//  }
}
