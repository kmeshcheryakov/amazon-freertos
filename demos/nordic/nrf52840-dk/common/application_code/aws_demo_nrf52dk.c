/*
 * Amazon FreeRTOS
 * Copyright (C) 2017 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * http://aws.amazon.com/freertos
 * http://www.FreeRTOS.org
 */

/**
 * @file aws_demo_nrf52dk.c
 * @brief Simple MQTT publish example.
 *
 * This application publishes MQTT "HelloWorld" messages to a topic using AWS Iot MQTT broker. It receives
 * acknowledgment packets from the broker and echoes back the message to the broker.
 *
 * The demo uses underlying network agnostic MQTT library to perform all MQTT operations. Hence the demo can
 * run over both a secured TCP/IP connection over WIFI network as well as over a secured BLE connection, using
 * a Mobile phone as MQTT proxy to the cloud.
 */

/* Standard includes. */
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
//#include "stdint.h"
//#include "stdarg.h"
//#include "stdio.h"

/* Build using a config header, if provided. */
#include "aws_iot_demo.h"

/*
 * FreeRTOS header includes
 */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "timers.h"
//#include "message_buffer.h"

/* MQTT library header includes */
#include "aws_iot_mqtt.h"

/* POSIX and Platform layer includes. */
#include "platform/aws_iot_clock.h"
#include "FreeRTOS_POSIX/time.h"
#include "FreeRTOS_POSIX/pthread.h"

/* Network connection includes */
#include "aws_iot_network.h"
#include "aws_iot_demo_network.h"
#include "aws_clientcredential.h"

/* Nordic BSP includes */
#include "bsp.h"
#include "nordic_common.h"
#include "sdk_errors.h"
#include "nrf_drv_wdt.h"
#include "nrf_drv_saadc.h"
#include "nrf_drv_timer.h"
#include "nrf_drv_clock.h"
#include "app_error.h"
#include "app_timer.h"

/*-----------------------------------------------------------*/

/* Amazon FreeRTOS OTA agent includes. */
#include "aws_ota_agent.h"

/* Required for demo task stack and priority */
#include "aws_ota_update_demo.h"
#include "aws_demo_config.h"
#include "aws_application_version.h"

#define otademoCONN_TIMEOUT_MS 2000UL

#define echoKEEPALIVE_SECONDS              ( 120 )

#define myappONE_SECOND_DELAY_IN_TICKS  pdMS_TO_TICKS( 1000UL )

static const char *pcStateStr[eOTA_NumAgentStates] =
{
     "Not Ready",
     "Ready",
     "Active",
     "Shutting down"
};

/*-----------------------------------------------------------*/

typedef struct Measurements {
    uint32_t temp;
    uint32_t humidity;
} xMeasurements_t;

/* Device states enumeration */
typedef enum
{
    eDeviceState_MQTT_Inactive = 0,
    eDeviceState_MQTT_WaitingForConnection,
    eDeviceState_MQTT_Disconnected,
    eDeviceState_MQTT_Connected,
} DeviceState_t;

/* LED definitions*/
#define LED_ONE                             BSP_LED_0_MASK
#define LED_TWO                             BSP_LED_1_MASK
#define LED_THREE                           BSP_LED_2_MASK
#define LED_FOUR                            BSP_LED_3_MASK
#define ALL_APP_LED                        (BSP_LED_0_MASK | BSP_LED_1_MASK | \
                                            BSP_LED_2_MASK | BSP_LED_3_MASK)            /**< Define used for simultaneous operation of all application LEDs. */
#define LED_BLINK_INTERVAL_MS               ( 300 )                                     /**< LED blinking interval. */

/* Timers definitions */
#define DEMO_TIMER_TICKS( MS )              ( ( uint32_t ) ROUNDED_DIV( ( MS ) * configTICK_RATE_HZ, 1000 ) )
#define TIMER_PERIOD_MS                     ( 1000 )
#define TIMER_RESOLUTION_IN_MS              ( 10 )                                      /**< Timer period. */
#define TIMER_FULL                          ( 0xFFFFFFFFUL )
#define TIMER_CONNECT                       ( 0 )       /* ms */
#define TIMER_PUBLISH                       ( 1 )       /* ms */
APP_TIMER_DEF(m_timer_tick_src_id);

/* Buttons definitions */
#define BUTTON_ONE                          ( 1 )
#define BUTTON_TWO                          ( 2 )
#define BUTTON_THREE                        ( 3 )
#define BUTTON_FOUR                         ( 4 )
#define BUTTON_DETECTION_DELAY              DEMO_TIMER_TICKS( 100 )

/* Minimum mesurement voltage */
#define MIN_VCC                             ( 2000 )    /* mV */

/**
 * @brief Dimension of the character array buffers used to hold data (strings in
 * this case) that is published to and received from the MQTT broker (in the cloud).
 */
#define demoDATA "%d"
#define demoMAX_DATA_LENGTH    ( 4 )
/* MQTT timeouts */
#define demoTIMEOUT_CONNECT                     ( 60000 )   /* ms */
#define demoTIMEOUT_PUBLISH                     ( 5000 )    /* ms */

/**
 * @brief The topic that the MQTT client both subscribes and publishes to.
 */
#define QUEUE_MSG_LENGTH                    ( 6 )

#define CLIENT_ID         ( clientcredentialIOT_THING_NAME )
//
///**
// * @brief The topic that the MQTT client both subscribes and publishes to.
// */
#define demoTOPIC_NAME         ( ( const uint8_t * ) "nrf52dk/"clientcredentialIOT_THING_NAME )

#define demoTOPIC_NAME_HEALTH  ( ( const uint8_t * ) "nrf52dk/"clientcredentialIOT_THING_NAME"/health" )

/**
 * @brief The string appended to messages that are echoed back to the MQTT broker.
 *
 * It is also used to detect if a received message has already been acknowledged.
 */
#define demoACK_STRING                     " ACK"

#define demoACK_LENGTH                     ( 4 )

#define demoMAX_ACK_MSG_LENGTH             ( demoMAX_DATA_LENGTH + demoACK_LENGTH )

#define demoQOS                            ( 0 )

#define demoKEEPALIVE_SECONDS              ( 120 )

#define demoNUM_MESSAGES                   ( 120 )

#define demoMSG_INTERVAL_SECONDS           ( 1 )

#define demoMQTT_TIMEOUT_MS                ( 5000 )

#define demoCONN_RETRY_INTERVAL_SECONDS    ( 5 )

#define demoCONN_RETRY_LIMIT               ( 100 )

#define demoTASK_STACK_SIZE                ( configMINIMAL_STACK_SIZE * 8 )

#define demoTASK_PRIORITY                  ( tskIDLE_PRIORITY + 1 )

#define demoCLEAR_ALL_BITS                 ( ( uint32_t ) 0xFFFFFFFFUL )

/**
 * @brief Opens a new MQTT connection with the IOT broker endpoint.
 *
 * Function first creates a network connection which can either be a Bluetooth Low Energy connection with
 * a compatible device or a secure socket connection over WIFI. It then performs MQTT connect with the broker
 * endpoint, and subscribes to the demo topic configured.
 *
 * @return true if all the steps succeeded.
 */
bool prbOpenMqttConnection( void );
/**
 * @brief Closes an MQTT connection with the broker endpoint.
 *
 * Function first closes the MQTT connection with the broker by optionally (set in the parameter) sending a
 * DISCONNECT message to the broker. It also tears down the physical connectivity betweend the device and the
 * broker.
 *
 * @param bSendDisconnect[in] Should send a DISCONNECT message to the broker.
 */
void prvCloseMqttConnection( bool bSendDisconnect );
/**
 * @brief Publishes a message using QoS0 or QoS1 to the specified topic.
 *
 * @param pcMesg Pointer to the message
 * @param xLength Length of the message
 * @param pucTopicName is a name of MQTT Topic at Broker
 * @return AWS_IOT_MQTT_SUCCESS if the publish was successful.
 */
static AwsIotMqttError_t prxPublishMessage( const char* pcMesg, size_t xLength, const uint8_t * pucTopicName );

/**
 * @brief Thread used to publish all the messages to the topic.
 * @param pvParam[in] Thread parameter
 */
static void prvPublishMessageTask( void* pvParam );

/**
 * @brief Creates and starts a publish message thread in detached state.
 * @return true if thread was started successfully.
 */
static bool prbStartPublishMessageTask( void);
/**
 * @brief Callback invoked when a DISCONNECT event was received for the connected network.
 *
 * @param xConnection The handle for the connection to the network.
 */
static void prvOnNetworkDisconnect( AwsIotDemoNetworkConnection_t xConnection );

/* Prepare timers */
/* #define demoUSE_ANTIFREEZE_TIMERS */
#ifdef demoUSE_ANTIFREEZE_TIMERS
static void timer_switch(uint32_t timer);
#endif

/* Initializes the board. */
static ret_code_t prvPeripheryInit( void );

/* Declaration of snprintf. The header stdio.h is not included because it
 * includes conflicting symbols on some platforms. */
extern int snprintf( char * pcS,
                     size_t xN,
                     const char * pcFormat,
                     ... );
/**
 * @brief Underlying network interface used for the MQTT connection.
 */
static AwsIotMqttNetIf_t xNetworkInterface = AWS_IOT_MQTT_NETIF_INITIALIZER;
/**
 * @brief Underlying network Connection used for the MQTT connection.
 */
static AwsIotDemoNetworkConnection_t xNetworkConnection = NULL;
/**
 * @brief Handle to the MQTT connection.
 */
static AwsIotMqttConnection_t xMqttConnection = AWS_IOT_MQTT_CONNECTION_INITIALIZER;
/**
 * @brief Global variable used to indiacte if the underlying connection is active.
 */
static bool xConnected = false;
/**
 * @brief Task Handle to notify of button have been pressed
 */
static TaskHandle_t xPublishMsgTaskHandle = NULL;

/* Mutex in order to concurrent update eDeviceState */
SemaphoreHandle_t xDeviceStateMutex = NULL;

/* Device state to update led indication */
DeviceState_t eDeviceState = eDeviceState_MQTT_Inactive;

/* AntiFreeze Timers handlers */
#ifdef demoUSE_ANTIFREEZE_TIMERS
const nrf_drv_timer_t xTimer1 = NRF_DRV_TIMER_INSTANCE(1);
const nrf_drv_timer_t xTimer2 = NRF_DRV_TIMER_INSTANCE(2);
#endif

/* Watch Dog Timer channel ID */
nrf_drv_wdt_channel_id xWatchDogChannel;

/*-----------------------------------------------------------*/
static OTA_State_t eOtaState;
static void App_OTACompleteCallback(OTA_JobEvent_t eEvent );

/* The OTA agent has completed the update job or determined that we're in
 * self test mode. If it was accepted, we want to activate the new image.
 * This typically means we should reset the device to run the new firmware.
 * If now is not a good time to reset the device, it may be activated later
 * by your user code. If the update was rejected, just return without doing
 * anything and we'll wait for another job. If it reported that we should
 * start test mode, normally we would perform some kind of system checks to
 * make sure our new firmware does the basic things we think it should do
 * but we'll just go ahead and set the image as accepted for demo purposes.
 * The accept function varies depending on your platform. Refer to the OTA
 * PAL implementation for your platform in aws_ota_pal.c to see what it
 * does for you.
 */

static void App_OTACompleteCallback( OTA_JobEvent_t eEvent )
{
	OTA_Err_t xErr = kOTA_Err_Uninitialized;
	
    if ( eEvent == eOTA_JobEvent_Activate )
    {
        configPRINTF( ( "Received eOTA_JobEvent_Activate callback from OTA Agent.\r\n" ) );
        OTA_ActivateNewImage();
    }
    else if (eEvent == eOTA_JobEvent_Fail)
    {
        configPRINTF( ( "Received eOTA_JobEvent_Fail callback from OTA Agent.\r\n" ) );
        /* Nothing special to do. The OTA agent handles it. */
    }
	else if (eEvent == eOTA_JobEvent_StartTest)
	{
		/* This demo just accepts the image since it was a good OTA update and networking
		 * and services are all working (or we wouldn't have made it this far). If this
		 * were some custom device that wants to test other things before calling it OK,
		 * this would be the place to kick off those tests before calling OTA_SetImageState()
		 * with the final result of either accepted or rejected. */
        configPRINTF( ( "Received eOTA_JobEvent_StartTest callback from OTA Agent.\r\n" ) );
	xErr = OTA_SetImageState (eOTA_ImageState_Accepted);
        if( xErr != kOTA_Err_None )
        {
            OTA_LOG_L1( " Error! Failed to set image state as accepted.\r\n" );    
        }
	}
}

/*-----------------------------------------------------------*/

AwsIotMqttError_t prxPublishMessage( const char* pcMesg, size_t xLength, const uint8_t * pucTopicName )
{
    AwsIotMqttPublishInfo_t xPublishInfo = AWS_IOT_MQTT_PUBLISH_INFO_INITIALIZER;
    AwsIotMqttReference_t xOperationLock = AWS_IOT_MQTT_REFERENCE_INITIALIZER;
    AwsIotMqttError_t xStatus;

    xPublishInfo.QoS = demoQOS;
    xPublishInfo.pTopicName = pucTopicName;
    xPublishInfo.topicNameLength = strlen( pucTopicName );

    xPublishInfo.pPayload = ( void * ) pcMesg;
    xPublishInfo.payloadLength = xLength;
    xPublishInfo.retryLimit = 0;
    xPublishInfo.retryMs = 0;

    if( demoQOS == 0 )
    {
        xStatus = AwsIotMqtt_Publish( xMqttConnection,
                &xPublishInfo,
                0,
                NULL,
                NULL );
    }
    else if( demoQOS == 1 || demoQOS == 2 )
    {
        xStatus = AwsIotMqtt_Publish( xMqttConnection,
                &xPublishInfo,
                AWS_IOT_MQTT_FLAG_WAITABLE,
                NULL,
                &xOperationLock );
        if( xStatus == AWS_IOT_MQTT_STATUS_PENDING )
        {
            xStatus = AwsIotMqtt_Wait( xOperationLock, demoMQTT_TIMEOUT_MS );
        }
    }

    return xStatus;
}

bool prbOpenMqttConnection( void )
{
    AwsIotMqttConnectInfo_t xConnectInfo = AWS_IOT_MQTT_CONNECT_INFO_INITIALIZER;
    bool xStatus = false;

    xSemaphoreTake( xDeviceStateMutex, portMAX_DELAY );
    eDeviceState = eDeviceState_MQTT_WaitingForConnection;
    xSemaphoreGive( xDeviceStateMutex );

    AwsIotDemo_CreateNetworkConnection(
            &xNetworkInterface,
            &xMqttConnection,
            prvOnNetworkDisconnect, //NULL,
            &xNetworkConnection,
            demoCONN_RETRY_INTERVAL_SECONDS, //echoCONN_RETRY_INTERVAL_SECONDS,
            demoCONN_RETRY_LIMIT );

    if( xNetworkConnection != NULL )
    {
       /*
        * If the network type is BLE, MQTT library connects to the IoT broker using
        * a proxy device as intermediary. So set .awsIotMqttMode to false. Disable keep alive
        * by setting keep alive seconds to zero.
        */
        configPRINTF( ( "Connecting to broker...\r\n" ) );
        if( AwsIotDemo_GetNetworkType( xNetworkConnection ) == AWSIOT_NETWORK_TYPE_BLE )
        {
            xConnectInfo.awsIotMqttMode = false;
            xConnectInfo.keepAliveSeconds = 0;
        }
        else
        {
            xConnectInfo.keepAliveSeconds = echoKEEPALIVE_SECONDS;
            xConnectInfo.awsIotMqttMode = true;
            xConnectInfo.keepAliveSeconds = demoKEEPALIVE_SECONDS;
        }

        xConnectInfo.cleanSession = true;
        xConnectInfo.clientIdentifierLength = strlen( clientcredentialIOT_THING_NAME );
        xConnectInfo.pClientIdentifier = clientcredentialIOT_THING_NAME;

        /* Connect to the IoT broker endpoint */
        if( AwsIotMqtt_Connect( &xMqttConnection,
                &xNetworkInterface,
                &xConnectInfo,
                NULL,
                demoMQTT_TIMEOUT_MS ) == AWS_IOT_MQTT_SUCCESS )
        {
            configPRINTF( ( "Connected to broker.\r\n" ) );
            xStatus = true;
            xSemaphoreTake( xDeviceStateMutex, portMAX_DELAY );
            eDeviceState = eDeviceState_MQTT_Connected;
            xSemaphoreGive( xDeviceStateMutex );
            if( eOtaState == eOTA_AgentState_NotReady )
            {
                eOtaState = OTA_AgentInit( xMqttConnection, ( const uint8_t * ) ( clientcredentialIOT_THING_NAME ), App_OTACompleteCallback, ( TickType_t ) ~0 );
                if( eOtaState != eOTA_AgentState_Ready )
                {
                    AwsIotLogError( "Failed to Activate OTA agent\r\n" );
                }
            }
        }

        if( xStatus == false )
        {
            configPRINTF( ( "ERROR:  AwsIotMqtt_Connect() Failed\r\n" ) );
            /* Close the MQTT connection to perform any cleanup */
            prvCloseMqttConnection( true );
        }

    }
    else
    {
        configPRINTF( ( "Failed to create Network Connection\r\n" ) );
    }

    return xStatus;
}

void prvCloseMqttConnection( bool bSendDisconnect )
{
    xSemaphoreTake( xDeviceStateMutex, portMAX_DELAY );
    eDeviceState = eDeviceState_MQTT_Disconnected;
    xSemaphoreGive( xDeviceStateMutex );
    /* Close the MQTT connection either by sending a DISCONNECT operation or not */
    
    eOtaState = OTA_AgentShutdown( myappONE_SECOND_DELAY_IN_TICKS );
    if( eOtaState != eOTA_AgentState_NotReady )
    {
        AwsIotLogError( "Could not shutdown OTA Agent\r\n" );
    }

    if( xMqttConnection != AWS_IOT_MQTT_CONNECTION_INITIALIZER )
    {
        AwsIotMqtt_Disconnect( xMqttConnection, !(bSendDisconnect) );
        xMqttConnection = AWS_IOT_MQTT_CONNECTION_INITIALIZER;
    }
    /* Delete the network connection */
    if( xNetworkConnection != NULL )
    {
    	AwsIotDemo_DeleteNetworkConnection( xNetworkConnection );
        xNetworkConnection = NULL;
    }
}

static void prvOnNetworkDisconnect( AwsIotDemoNetworkConnection_t xConnection )
{
    BaseType_t lHigherPriorityTaskWoken = pdFALSE;
    
    xSemaphoreTakeFromISR( xDeviceStateMutex, &lHigherPriorityTaskWoken );
    eDeviceState = eDeviceState_MQTT_Disconnected;
    xSemaphoreGiveFromISR( xDeviceStateMutex, &lHigherPriorityTaskWoken );
    xConnected = false;
    portEND_SWITCHING_ISR( lHigherPriorityTaskWoken );
}

static void prvPublishMessageTask( void* pvParam )
{
    UNUSED_VARIABLE( pvParam );
    uint32_t ulButton;
    BaseType_t xResult;
    const TickType_t xPublishMsgPeriod = pdMS_TO_TICKS( 2000 );
    AwsIotMqttError_t xError = AWS_IOT_MQTT_SUCCESS;
    char cMessage[ demoMAX_DATA_LENGTH ];
    size_t xMessageLength;
    struct timespec xMsgInterval =
    {
            .tv_sec = demoMSG_INTERVAL_SECONDS,
            .tv_nsec = 0
    };

/* TODO: UNCOMMENT FOLLOWING */
    xConnected = prbOpenMqttConnection();

    for( ; ; )
    {
        /* Wait for a button is pressed notification for a xPublishMsgPeriod. */
        /* If it's not occurred, proceed with default value to HEALTH topic */
        /* and clear notification value at exit */
        xResult = xTaskNotifyWait( 0, demoCLEAR_ALL_BITS, &ulButton, xPublishMsgPeriod );
        
        xMessageLength = snprintf( cMessage, demoMAX_DATA_LENGTH, demoDATA, ulButton );
        
        /* Reset to default value */
        ulButton = 0;
        
        if( xConnected )
        {
            if( xResult == pdPASS )
            {
                xError = prxPublishMessage( cMessage, xMessageLength, demoTOPIC_NAME );
            }
            else
            {
                xError = prxPublishMessage( cMessage, xMessageLength, demoTOPIC_NAME_HEALTH );
            }

            if( xError == AWS_IOT_MQTT_SUCCESS)
            {
                AwsIotLogInfo( "Sent: %.*s", xMessageLength, cMessage );
                ( void ) clock_nanosleep( CLOCK_REALTIME, 0, &xMsgInterval, NULL );
            }
            else
            {
                if( ( xError == AWS_IOT_MQTT_NO_MEMORY ) || ( xError == AWS_IOT_MQTT_BAD_PARAMETER ) )
                {
                    AwsIotLogError( "Failed to publish Message, error = %s", AwsIotMqtt_strerror( xError ) );
                    break;
                }
                else
                {
                    prvCloseMqttConnection(false );
                    xConnected = prbOpenMqttConnection();
                }
            }

            eOtaState = OTA_GetAgentState();
            if( ( eOtaState == eOTA_AgentState_Ready ) || ( eOtaState == eOTA_AgentState_Active ) )
            {
                configPRINTF( ( "State: %s  Received: %u   Queued: %u   Processed: %u   Dropped: %u\r\n", 
                              pcStateStr[eOtaState],
                              OTA_GetPacketsReceived(), 
                              OTA_GetPacketsQueued(), 
                              OTA_GetPacketsProcessed(), 
                              OTA_GetPacketsDropped() ) );
            }
        }
        else
        {
            prvCloseMqttConnection( false );
            xConnected = prbOpenMqttConnection();
        }
    }

    vTaskDelete(NULL);
}

static bool prbStartPublishMessageTask( void )
{
    ret_code_t xErrCode = NRF_SUCCESS;
    xDeviceStateMutex = xSemaphoreCreateMutex();

    /* Perform any hardware initialization that does not require the RTOS to be running */
    xErrCode = prvPeripheryInit();
    APP_ERROR_CHECK(xErrCode);
    
    if( pdPASS != xTaskCreate( prvPublishMessageTask,
                               "Publish Message Task",
                               demoTASK_STACK_SIZE,
                               NULL,
                               demoTASK_PRIORITY,
                               &xPublishMsgTaskHandle ) )
    {
        xErrCode = NRF_ERROR_NO_MEM;
    }
    else
    {
        xErrCode = pdPASS;
    }
    return xErrCode;
}

/*-----------------------------------------------------------*/

void vStartMQTTBLESensorsDemo( void )
{
    if( prbStartPublishMessageTask() == pdFAIL )
    {
        AwsIotLogInfo( "Failed to create task for publish measurements\n");
        return;
    }
}
/*-----------------------------------------------------------*/

/**@brief Saadc callback.
 *
 * @param[in]   p_event   The event of the saadc triggered the callback.
 */
void saadc_callback(nrf_drv_saadc_evt_t const * p_event)
{
    if(p_event->type == NRF_DRV_SAADC_EVT_DONE)
    {
        /*
        * Do nothing
        */
    }
    if(p_event->type == NRF_DRV_SAADC_EVT_LIMIT)
    {
        /*
        * Do nothing
        */
    }
}
/*-----------------------------------------------------------*/

/**
 * @brief WDT events handler.
 */
void wdt_event_handler(void)
{
    LEDS_OFF(ALL_APP_LED);
}
/*-----------------------------------------------------------*/

/**@brief Function for the LEDs initialization.
 *
 * @details Initializes all LEDs used by this application.
 */
static void leds_init(void)
{
    // Configure application LED pins.
    LEDS_CONFIGURE(ALL_APP_LED);

    // Turn off all LED on initialization.
    LEDS_OFF(ALL_APP_LED);

    LEDS_ON(LED_FOUR);
}
/*-----------------------------------------------------------*/

/**
 * @brief Timer callback used for controlling board LEDs to represent application state.
 */
static void blink_timeout_handler()
{
    int32_t vcc;
    nrf_saadc_value_t saadc_sample;

    switch ( eDeviceState )
    {
        case eDeviceState_MQTT_Inactive:
            ;
        case eDeviceState_MQTT_Disconnected:
            LEDS_OFF(ALL_APP_LED);
            break;

        case eDeviceState_MQTT_WaitingForConnection:
            #ifdef demoUSE_ANTIFREEZE_TIMERS
            timer_switch( (uint32_t) TIMER_CONNECT );
            #endif
            LEDS_INVERT( LED_ONE );
            break;

        case eDeviceState_MQTT_Connected:
            #ifdef demoUSE_ANTIFREEZE_TIMERS
            timer_switch( (uint32_t) TIMER_PUBLISH );
            #endif
            LEDS_ON( LED_ONE );
            break;

        default:
            break;
    }

    nrf_drv_saadc_sample_convert( 0, &saadc_sample);
    /*
     * Resolusion of ADC - 10 bit, with maximum value 3600mV
     * 1023 - 3600
     *    0 -    0
     */
    vcc = saadc_sample * 3600 / 1023;
    /* configPRINTF( ( "VCC SAMPLE: %d\r\n", vcc ) ); */
    
    if( vcc < MIN_VCC )
    {
        LEDS_INVERT( LED_THREE );
    }

    nrf_drv_wdt_feed();
}
/*-----------------------------------------------------------*/

/**@brief Buttons event handler.
 *
 * @param[in]   pin_no   Button number.
 * @param[in]   button_action   Button action.
 */
static void button_event_handler(uint8_t pin_no, uint8_t button_action)
{
    BaseType_t lHigherPriorityTaskWoken = pdFALSE;
    eNotifyAction eAction = eSetValueWithoutOverwrite;
    uint32_t ulButton;

    LEDS_INVERT( LED_FOUR );

    if (button_action == APP_BUTTON_PUSH)
    {
        switch (pin_no)
        {
            case BSP_BUTTON_0:
                ulButton = BUTTON_ONE;
                break;

            case BSP_BUTTON_1:
                ulButton = BUTTON_TWO;
                break;

            case BSP_BUTTON_2:
                ulButton = BUTTON_THREE;
                break;

            case BSP_BUTTON_3:
                ulButton = BUTTON_FOUR;
                break;

            default:
                break;
        }

        UNUSED_VARIABLE( xTaskNotifyFromISR( xPublishMsgTaskHandle,
                                         ulButton,
                                         eAction,
                                         &lHigherPriorityTaskWoken ) );
    }
    portEND_SWITCHING_ISR( lHigherPriorityTaskWoken );
}
/*-----------------------------------------------------------*/

/**
 * @brief Initializes the buttons.
 * 
 * @return   NRF_SUCCESS on success, otherwise an error code.
 */
static ret_code_t button_init(void)
{
    ret_code_t err_code = NRF_SUCCESS;

    static app_button_cfg_t buttons[] =
    {
        { BSP_BUTTON_0, false, BUTTON_PULL, button_event_handler },
        { BSP_BUTTON_1, false, BUTTON_PULL, button_event_handler },
        { BSP_BUTTON_2, false, BUTTON_PULL, button_event_handler },
        { BSP_BUTTON_3, false, BUTTON_PULL, button_event_handler },
    };

    err_code = app_button_init(buttons, sizeof(buttons) / sizeof(buttons[0]), BUTTON_DETECTION_DELAY );
    APP_ERROR_CHECK(err_code);

    err_code = app_button_enable();
    APP_ERROR_CHECK(err_code);

    return  err_code;
}
/*-----------------------------------------------------------*/

/**@brief Function for updating the wall clock of the Timer module.
 *
 * @param[in]   p_context   Pointer used for passing context. No context used in this application.
 */
static void timer_tick_callback(void * p_context)
{
    UNUSED_VARIABLE(p_context);
    blink_timeout_handler();
}
/*-----------------------------------------------------------*/

/**@brief Function for the Timer initialization.
 *
 * @details Initializes the timer module. This creates and starts application timers.
 * 
 * @return   NRF_SUCCESS on success, otherwise an error code.
 */
static ret_code_t timers_init(void)
{
    ret_code_t err_code = NRF_SUCCESS;

    // Initialize timer module.
    APP_ERROR_CHECK(app_timer_init());

    // Create a sys timer.
    err_code = app_timer_create(&m_timer_tick_src_id,
                                APP_TIMER_MODE_REPEATED,
                                timer_tick_callback);
    APP_ERROR_CHECK(err_code);

    err_code = app_timer_start(m_timer_tick_src_id,
                               DEMO_TIMER_TICKS( TIMER_PERIOD_MS ),
                               NULL);

    APP_ERROR_CHECK(err_code);

    return  err_code;
}
/*-----------------------------------------------------------*/

/**
 * @brief Initializes the wdt.
 * 
 * @return   NRF_SUCCESS on success, otherwise an error code.
 */
static ret_code_t wdt_init(void)
{
    ret_code_t err_code = NRF_SUCCESS;

    nrf_drv_wdt_config_t config = NRF_DRV_WDT_DEAFULT_CONFIG;
    err_code = nrf_drv_wdt_init(&config, wdt_event_handler);
    APP_ERROR_CHECK(err_code);
    err_code = nrf_drv_wdt_channel_alloc(&xWatchDogChannel);
    APP_ERROR_CHECK(err_code);
    nrf_drv_wdt_enable();

    return  err_code;
}
/*-----------------------------------------------------------*/

/**
 * @brief Initializes the saadc.
 * 
 * @return   NRF_SUCCESS on success, otherwise an error code.
 */
static ret_code_t saadc_init( void )
{
    ret_code_t err_code = NRF_SUCCESS;

    nrf_saadc_channel_config_t channel_config =
    NRF_DRV_SAADC_DEFAULT_CHANNEL_CONFIG_SE( NRF_SAADC_INPUT_VDD );

    err_code = nrf_drv_saadc_init( NULL, saadc_callback );
    APP_ERROR_CHECK( err_code );

    err_code = nrf_drv_saadc_channel_init( 0, &channel_config );
    APP_ERROR_CHECK( err_code );

    /*
     * Resolusion of ADC - 10 bit, with maximum value 3600mV
     * 1023 - 3600
     *    0 -    0
     */
    nrf_drv_saadc_limits_set( 0, MIN_VCC * 1023 / 3600, NRF_DRV_SAADC_LIMITH_DISABLED - 1 );

    return err_code;
}
/*-----------------------------------------------------------*/

/**
 * @brief Timers switch used to monitor the connection and publish. There is only one timer running at a time.
 *
 * @param[in]   timer   The number of the timer, which must be ON.
 */
#ifdef demoUSE_ANTIFREEZE_TIMERS
void timer_switch( uint32_t timer )
{
    if( ( timer == TIMER_CONNECT ) && nrf_drv_timer_is_enabled( &xTimer2 ) )
    {
        nrf_drv_timer_disable( &xTimer2 );
        nrf_drv_timer_clear( &xTimer1 );
        nrf_drv_timer_enable( &xTimer1 );
    }
    else if( ( timer == TIMER_PUBLISH ) && nrf_drv_timer_is_enabled( &xTimer1 ) )
    {
        nrf_drv_timer_disable( &xTimer1 );
        nrf_drv_timer_clear( &xTimer2 );
        nrf_drv_timer_enable( &xTimer2 );
    }
}
#endif
/*-----------------------------------------------------------*/

/**
 * @brief Timer driver event handler type.
 *
 * @param[in] event_type Timer event.
 * @param[in] p_context  General purpose parameter set during initialization of
 *                       the timer. This parameter can be used to pass
 *                       additional information to the handler function, for
 *                       example, the timer ID.
 */
 #ifdef demoUSE_ANTIFREEZE_TIMERS
void timer_event_handler(nrf_timer_event_t event_type, void* p_context)
{
    UNUSED_PARAMETER(p_context);
    
    switch (event_type)
    {
        case NRF_TIMER_EVENT_COMPARE0:
            /*When the timer channel compare 0 interrupt is triggered, the system soft reset is performed.*/
            NVIC_SystemReset();
            break;

        default:
            //Do nothing.
            break;
    }
}
#endif
/*-----------------------------------------------------------*/
#ifdef demoUSE_ANTIFREEZE_TIMERS
static ret_code_t init_timer(nrf_drv_timer_t const * const p_instance, uint32_t time_ms)
{
    uint32_t time_ticks;
    uint32_t err_code = NRF_SUCCESS;

    //Configure TIMER_LED for generating simple light effect - leds on board will invert his state one after the other.
    nrf_drv_timer_config_t timer_cfg = NRF_DRV_TIMER_DEFAULT_CONFIG;
    err_code = nrf_drv_timer_init(p_instance, &timer_cfg, timer_event_handler);
    APP_ERROR_CHECK(err_code);

    time_ticks = nrf_drv_timer_ms_to_ticks(p_instance, time_ms);

    nrf_drv_timer_extended_compare(
         p_instance, NRF_TIMER_CC_CHANNEL0, time_ticks, NRF_TIMER_SHORT_COMPARE0_CLEAR_MASK, true);

    return err_code;
}
#endif
/*-----------------------------------------------------------*/

/**
 * @brief Initializes the board.
 */
static ret_code_t prvPeripheryInit( void )
{
    ret_code_t err_code = NRF_SUCCESS;

    leds_init();

    /* Timer for update leds and feed WatchDog */
    err_code = timers_init();
    APP_ERROR_CHECK(err_code);

    err_code = button_init();
    APP_ERROR_CHECK(err_code);

#ifdef demoUSE_ANTIFREEZE_TIMERS
    err_code = init_timer(&xTimer1, demoTIMEOUT_CONNECT);
    APP_ERROR_CHECK(err_code);
    nrf_drv_timer_enable(&xTimer1);

    err_code = init_timer(&xTimer2, demoTIMEOUT_PUBLISH);
    APP_ERROR_CHECK(err_code);
#endif

    //Configure WDT.
    err_code = wdt_init();
    APP_ERROR_CHECK(err_code);

    err_code = saadc_init();
    APP_ERROR_CHECK(err_code);

    return err_code;
}
///*-----------------------------------------------------------*/
//
///**@brief Main print function.
// *
// * @param[in] pcString   Pointer to the data.
// */
///* configUSE_STATIC_ALLOCATION is set to 1, so the application must provide an
// * implementation of vApplicationGetIdleTaskMemory() to provide the memory that is
// * used by the Idle task. */
//void vApplicationGetIdleTaskMemory( StaticTask_t ** ppxIdleTaskTCBBuffer,
//                                    StackType_t ** ppxIdleTaskStackBuffer,
//                                    uint32_t * pulIdleTaskStackSize )
//{
//    /* If the buffers to be provided to the Idle task are declared inside this
//     * function then they must be declared static - otherwise they will be allocated on
//     * the stack and so not exists after this function exits. */
//    static StaticTask_t xIdleTaskTCB;
//    static StackType_t uxIdleTaskStack[ configMINIMAL_STACK_SIZE ];
//
//    /* Pass out a pointer to the StaticTask_t structure in which the Idle
//     * task's state will be stored. */
//    *ppxIdleTaskTCBBuffer = &xIdleTaskTCB;
//
//    /* Pass out the array that will be used as the Idle task's stack. */
//    *ppxIdleTaskStackBuffer = uxIdleTaskStack;
//
//    /* Pass out the size of the array pointed to by *ppxIdleTaskStackBuffer.
//     * Note that, as the array is necessarily of type StackType_t,
//     * configMINIMAL_STACK_SIZE is specified in words, not bytes. */
//    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
//}
///*-----------------------------------------------------------*/
//
///* configUSE_STATIC_ALLOCATION is set to 1, so the application must provide an
// * implementation of vApplicationGetTimerTaskMemory() to provide the memory that is
// * used by the RTOS daemon/time task. */
//void vApplicationGetTimerTaskMemory( StaticTask_t ** ppxTimerTaskTCBBuffer,
//                                     StackType_t ** ppxTimerTaskStackBuffer,
//                                     uint32_t * pulTimerTaskStackSize )
//{
//    /* If the buffers to be provided to the Timer task are declared inside this
//     * function then they must be declared static - otherwise they will be allocated on
//     * the stack and so not exists after this function exits. */
//    static StaticTask_t xTimerTaskTCB;
//    static StackType_t uxTimerTaskStack[ configTIMER_TASK_STACK_DEPTH ];
//
//    /* Pass out a pointer to the StaticTask_t structure in which the Idle
//     * task's state will be stored. */
//    *ppxTimerTaskTCBBuffer = &xTimerTaskTCB;
//
//    /* Pass out the array that will be used as the Timer task's stack. */
//    *ppxTimerTaskStackBuffer = uxTimerTaskStack;
//
//    /* Pass out the size of the array pointed to by *ppxTimerTaskStackBuffer.
//     * Note that, as the array is necessarily of type StackType_t,
//     * configMINIMAL_STACK_SIZE is specified in words, not bytes. */
//    *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
//}
///*-----------------------------------------------------------*/
//
///**
// * @brief ResetsWhen the timer channel 0 interrupt is triggered, the system soft reset is performed. the device if pvPortMalloc fails.
// *
// * Called if a call to pvPortMalloc() fails because there is insufficient
// * free memory available in the FreeRTOS heap.  pvPortMalloc() is called
// * internally by FreeRTOS API functions that create tasks, queues, software
// * timers, and semaphores.  The size of the FreeRTOS heap is set by the
// * configTOTAL_HEAP_SIZE configuration constant in FreeRTOSConfig.h.
// *
// */
//void vApplicationMallocFailedHook()
//{
//    /* vApplicationMallocFailedHook() will only be called if
//    configUSE_MALLOC_FAILED_HOOK is set to 1 in FreeRTOSConfig.h.  It is a hook
//    function that will get called if a call to pvPortMalloc() fails.
//    pvPortMalloc() is called internally by the kernel whenever a task, queue,
//    timer or semaphore is created.  It is also called by various parts of the
//    demo application.  If heap_1.c or heap_2.c are used, then the size of the
//    heap available to pvPortMalloc() is defined by configTOTAL_HEAP_SIZE in
//    FreeRTOSConfig.h, and the xPortGetFreeHeapSize() API function can be used
//    to query the size of free heap space that remains (although it does not
//    provide information on how the remaining heap might be fragmented). */
//
//    configPRINTF( ( "ERROR: Malloc failed to allocate memory\r\n" ) );
//    NVIC_SystemReset();
//}

//
///*-----------------------------------------------------------*/
///**
// * @brief Creates an MQTT client and then connects to the MQTT broker.
// *
// * The MQTT broker end point is set by clientcredentialMQTT_BROKER_ENDPOINT.
// *
// * @return pdPASS if everything is successful, pdFAIL otherwise.
// */
//static BaseType_t prvCreateClientAndConnectToBroker( void );
//
///**
// * @brief The callback registered with the MQTT client to get notified when
// * data is received from the broker.
// *
// * @param[in] pvUserData User data as supplied while registering the callback.
// * @param[in] pxCallbackParams Data received from the broker.
// *
// * @return Indicates whether or not we take the ownership of the buffer containing
// * the MQTT message. We never take the ownership and always return eMQTTFalse.
// */
//static BaseType_t prvMQTTCallback( void * pvUserData,
//                                   const MQTTAgentCallbackParams_t * const pxCallbackParams )
//{
//    BaseType_t xTakeOwnership = pdFALSE;
//    MQTTAgentReturnCode_t xReturnCode;
//
//    /* Remove compiler warnings about unused parameters. */
//    ( void ) pvUserData;
//
//    switch( pxCallbackParams->xMQTTEvent )
//    {
//        case eMQTTAgentDisconnect:
//            iLEDSState = eDeviceState_LEDS_MQTT_Disconnected;
//            iConnectionState = 0;
//            configPRINTF( ( "MQTT Client got disconnected.\r\n" ) );
//            xReturnCode = MQTT_AGENT_Delete( xMQTTHandle );
//            xMQTTHandle = NULL;
//            configPRINTF( ( "MQTT Client Deleted.\r\n" ) );
//            ( void ) xTaskCreate( prvMQTTConnectAndPublishTask,		/* The function that implements the demo task. */
//                          "MQTTDemo",                                   /* The name to assign to the task being created. */
//                          democonfigMQTT_ECHO_TASK_STACK_SIZE,		/* The size, in WORDS (not bytes), of the stack to allocate for the task being created. */
//                          NULL,                                         /* The task parameter is not being used. */
//                          democonfigMQTT_ECHO_TASK_PRIORITY,		/* The priority at which the task being created will run. */
//                          NULL );                                       /* Not storing the task's handle. */
//            break;
//
//        case eMQTTAgentPublish:
//            configPRINTF( ( "WARN: Should not have been called as we are registering topic specific callbacks.\r\n" ) );
//            break;
//    }
//
//    /* We do not want to take the ownership of buffers in any case. */
//    return xTakeOwnership;
//}
//
///*-----------------------------------------------------------*/
//
//static BaseType_t prvCreateClientAndConnectToBroker( void )
//{
//    MQTTAgentReturnCode_t xReturned;
//    BaseType_t xReturn = pdFAIL;
//
//    MQTTAgentConnectParams_t xConnectParameters = 
//    {
//        (const char *)clientcredentialMQTT_BROKER_ENDPOINT, /* The URL of the MQTT broker to connect to. */
//        pdTRUE,                                             /* Set to pdTRUE if the provided URL is an IP address, otherwise set to pdFALSE. */
//        clientcredentialMQTT_BROKER_PORT,                   /* Port number on which the MQTT broker is listening. */
//        CLIENT_ID,                                          /* Client Identifier of the MQTT client. It should be unique per broker. */
//        0,                                                  /* The length of the client Id, filled in later as not const. */
//        pdTRUE,                                             /* Set to pdTRUE to use TLS, pdFALSE to not use TLS. */
//        NULL,                                               /* User data supplied to the callback. Can be NULL. */
//        &( prvMQTTCallback ),                               /* Callback used to report various events. Can be NULL. */
//        (char *)GREENGRASS_CERTIFICATE_PEM,                 /* Certificate used for secure connection. Can be NULL. */
//        strlen((char *)GREENGRASS_CERTIFICATE_PEM) + 1      /* Size of certificate used for secure connection. */ /* include '\0' to length*/
//    };
//
//    /* Check this function has not already been executed. */
//    configASSERT( xMQTTHandle == NULL );
//
//    /* The MQTT client object must be created before it can be used.  The
//     * maximum number of MQTT client objects that can exist simultaneously
//     * is set by mqttconfigMAX_BROKERS. */
//    xReturned = MQTT_AGENT_Create( &xMQTTHandle );
//
//    if( xReturned == eMQTTAgentSuccess )
//    {
//        configPRINTF( ( "MQTT_AGENT_Create OK\r\n" ) );
//        /* Fill in the MQTTAgentConnectParams_t member that is not const,
//         * and therefore could not be set in the initializer (where
//         * xConnectParameters is declared in this function). */
//        xConnectParameters.usClientIdLength = ( uint16_t ) strlen( ( const char * )CLIENT_ID );
//
//        /* Connect to the broker. */
//        configPRINTF( ( "MQTT demo attempting to connect to %s.\r\n", clientcredentialMQTT_BROKER_ENDPOINT ) );
//        xReturned = MQTT_AGENT_Connect( xMQTTHandle,
//                                        &xConnectParameters,
//                                        democonfigMQTT_ECHO_TLS_NEGOTIATION_TIMEOUT );
//
//        if( xReturned != eMQTTAgentSuccess )
//        {
//            /* Could not connect, so delete the MQTT client. */
//            ( void ) MQTT_AGENT_Delete( xMQTTHandle );
//            configPRINTF( ( "ERROR:  MQTT demo failed to connect.\r\n" ) );
//        }
//        else
//        {
//            configPRINTF( ( "MQTT demo connected.\r\n" ) );
//            xReturn = pdPASS;
//        }
//    }
//
//    return xReturn;
//}
///*-----------------------------------------------------------*/
//
//BaseType_t prvPublishMessage( BaseType_t ButtonNum )
//{
//    MQTTAgentPublishParams_t xPublishParameters;
//    MQTTAgentReturnCode_t xReturned;
//    char cDataBuffer[ MAX_DATA_LENGTH ];
//
//    /* Check this function is not being called before the MQTT client object has
//     * been created. */
//    configASSERT( xMQTTHandle != NULL );
//
//    /* Create the message that will be published
//     * 0 - health message
//     * 1 .. 4 - number of button pressed on board */
//    ( void ) snprintf( cDataBuffer, MAX_DATA_LENGTH, "%d", ( int ) ButtonNum );
//
//    /* Setup the publish parameters. */
//    memset( &( xPublishParameters ), 0x00, sizeof( xPublishParameters ) );
//    xPublishParameters.pucTopic = TOPIC_NAME;
//    xPublishParameters.pvData = cDataBuffer;
//    xPublishParameters.usTopicLength = ( uint16_t ) strlen( ( const char * ) TOPIC_NAME );
//    xPublishParameters.ulDataLength = ( uint32_t ) strlen( cDataBuffer );
//    xPublishParameters.xQoS = eMQTTQoS0;
//
//    if(ButtonNum == 0)
//    {
//        xPublishParameters.pucTopic = TOPIC_NAME_HEALTH;
//        xPublishParameters.usTopicLength = ( uint16_t ) strlen( ( const char * ) TOPIC_NAME_HEALTH );
//    }
//
//    /* Publish the message. */
//    xReturned = MQTT_AGENT_Publish( xMQTTHandle,
//                                    &( xPublishParameters ),
//                                    democonfigMQTT_TIMEOUT );
//
//    if( xReturned == eMQTTAgentSuccess )
//    {
//        configPRINTF( ( "Message successfully published '%s'\r\n", cDataBuffer ) );
//    }
//    else
//    {
//        configPRINTF( ( "ERROR:  Message failed to publish '%s'\r\n", cDataBuffer ) );
//    }
//
//    return xReturned;
//}
///*-----------------------------------------------------------*/
//
///**@brief Publish task entry function.
// *
// * @param[in] pvParameter   Pointer that will be used as the parameter for the task.
// */
//static void prvPublishTask (void * pvParameter)
//{
//    UNUSED_PARAMETER(pvParameter);
//
//    /* Create the semaphore that is used to demonstrate a task being
//    synchronised with an interrupt. */
//    vSemaphoreCreateBinary( xPublicSemaphore );
//
//    while (true)
//    {    
//        xSemaphoreTake( xPublicSemaphore, portMAX_DELAY );
//
//        if(iButtonPressed)
//        {
//            if(iConnectionState)
//            {
//                if( xQueueSend( xQueueMsg, &iButtonPressed, 0 ) != pdPASS )
//                {
//                    configPRINTF( ( "ERROR prvPublishTask\r\n" ) );
//                    iConnectionState = 0;
//                }
//            }
//
//            iButtonPressed = 0;
//        }
//    }
//}
///*-----------------------------------------------------------*/
//
///**@brief Publish health task entry function.
// *
// * @param[in] pvParameter   Pointer that will be used as the parameter for the task.
// */
//static void prvHealthPublishTask (void * pvParameter)
//{
//    UNUSED_PARAMETER(pvParameter);
//
//    BaseType_t but = 0;
//
//    /* Create the semaphore that is used to demonstrate a task being
//    synchronised with an interrupt. */
//    while (true)
//    {    
//        if(iConnectionState)
//        {
//            if( xQueueSend( xQueueMsg, &but, 0 ) != pdPASS )
//            {
//                configPRINTF( ( "ERROR prvHealthPublishTask\r\n" ) );
//                iConnectionState = 0;
//            }
//        }
//        vTaskDelay(2000);
//    }
//}
///*-----------------------------------------------------------*/
//
///**@brief Queue task entry function.
// *
// * @param[in] pvParameter   Pointer that will be used as the parameter for the task.
// */
//static void prvSendToQueueTask (void * pvParameter)
//{
//    UNUSED_PARAMETER(pvParameter);
//
//    BaseType_t but;
//
//    for( ;; )
//    {
//        /* Block to wait for the next string to print. */
//        if( xQueueReceive( xQueueMsg, &but, portMAX_DELAY ) == pdPASS )
//        {
//            if(prvPublishMessage(but) != 0)
//            {
//                configPRINTF( ( "ERROR Publish MSG\r\n" ) );
//            }
//            /* xTimer2 make reset for device if messages is not sended during 5s */
//            nrf_drv_timer_clear(&xTimer2);
//        }
//        else
//        {
//            configPRINTF( ( "ERROR xQueueMsg\r\n" ) );
//        }
//    }
//}
///*-----------------------------------------------------------*/
//
//static void prvMQTTConnectAndPublishTask( void * pvParameters )
//{
//    BaseType_t x, xReturned;
//
//    /* Avoid compiler warnings about unused parameters. */
//    ( void ) pvParameters;
//
//    iLEDSState = eDeviceState_LEDS_MQTT_ConnectableMode;
//
//    /* Create the MQTT client object and connect it to the MQTT broker. */
//    xReturned = prvCreateClientAndConnectToBroker();
//
//    if( xReturned == pdPASS )
//    {
//        iLEDSState = eDeviceState_LEDS_MQTT_Connected;
//
//        configPRINTF( ( "MQTT demo test task created.\r\n" ) );
//
//        xQueueMsg = xQueueCreate( QUEUE_MSH_LENGTH, sizeof( BaseType_t ) );
//
//        if( xQueueMsg == NULL )
//        {
//            configPRINTF( ( "ERROR: xQueueMsg\r\n" ) );
//        }
//
//        /* Create task for sending messages to xQueueMsg */
//        UNUSED_VARIABLE(xTaskCreate(prvPublishTask, "Publish", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY+1, NULL));
//
//        /* Create task for sending health messages to xQueueMsg */
//        UNUSED_VARIABLE(xTaskCreate(prvHealthPublishTask, "Health", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY, NULL));
//
//        /* Create task for Publishing packets */
//        UNUSED_VARIABLE(xTaskCreate(prvSendToQueueTask, "Queue", configMINIMAL_STACK_SIZE * 3, NULL, tskIDLE_PRIORITY+2, NULL));
//    }
//    else
//    {
//        iLEDSState = eDeviceState_LEDS_MQTT_Disconnected;
//
//        configPRINTF( ( "ERROR: MQTT demo test task is not created.\r\n" ) );
//    }
//
//    vTaskDelete( NULL ); /* Delete this task. */
//}
///*-----------------------------------------------------------*/
//
//void vStartMQTTDemo( void )
//{
//    configPRINTF( ( "Creating MQTT Demo Task...\r\n" ) );
//
//    /* Create the task that publishes messages to the MQTT broker every five
//     * seconds.  This task, in turn, creates the task that echoes data received
//     * from the broker back to the broker. */
//    ( void ) xTaskCreate( prvMQTTConnectAndPublishTask,        /* The function that implements the demo task. */
//                          "MQTTDemo",                          /* The name to assign to the task being created. */
//                          democonfigMQTT_ECHO_TASK_STACK_SIZE, /* The size, in WORDS (not bytes), of the stack to allocate for the task being created. */
//                          NULL,                                /* The task parameter is not being used. */
//                          democonfigMQTT_ECHO_TASK_PRIORITY,   /* The priority at which the task being created will run. */
//                          NULL );                              /* Not storing the task's handle. */
//}
///*-----------------------------------------------------------*/
