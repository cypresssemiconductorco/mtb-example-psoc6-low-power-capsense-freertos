/******************************************************************************
* File Name:   capsense.c
*
* Description: This file contains function definitions for performing CapSense
*              operations.
*
* Related Document: See README.md
*
*
*******************************************************************************
* Copyright 2020-2024, Cypress Semiconductor Corporation (an Infineon company) or
* an affiliate of Cypress Semiconductor Corporation.  All rights reserved.
*
* This software, including source code, documentation and related
* materials ("Software") is owned by Cypress Semiconductor Corporation
* or one of its affiliates ("Cypress") and is protected by and subject to
* worldwide patent protection (United States and foreign),
* United States copyright laws and international treaty provisions.
* Therefore, you may use this Software only as provided in the license
* agreement accompanying the software package from which you
* obtained this Software ("EULA").
* If no EULA applies, Cypress hereby grants you a personal, non-exclusive,
* non-transferable license to copy, modify, and compile the Software
* source code solely for use in connection with Cypress's
* integrated circuit products.  Any reproduction, modification, translation,
* compilation, or representation of this Software except as specified
* above is prohibited without the express written permission of Cypress.
*
* Disclaimer: THIS SOFTWARE IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, NONINFRINGEMENT, IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. Cypress
* reserves the right to make changes to the Software without notice. Cypress
* does not assume any liability arising out of the application or use of the
* Software or any product or circuit described in the Software. Cypress does
* not authorize its products for use in any products where a malfunction or
* failure of the Cypress product may reasonably be expected to result in
* significant property damage, injury or death ("High Risk Product"). By
* including Cypress's product in a High Risk Product, the manufacturer
* of such system or application assumes all risk of such use and in doing
* so agrees to indemnify Cypress against all liability.
*******************************************************************************/

/*******************************************************************************
 * Include header files
 ******************************************************************************/
#include "cybsp.h"
#include "cyhal.h"
#include "cycfg.h"
#include "cycfg_capsense.h"
#include "cy_pdl.h"

#include "capsense.h"

#include "FreeRTOS.h"
#include "timers.h"

#include <stdio.h>

/*******************************************************************************
* Macros
*******************************************************************************/
/* In this example, the CapSense scans are performed at different rates
 * depending on touch detection. The scans performed every 
 * CAPSENSE_FAST_SCAN_INTERVAL_MS are fast scans and the scans performed at 
 * CAPSENSE_SLOW_SCAN_INTERVAL_MS are slow scans.
 */
#define CAPSENSE_FAST_SCAN_INTERVAL_MS       (20U)
#define CAPSENSE_SLOW_SCAN_INTERVAL_MS       (200U)

/* Maximum number of fast scans after which the scans are initiated every
 * CAPSENSE_SLOW_SCAN_INTERVAL_MS.
 */
#define MAX_CAPSENSE_FAST_SCAN_COUNT         (100U)

/* This is the value of the variable capsense_fast_scan_count at the beginning
 * of every fast scan cycle.
 */
#define RESET_CAPSENSE_FAST_SCAN_COUNT       (1U)

/* These macros define the operations that are performed by the CPU in the
 * example. They are,
 * INITIATE_SCAN: In this state, the device initiates a CapSense scan if the
 * CapSense Hardware is not busy. After successfully starting a scan, the state
 * variable is changed to WAIT_IN_SLEEP.
 * 
 * WAIT_IN_SLEEP: In this state, the device locks deep sleep and waits for task
 * notification which informs the device that the scan has been completed and
 * the device can process the scan data. FreeRTOS takes care of putting the
 * device in sleep state while waiting for the notification. The task
 * notification changes the state variable to PROCESS_TOUCH.
 * 
 * PROCESS_TOUCH: In this state, the device processes the scan data. The widget
 * that is processed depends on the type of scan performed i.e., fast scan or
 * slow scan. After processing the touch data, the state variable is changed to
 * WAIT_IN_DEEP_SLEEP.
 * 
 * WAIT_IN_DEEP_SLEEP: In this state, the device unlocks deep sleep and waits
 * for task notification which informs the device that it is time to start the
 * next scan.  FreeRTOS takes care of putting the device in deep sleep state
 * while waiting for the notification. The task notification changes the state
 * variable to INITIATE_SCAN.
 * 
 * UNUSED_STATE: As the name states, this state is not used in the CapSense
 * operation.
 */
#define INITIATE_SCAN                        (1U)
#define WAIT_IN_SLEEP                        (2U)
#define PROCESS_TOUCH                        (3U)
#define WAIT_IN_DEEP_SLEEP                   (4U)
#define UNUSED_STATE                         (5U)


/* For PSoC 63 device, use lower data rate as IMO is used as Peri Clock source */
#if defined(CY_DEVICE_PSOC6ABLE2)
    #define I2C_SPEED_KHZ    (CYHAL_EZI2C_DATA_RATE_100KHZ)
#else
    #define I2C_SPEED_KHZ    (CYHAL_EZI2C_DATA_RATE_400KHZ)
#endif


/*******************************************************************************
 * Global variables
 ******************************************************************************/
#if (defined(CAPSENSE_TUNER_ENABLE))
static cy_stc_scb_ezi2c_context_t ezi2c_context;
static cyhal_ezi2c_t sEzI2C;
static cyhal_ezi2c_slave_cfg_t sEzI2C_sub_cfg;
static cyhal_ezi2c_cfg_t sEzI2C_cfg;
#endif /*CAPSENSE_TUNER_ENABLE*/

static TimerHandle_t  scan_timer_handle;

TaskHandle_t capsense_task_handle;

/* SysPm callback parameters for CapSense. */
static cy_stc_syspm_callback_params_t callback_params = 
{
    .base       = CYBSP_CSD_HW,
    .context    = &cy_capsense_context
};

static cy_stc_syspm_callback_t capsense_deep_sleep_cb = 
{
    Cy_CapSense_DeepSleepCallback,  
    CY_SYSPM_DEEPSLEEP,
    0,
    &callback_params,
    NULL, 
    NULL
};

/*******************************************************************************
 * Function prototypes
 ******************************************************************************/
static cy_status initialize_capsense(void);
static bool process_touch(uint32_t widget_id, uint32_t *slider_position);
static void capsense_isr(void);
static void capsense_callback(cy_stc_active_scan_sns_t *ptrActiveScan);
static void scan_timer_callback( TimerHandle_t scan_timer_handle );

#if (defined(CAPSENSE_TUNER_ENABLE))
static void initialize_capsense_tuner(void);
static void handle_ezi2c_tuner_event(void *callback_arg, cyhal_ezi2c_status_t event);
#endif /* CAPSENSE_TUNER_ENABLE */

/*******************************************************************************
 * Function definitions
 ******************************************************************************/

/*******************************************************************************
* Function Name: capsense_task
********************************************************************************
* Summary: This task performs the following the operations,
* 1. Initializes CapSense HW.
* 2. Initializes CapSense Tuner if enabled. Define ENABLE_CAPSENSE_TUNER to
* enable CapSense Tuner.
* 3. Configures and starts a timer.
* 4. Runs an FSM which handles CapSense scan, processes touch, and timing of
* sleep and deep sleep.
*  
*
* Parameters:
* void *arg: unused parameter.
*
* Return:
* 
*
*******************************************************************************/
void capsense_task(void *arg)
{
    cy_status status;
    uint32_t slider_position = 0;
    static uint32_t state = INITIATE_SCAN;
    static bool is_fast_scan_enabled = true;
    static uint32_t capsense_fast_scan_count = RESET_CAPSENSE_FAST_SCAN_COUNT;

    /* Create a timer instance which is used to inform the CPU when to start the
     * next scan. Since the example starts in fast scan, the timer period is set
     * as CAPSENSE_FAST_SCAN_INTERVAL_MS.
     */
    scan_timer_handle = xTimerCreate("Scan Timer", pdMS_TO_TICKS(CAPSENSE_FAST_SCAN_INTERVAL_MS), pdTRUE, (void *) 0, scan_timer_callback);

#if (defined(CAPSENSE_TUNER_ENABLE))
   initialize_capsense_tuner();

   /* Lock deep sleep if tuner is enabled since SCB may not be deep sleep
    * wake-up capable across all kits. Also, the current consumption is higher 
    * with the tuner enabled. It is expected to enable the tuner only when
    * configuring CapSense Widget parameters and disabled otherwise.
    */
   cyhal_syspm_lock_deepsleep();
#endif /* CAPSENSE_TUNER_ENABLE */

    status = initialize_capsense();

    if (CYRET_SUCCESS != status)
    {
        /* Halt the CPU if CapSense initialization failed. */
        CY_ASSERT(0);
    }

#if (!defined(CAPSENSE_TUNER_ENABLE))
    /* If tuner is not enabled, call Cy_CapSense_SetupWidget to set up the
     * linear slider in fast scan mode. Cy_CapSense_ScanAllWidgets is not called
     * here because it sets up and scans both the widgets used in this example
     * which results in longer scan times.
     */
    Cy_CapSense_SetupWidget(CY_CAPSENSE_LINEARSLIDER0_WDGT_ID, &cy_capsense_context);
#endif /* CAPSENSE_TUNER_ENABLE */

    if( pdPASS != xTimerStart( scan_timer_handle, 0 ) )
    {
        CY_ASSERT(0);
    }

    for (;;)
    {
        switch(state)
        {
            case INITIATE_SCAN:

                if (CY_CAPSENSE_NOT_BUSY == Cy_CapSense_IsBusy(&cy_capsense_context))
                {
                    #if (defined(CAPSENSE_TUNER_ENABLE))
                    /* Cy_CapSense_ScanAllWidgets is called when tuner is
                     * enabled to get the status of both the widgets in the
                     * CapSense Tuner. Using Cy_CapSense_SetupWidget and
                     * Cy_CapSense_Scan will update the status of only the
                     * widget set up by Cy_CapSense_SetupWidget.
                     */
                    Cy_CapSense_ScanAllWidgets(&cy_capsense_context);
                    #else
                    Cy_CapSense_Scan(&cy_capsense_context);
                    #endif /* CAPSENSE_TUNER_ENABLE */
                    state = WAIT_IN_SLEEP;
                }
                break;

            case WAIT_IN_SLEEP:

                #if (!defined(CAPSENSE_TUNER_ENABLE))
                /* If tuner is disabled lock deep sleep, and wait until scan is
                 * completed.
                 */
                cyhal_syspm_lock_deepsleep();
                #endif /* CAPSENSE_TUNER_ENABLE */

                xTaskNotifyWait(0, 0, &state, portMAX_DELAY);

                break;

            case WAIT_IN_DEEP_SLEEP:

                #if (!defined(CAPSENSE_TUNER_ENABLE))
                /* If tuner is disabled unlock deep sleep, and wait before
                 * starting next scan.
                 */
                cyhal_syspm_unlock_deepsleep();
                #endif /* CAPSENSE_TUNER_ENABLE */

                xTaskNotifyWait(0, 0, &state, portMAX_DELAY);

                break;

            case PROCESS_TOUCH:
                if(is_fast_scan_enabled)
                {
                    /* If new touch is detected, then the value of
                     * capsense_fast_scan_count is reset to
                     * RESET_CAPSENSE_FAST_SCAN_COUNT, and the slider position
                     * is displayed on the serial terminal. If not,
                     * capsense_fast_scan_count is incremented until
                     * MAX_CAPSENSE_FAST_SCAN_COUNT after which the timer period
                     * is changed to CAPSENSE_SLOW_SCAN_INTERVAL_MS and the
                     * Ganged Sensor widget is set up for the next scan.
                     */
                    if(process_touch(CY_CAPSENSE_LINEARSLIDER0_WDGT_ID, &slider_position))
                    {
                        capsense_fast_scan_count = RESET_CAPSENSE_FAST_SCAN_COUNT;
                        printf("Slider position = %ld\r\n", (unsigned long)slider_position);
                    }
                    else
                    {
                        if (MAX_CAPSENSE_FAST_SCAN_COUNT > capsense_fast_scan_count)
                        {
                            capsense_fast_scan_count++;
                        }
                        else
                        {
                            is_fast_scan_enabled = false;
                            printf("Fast scan time-out, switching to slow scan.\r\n");
                        #if (!defined(CAPSENSE_TUNER_ENABLE))
                            /* Set up Ganged Sensor widget for the next scan and
                             * change timer period to
                             * CAPSENSE_SLOW_SCAN_INTERVAL_MS only if tuner is 
                             * disabled.
                             */
                            Cy_CapSense_SetupWidget(CY_CAPSENSE_GANGEDSENSOR_WDGT_ID, &cy_capsense_context);
                        #endif
                            xTimerChangePeriod( scan_timer_handle, CAPSENSE_SLOW_SCAN_INTERVAL_MS, 0 );
                        }
                    }
                }
                else
                {
                    /* If a touch is detected for the Ganged Sensor widget, then
                     * the example switches to fast scan mode by setting up
                     * Linear slider widget and changing the timer period to 
                     * CAPSENSE_FAST_SCAN_INTERVAL_MS.
                     */
                    if(process_touch(CY_CAPSENSE_GANGEDSENSOR_WDGT_ID, &slider_position))
                    {
                        is_fast_scan_enabled = true;
                        printf("Touch detected, switching to fast scan.\r\n");

                    #if (!defined(CAPSENSE_TUNER_ENABLE))
                        /* Set up Linear Slider widget for the next scan, change
                         * capsense_fast_scan_count to 
                         * RESET_CAPSENSE_FAST_SCAN_COUNT change timer period to
                         * CAPSENSE_SLOW_SCAN_INTERVAL_MS only if tuner is 
                         * disabled.
                         */
                        Cy_CapSense_SetupWidget(CY_CAPSENSE_LINEARSLIDER0_WDGT_ID, &cy_capsense_context);
                    #endif
                        capsense_fast_scan_count = RESET_CAPSENSE_FAST_SCAN_COUNT;
                        xTimerChangePeriod( scan_timer_handle, CAPSENSE_FAST_SCAN_INTERVAL_MS, 0 );
                    }
                }

                /* Establishes synchronized operation between the CapSense
                 * middleware and the CapSense Tuner tool.
                 */
                #if (defined(CAPSENSE_TUNER_ENABLE))
                Cy_CapSense_RunTuner(&cy_capsense_context);
                #endif /* CAPSENSE_TUNER_ENABLE */
                state = WAIT_IN_DEEP_SLEEP;
                break;

            default:
                break;
        }
    }
}

/*******************************************************************************
* Function Name: process_touch
********************************************************************************
* Summary: This function processes the touch information of the widget specified
* by widget_id. The function supports processing either the 
* CY_CAPSENSE_LINEARSLIDER0_WDGT_ID or the CY_CAPSENSE_GANGEDSENSOR_WDGT_ID.
* The function returns whether a new touch is detected.
*  
*
* Parameters:
* uint32_t widget_id: The value of the CapSense Widget ID.
* uint32_t *slider_position: Pointer to variable storing slider position.
*
* Return:
* bool: Status of touch detection.
*
*******************************************************************************/
static bool process_touch(uint32_t widget_id, uint32_t *slider_position)
{
    cy_stc_capsense_touch_t *slider_touch_info;
    uint16_t slider_pos;
    uint8_t slider_touch_status;
    bool is_new_touch_detected = false;
    static uint16_t slider_pos_prev;

    /* Process all widgets if tuner is enabled. Otherwise, process the widget
     * specified by widget_id.
     */
#if (!defined(CAPSENSE_TUNER_ENABLE))
    Cy_CapSense_ProcessWidget(widget_id, &cy_capsense_context);
#else
    Cy_CapSense_ProcessAllWidgets(&cy_capsense_context);
#endif /* CAPSENSE_TUNER_ENABLE */


    switch(widget_id)
    {
        case CY_CAPSENSE_LINEARSLIDER0_WDGT_ID:
            /* Get slider status. */
            slider_touch_info = Cy_CapSense_GetTouchInfo(
            CY_CAPSENSE_LINEARSLIDER0_WDGT_ID, &cy_capsense_context);
            slider_touch_status = slider_touch_info->numPosition;
            slider_pos = slider_touch_info->ptrPosition->x;

            /* Detect the new touch on slider. */
            if ((0 != slider_touch_status) &&
                (slider_pos != slider_pos_prev))
            {
                *slider_position = slider_pos;
                is_new_touch_detected = true;
            }

            /* Update previous touch status. */
            slider_pos_prev = slider_pos;

            break;

        case CY_CAPSENSE_GANGEDSENSOR_WDGT_ID:
            /* Check if there is touch detected for the Ganged Sensor widget.*/
            if (Cy_CapSense_IsWidgetActive(CY_CAPSENSE_GANGEDSENSOR_WDGT_ID, &cy_capsense_context))
            {
                is_new_touch_detected = true;
            }

            break;

        default:
            break;
    }

    return is_new_touch_detected;
}


/*******************************************************************************
* Function Name: initialize_capsense
********************************************************************************
* Summary:
*  This function performs the following operations,
*  1. Initializes CapSense HW,
*  2. Configure the CapSense interrupt,
*  3. Registers deep sleep callback for CapSense HW, and
*  4. Registers a callback to indicate end of scan.
*
*******************************************************************************/
static cy_status initialize_capsense(void)
{
    cy_status status = CYRET_SUCCESS;

    /* CapSense interrupt configuration. */
    const cy_stc_sysint_t CapSense_interrupt_config =
    {
        .intrSrc = CYBSP_CSD_IRQ,
        .intrPriority = CAPSENSE_INTR_PRIORITY,
    };

    /* Capture the CSD HW block and initialize it to the default state. */
    status = Cy_CapSense_Init(&cy_capsense_context);
    if (CYRET_SUCCESS != status)
    {
        return status;
    }

    /* Initialize CapSense interrupt. */
    Cy_SysInt_Init(&CapSense_interrupt_config, capsense_isr);
    NVIC_ClearPendingIRQ(CapSense_interrupt_config.intrSrc);
    NVIC_EnableIRQ(CapSense_interrupt_config.intrSrc);

    /* Initialize the CapSense firmware modules. */
    status = Cy_CapSense_Enable(&cy_capsense_context);
    if (CYRET_SUCCESS != status)
    {
        return status;
    }

    /* Register a deep sleep callback for CapSense block. */
    Cy_SysPm_RegisterCallback(&capsense_deep_sleep_cb);

    /* Assign a callback function to indicate end of CapSense scan. */
    status = Cy_CapSense_RegisterCallback(CY_CAPSENSE_END_OF_SCAN_E,
            capsense_callback, &cy_capsense_context);
    if (CYRET_SUCCESS != status)
    {
        return status;
    }

    return status;
}


/*******************************************************************************
* Function Name: capsense_isr
********************************************************************************
* Summary:
*  Wrapper function for handling interrupts from CapSense block.
*
*******************************************************************************/
static void capsense_isr(void)
{
    Cy_CapSense_InterruptHandler(CYBSP_CSD_HW, &cy_capsense_context);
}


/*******************************************************************************
* Function Name: capsense_callback()
********************************************************************************
* Summary:
*  This function writes to the notification value duirng task notification to 
*  indicate end of scan.
*
* Parameters:
*  cy_stc_active_scan_sns_t* : pointer to active sensor details.
*
*******************************************************************************/
static void capsense_callback(cy_stc_active_scan_sns_t * ptrActiveScan)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    uint32_t notify_state_change = PROCESS_TOUCH;

    /* Notify the capsense_task that scan has completed. */
    xTaskNotifyFromISR(capsense_task_handle, notify_state_change, eSetValueWithOverwrite,
                       &xHigherPriorityTaskWoken);

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/*******************************************************************************
* Function Name: scan_timer_callback()
********************************************************************************
* Summary:
*  This function is called when the software timer elapses. This function writes
*  to the notification value duirng task notification to indicate start of a new
*  scan.
*
* Parameters:
*  TimerHandle_t scan_timer_handle : pointer to the scan timer handle.
*
*******************************************************************************/
static void scan_timer_callback( TimerHandle_t scan_timer_handle )
{
    uint32_t notify_state_change = INITIATE_SCAN;

    /* Notify capsense_task to start a new scan. */
    xTaskNotify(capsense_task_handle, notify_state_change, eSetValueWithOverwrite);
}


#if (defined(CAPSENSE_TUNER_ENABLE))
/*******************************************************************************
* Function Name: ezi2c_isr
********************************************************************************
* Summary:
*  Wrapper function for handling interrupts from EZI2C block.
*
*******************************************************************************/
static void handle_ezi2c_tuner_event(void *callback_arg, cyhal_ezi2c_status_t event)
{
    cyhal_ezi2c_status_t status;
    cy_stc_scb_ezi2c_context_t *context = &ezi2c_context;

    /* Get the slave interrupt sources. */
    status = cyhal_ezi2c_get_activity_status(&sEzI2C);

    /* Handle the error conditions. */
    if (0UL != (CYHAL_EZI2C_STATUS_ERR & status))
    {
        CY_ASSERT(0);
    }

    /* Handle the receive direction (master writes data). */
    if (0 != (CYHAL_EZI2C_STATUS_READ1 & status))
    {
        cyhal_i2c_slave_config_write_buffer((cyhal_i2c_t *)&sEzI2C, context->curBuf, context->bufSize);
    }
    /* Handle the transmit direction (master reads data). */
    if (0 != (CYHAL_EZI2C_STATUS_WRITE1 & status))
    {
        cyhal_i2c_slave_config_read_buffer((cyhal_i2c_t *)&sEzI2C, context->curBuf, context->bufSize);
    }
}


/*******************************************************************************
* Function Name: initialize_capsense_tuner
********************************************************************************
* Summary:
*  Initializes communication between Tuner GUI and PSoC 6 MCU.
*
*******************************************************************************/
static void initialize_capsense_tuner(void)
{
    cy_rslt_t result;

    /* Configure Capsense Tuner as EzI2C Slave. */
    sEzI2C_sub_cfg.buf = (uint8 *)&cy_capsense_tuner;
    sEzI2C_sub_cfg.buf_rw_boundary = sizeof(cy_capsense_tuner);
    sEzI2C_sub_cfg.buf_size = sizeof(cy_capsense_tuner);
    sEzI2C_sub_cfg.slave_address = 8U;

    /* Configure EzI2C block parameters. */
    sEzI2C_cfg.data_rate = I2C_SPEED_KHZ;
    sEzI2C_cfg.enable_wake_from_sleep = false;
    sEzI2C_cfg.slave1_cfg = sEzI2C_sub_cfg;
    sEzI2C_cfg.sub_address_size = CYHAL_EZI2C_SUB_ADDR16_BITS;
    sEzI2C_cfg.two_addresses = false;
    result = cyhal_ezi2c_init( &sEzI2C, CYBSP_I2C_SDA, CYBSP_I2C_SCL, NULL, &sEzI2C_cfg);

    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    cyhal_ezi2c_register_callback( &sEzI2C, handle_ezi2c_tuner_event, NULL);
    cyhal_ezi2c_enable_event(&sEzI2C,
                             (cyhal_ezi2c_status_t)(CYHAL_EZI2C_STATUS_ERR | CYHAL_EZI2C_STATUS_WRITE1 | CYHAL_EZI2C_STATUS_READ1),
                             EZI2C_INTERRUPT_PRIORITY, true);

}
#endif /* CAPSENSE_TUNER_ENABLE */

/* [] END OF FILE */
