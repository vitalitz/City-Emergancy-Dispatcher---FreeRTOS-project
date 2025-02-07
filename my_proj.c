/* Standard includes. */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <signal.h>

/* Kernel includes. */
#include "FreeRTOS.h"

#ifndef PTHREAD_STACK_MIN
    #define PTHREAD_STACK_MIN 16384 // Define a reasonable default value
#endif

#include <task.h>
#include <queue.h>
#include <timers.h>
#include <semphr.h>

#define REQUEST_QUEUE_ITEM_SIZE sizeof(Message_t)

#define CALL_OPERATORS_COUNT 10
#define POLICE_UNIT_COUNT 3
#define AMBULANCE_UNIT_COUNT 4
#define FIRE_UNIT_COUNT 2

enum LogMessageType {
    LOG_INFO = 0,
    LOG_WARNING = 1,
    LOG_ERROR = 2
};

typedef struct {
    enum LogMessageType messageType;
    char taskName[32];
    char message[256];
} LogMessage_t;

enum PriorityLevel {
    LOW = 0,
    MEDIUM = 1,
    HIGH = 2
};

enum EmergencyType {
    POLICE = 0,
    AMBULANCE = 1,
    FIRE = 2
};

typedef struct {
    enum EmergencyType emergencyType;
    enum PriorityLevel priorityLevel;
    TickType_t timeLimit;
    QueueHandle_t responseQueue;  // Response queue for this task
    TaskHandle_t taskHandle;      // Handle of the sender task
} Message_t;

typedef struct {
    char unitName[32];
    int unitsAvailable;
    QueueHandle_t requestQueue;
    QueueHandle_t unitQueue;
    SemaphoreHandle_t xUnitsCountingSemaphore;
    char *auxUnitName[2];
    QueueHandle_t auxUnitQueue[2];
    SemaphoreHandle_t xAuxUnitsCountingSemaphore[2];
} DepartmentParams_t;

typedef struct {
    SemaphoreHandle_t xUnitsCountingSemaphore;
    QueueHandle_t eventQueue;
} UnitParams_t;

typedef struct {
    Message_t message;
    SemaphoreHandle_t xUnitsCountingSemaphore;
    QueueHandle_t departmentQueue;
    QueueHandle_t unitQueue;
    char *auxUnitName[2];
    QueueHandle_t auxUnitQueue[2];
    SemaphoreHandle_t xAuxUnitsCountingSemaphore[2];
} EventDaemonParams_t;

QueueHandle_t logQueue = NULL;
QueueHandle_t emergencyCallQueue = NULL;
QueueHandle_t policeDepartmentQueue = NULL;
QueueHandle_t ambulanceDepartmentQueue = NULL;
QueueHandle_t fireDepartmentQueue = NULL;
QueueHandle_t policeUnitsQueue = NULL;
QueueHandle_t ambulanceUnitsQueue = NULL;
QueueHandle_t fireUnitsQueue = NULL;

SemaphoreHandle_t xPoliceUnitsCountingSemaphore = NULL;
SemaphoreHandle_t xAmbulanceUnitsCountingSemaphore = NULL;
SemaphoreHandle_t xFireUnitsCountingSemaphore = NULL;

FILE *logFile;

void logMsgSend(QueueHandle_t logQueue, enum LogMessageType type, char* taskName, char* message);

char* LogMessageType2string(enum LogMessageType type);
char* EmergencyType2string(enum EmergencyType type);
char* PriorityLevel2string(enum PriorityLevel level);

/* Task function prototypes */
void vLoggingTask(void *pvParameters);
void vEmergencyEventTask(void *pvParameters);
void vEmergencyDespatcherTask(void *pvParameters);
void vFaultManagementTask(void *pvParameters);

void vDepartmentTask(void *pvParameters);
void vUnitTask(void *pvParameters);
void vEventDaemonTask(void *pvParameters);

/* Timer callback prototype */
void vRandomEventTimerCallback(TimerHandle_t xRandomEventTimer);

void handle_sigint(int sig)
{
    printf("Caught signal %d\n", sig);
    if(logFile != NULL) {
        fflush(logFile);
        fclose(logFile);
    }
    exit(0);
}

int my_proj(void)
{
    signal(SIGINT, handle_sigint);

    logQueue = xQueueCreate(10, sizeof(LogMessage_t));
    if (logQueue == NULL) {
        printf("Failed to create log queue.\n");
    }

    /* Create a task to handle logging */
    xTaskCreate(vLoggingTask, "LoggingTask", configMINIMAL_STACK_SIZE, (void*)logQueue, tskIDLE_PRIORITY + 1, NULL);

    /* Create a software timer */
    TimerHandle_t xRandomEventTimer = xTimerCreate(
        "Timer1",                   // Timer name (for debugging)
        pdMS_TO_TICKS(2000),        // Timer period (2 seconds)
        pdTRUE,                     // Auto-reload (runs repeatedly)
        (void *)0,                  // Timer ID (optional)
        vRandomEventTimerCallback              // Callback function
    );

    if (xRandomEventTimer == NULL) {
        printf("Failed to create timer.\n");
        return 1;
    }

    /* Start the timer */
    if (xTimerStart(xRandomEventTimer, 0) != pdPASS) {
        printf("Failed to start timer.\n");
        return 1;
    }
    
    /* Create a task to handle emergency events */
    xTaskCreate(vEmergencyDespatcherTask, "EmergencyDespatcherTask", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 5, NULL);

    /* Start the scheduler */
    vTaskStartScheduler();
    return 1;
}

void logMsgSend(QueueHandle_t logQueue, enum LogMessageType type, char* taskName, char* message) {
    LogMessage_t logMessage;
    logMessage.messageType = type;
    snprintf(logMessage.taskName, sizeof(logMessage.taskName), "%s", taskName);
    snprintf(logMessage.message, sizeof(logMessage.message), "%s", message);
    if (xQueueSend(logQueue, &logMessage, portMAX_DELAY) != pdPASS) {
        printf("Failed to send log message.\n");
    }
}

void vLoggingTask(void *pvParameters) {
    LogMessage_t logMessage;
    time_t rawtime;
    struct tm *timeinfo;
    char logLine[320];

    logFile = fopen("log.txt", "w");
    if (logFile == NULL) {
        printf("Failed to open log file.\n");
        vTaskDelete(NULL);
    }

    printf("Logging task is running.\n");
    
    for (;;) {
        if (xQueueReceive(logQueue, &logMessage, portMAX_DELAY) == pdPASS) {
            time(&rawtime);
            timeinfo = localtime(&rawtime);
            snprintf(logLine, sizeof(logLine), "[Time: %02d:%02d:%02d] [%s] [%s] %s\n",
                timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec, LogMessageType2string(logMessage.messageType), logMessage.taskName, logMessage.message);
            fprintf(logFile, "%s", logLine);
            printf("%s", logLine);
        }
        else {
            printf("Failed to receive log message.\n");
        }
    }
}

/* vEmergencyEventTask function */
void vEmergencyEventTask(void *pvParameters) {
    QueueHandle_t responseQueue;
    Message_t emergencyCall;
    char logMsg[256];

    // Create a private response queue for this task
    responseQueue = xQueueCreate(1, sizeof(Message_t));
    if (responseQueue == NULL) {
        printf("%s: Failed to create response queue.\n", pcTaskGetName(NULL));
        vTaskDelete(NULL);
    }

    // Assign the same queue to emergencyCall.responseQueue
    emergencyCall.responseQueue = responseQueue;
    emergencyCall.taskHandle = xTaskGetCurrentTaskHandle();
    emergencyCall.timeLimit = xTaskGetTickCount() + pdMS_TO_TICKS((10 + rand() % 60) * 1000); // Random number between 10 and 50 seconds

    emergencyCall.emergencyType = rand() % 3; // Random number between 0 and 2
    emergencyCall.priorityLevel = rand() % 3; // Random number between 0 and 2

    sprintf(logMsg, "Event: %s, Priority: %s, Time limit: %ld seconds",
        EmergencyType2string(emergencyCall.emergencyType), PriorityLevel2string(emergencyCall.priorityLevel), (emergencyCall.timeLimit - xTaskGetTickCount())/1000);

    logMsgSend(logQueue, LOG_INFO, pcTaskGetName(NULL), logMsg);

    if (xQueueSend(emergencyCallQueue, &emergencyCall, emergencyCall.timeLimit - xTaskGetTickCount()) == pdPASS) {
        // Wait for the response
        if (xQueueReceive(responseQueue, &emergencyCall, emergencyCall.timeLimit - xTaskGetTickCount()) == pdPASS) {
            logMsgSend(logQueue, LOG_INFO, pcTaskGetName(NULL), "The event was successfully completed");
        } else {
            logMsgSend(logQueue, LOG_ERROR, pcTaskGetName(NULL), "The event wasn't completed in time");
        }
    } else {
        logMsgSend(logQueue, LOG_ERROR, pcTaskGetName(NULL), "The call wasn't answered");
    }
    emergencyCall.responseQueue = NULL;
    // Clean up and delete the task
    vQueueDelete(responseQueue);
    vTaskDelete(NULL);
}

/* vEmergencyDespatcherTask function */
void vEmergencyDespatcherTask(void *pvParameters) {
    Message_t message;
    char logMsg[256];
    DepartmentParams_t *policeParams = (DepartmentParams_t *)pvPortMalloc(sizeof(DepartmentParams_t));
    DepartmentParams_t *ambulanceParams = (DepartmentParams_t *)pvPortMalloc(sizeof(DepartmentParams_t));
    DepartmentParams_t *fireParams = (DepartmentParams_t *)pvPortMalloc(sizeof(DepartmentParams_t));
    policeDepartmentQueue = xQueueCreate(1, REQUEST_QUEUE_ITEM_SIZE);
    ambulanceDepartmentQueue = xQueueCreate(1, REQUEST_QUEUE_ITEM_SIZE);
    fireDepartmentQueue = xQueueCreate(1, REQUEST_QUEUE_ITEM_SIZE);
    policeUnitsQueue = xQueueCreate(POLICE_UNIT_COUNT, REQUEST_QUEUE_ITEM_SIZE);
    ambulanceUnitsQueue = xQueueCreate(AMBULANCE_UNIT_COUNT, REQUEST_QUEUE_ITEM_SIZE);
    fireUnitsQueue = xQueueCreate(FIRE_UNIT_COUNT, REQUEST_QUEUE_ITEM_SIZE);
    xPoliceUnitsCountingSemaphore = xSemaphoreCreateCounting(POLICE_UNIT_COUNT, POLICE_UNIT_COUNT);
    xAmbulanceUnitsCountingSemaphore = xSemaphoreCreateCounting(AMBULANCE_UNIT_COUNT, AMBULANCE_UNIT_COUNT);
    xFireUnitsCountingSemaphore = xSemaphoreCreateCounting(FIRE_UNIT_COUNT, FIRE_UNIT_COUNT);
    emergencyCallQueue = xQueueCreate(1, REQUEST_QUEUE_ITEM_SIZE);
    logMsgSend(logQueue, LOG_INFO, pcTaskGetName(NULL), "Dispatcher task is running");
    sprintf(policeParams->unitName, "Police");
    sprintf(ambulanceParams->unitName, "Ambulance");
    sprintf(fireParams->unitName, "Fire");
    policeParams->unitsAvailable = POLICE_UNIT_COUNT;
    ambulanceParams->unitsAvailable = AMBULANCE_UNIT_COUNT;
    fireParams->unitsAvailable = FIRE_UNIT_COUNT;
    policeParams->requestQueue = policeDepartmentQueue;
    ambulanceParams->requestQueue = ambulanceDepartmentQueue;
    fireParams->requestQueue = fireDepartmentQueue;
    policeParams->unitQueue = policeUnitsQueue;
    ambulanceParams->unitQueue = ambulanceUnitsQueue;
    fireParams->unitQueue = fireUnitsQueue;
    policeParams->xUnitsCountingSemaphore = xPoliceUnitsCountingSemaphore;
    ambulanceParams->xUnitsCountingSemaphore = xAmbulanceUnitsCountingSemaphore;
    fireParams->xUnitsCountingSemaphore = xFireUnitsCountingSemaphore;
    policeParams->auxUnitName[0] = ambulanceParams->unitName;
    policeParams->auxUnitQueue[0] = ambulanceUnitsQueue;
    policeParams->xAuxUnitsCountingSemaphore[0] = xAmbulanceUnitsCountingSemaphore;
    policeParams->auxUnitName[1] = fireParams->unitName;
    policeParams->auxUnitQueue[1] = fireUnitsQueue;
    policeParams->xAuxUnitsCountingSemaphore[1] = xFireUnitsCountingSemaphore;
    ambulanceParams->auxUnitName[0] = policeParams->unitName;
    ambulanceParams->auxUnitQueue[0] = policeUnitsQueue;
    ambulanceParams->xAuxUnitsCountingSemaphore[0] = xPoliceUnitsCountingSemaphore;
    ambulanceParams->auxUnitName[1] = fireParams->unitName;
    ambulanceParams->auxUnitQueue[1] = fireUnitsQueue;
    ambulanceParams->xAuxUnitsCountingSemaphore[1] = xFireUnitsCountingSemaphore;
    fireParams->auxUnitName[0] = policeParams->unitName;
    fireParams->auxUnitQueue[0] = policeUnitsQueue;
    fireParams->xAuxUnitsCountingSemaphore[0] = xPoliceUnitsCountingSemaphore;
    fireParams->auxUnitName[1] = ambulanceParams->unitName;
    fireParams->auxUnitQueue[1] = ambulanceUnitsQueue;
    fireParams->xAuxUnitsCountingSemaphore[1] = xAmbulanceUnitsCountingSemaphore;

    xTaskCreate(vDepartmentTask, "PoliceDepartmentTask", configMINIMAL_STACK_SIZE, (void*)policeParams, tskIDLE_PRIORITY + 2, NULL);
    xTaskCreate(vDepartmentTask, "AmbulanceDepartmentTask", configMINIMAL_STACK_SIZE, (void*)ambulanceParams, tskIDLE_PRIORITY + 2, NULL);
    xTaskCreate(vDepartmentTask, "FireDepartmentTask", configMINIMAL_STACK_SIZE, (void*)fireParams, tskIDLE_PRIORITY + 2, NULL);
    
    for (;;) {
        if (xQueueReceive(emergencyCallQueue, &message, portMAX_DELAY) == pdPASS) {
            switch (message.emergencyType) {
            case POLICE:
                if (xQueueSend(policeParams->requestQueue, &message, 0) != pdPASS) {
                    logMsgSend(logQueue, LOG_ERROR, pcTaskGetName(NULL), "Failed to send the message to the police department");
                }
                break;
            case AMBULANCE:
                if (xQueueSend(ambulanceParams->requestQueue, &message, 0) != pdPASS) {
                    logMsgSend(logQueue, LOG_ERROR, pcTaskGetName(NULL), "Failed to send the message to the ambulance department");
                }
                break;
            case FIRE:
                if (xQueueSend(fireParams->requestQueue, &message, 0) != pdPASS) {
                    logMsgSend(logQueue, LOG_ERROR, pcTaskGetName(NULL), "Failed to send the message to the fire department");
                }
                break;
            }
        }
    }
    vPortFree(policeParams);
    vPortFree(ambulanceParams);
    vPortFree(fireParams);
}

void vDepartmentTask(void *pvParameters)
{
    DepartmentParams_t *params = (DepartmentParams_t *)pvParameters;
    UnitParams_t *unitParams = (UnitParams_t *)pvPortMalloc(sizeof(UnitParams_t));
    Message_t message;
    char logMsg[256];

    sprintf(logMsg, "%s department task is running", params->unitName);
    logMsgSend(logQueue, LOG_INFO, pcTaskGetName(NULL), logMsg);

    unitParams->eventQueue = params->unitQueue;
    unitParams->xUnitsCountingSemaphore = params->xUnitsCountingSemaphore;

    for (int i = 0; i < params->unitsAvailable; i++) {
        char taskName[32], tmp_str[64];
        snprintf(tmp_str, sizeof(tmp_str), "%sUnitTask%d", params->unitName, i);
        snprintf(taskName, sizeof(taskName), "%.31s", tmp_str);
        xTaskCreate(vUnitTask, taskName, configMINIMAL_STACK_SIZE, (void*)unitParams, tskIDLE_PRIORITY + 1 + i, NULL);
    }
    for (;;) {
        if(xQueueReceive(params->requestQueue, &message, portMAX_DELAY) == pdPASS) {
            EventDaemonParams_t *eventDaemonParams = (EventDaemonParams_t *)pvPortMalloc(sizeof(EventDaemonParams_t));
            eventDaemonParams->message = message;
            eventDaemonParams->xUnitsCountingSemaphore = params->xUnitsCountingSemaphore;
            eventDaemonParams->departmentQueue = params->requestQueue;
            eventDaemonParams->unitQueue = params->unitQueue;
            eventDaemonParams->auxUnitName[0] = params->auxUnitName[0];
            eventDaemonParams->auxUnitQueue[0] = params->auxUnitQueue[0];
            eventDaemonParams->xAuxUnitsCountingSemaphore[0] = params->xAuxUnitsCountingSemaphore[0];
            eventDaemonParams->auxUnitName[1] = params->auxUnitName[1];
            eventDaemonParams->auxUnitQueue[1] = params->auxUnitQueue[1];
            eventDaemonParams->xAuxUnitsCountingSemaphore[1] = params->xAuxUnitsCountingSemaphore[1];

            xTaskCreate(vEventDaemonTask, "EventDaemonTask", configMINIMAL_STACK_SIZE, (void*)eventDaemonParams, tskIDLE_PRIORITY + 2, NULL);
        }
    }
    vPortFree(unitParams); 
}

/* vUnitTask function */
void vUnitTask(void *pvParameters)
{
    UnitParams_t *params = (UnitParams_t *)pvParameters;
    Message_t message;
    char logMsg[256];

    logMsgSend(logQueue, LOG_INFO, pcTaskGetName(NULL), "Unit task is running");

    for (;;) {
        if (xQueueReceive(params->eventQueue, &message, portMAX_DELAY) == pdPASS) {
            sprintf(logMsg, "Got message of: %s",
                pcTaskGetName(message.taskHandle));
            logMsgSend(logQueue, LOG_INFO, pcTaskGetName(NULL), logMsg);
            sprintf(logMsg, "Attending the event of emergency call: %s, Priority: %s, Time limit: %ld seconds",
                pcTaskGetName(message.taskHandle), PriorityLevel2string(message.priorityLevel), (message.timeLimit - xTaskGetTickCount())/1000);
            logMsgSend(logQueue, LOG_INFO, pcTaskGetName(NULL), logMsg);

            int responseTime = 5000 + rand() % 50000;
            if(message.timeLimit < xTaskGetTickCount() + pdMS_TO_TICKS(responseTime)) {
                logMsgSend(logQueue, LOG_WARNING, pcTaskGetName(NULL), "Dropping - the event won't be completed in time");
            } else {
                vTaskDelay(pdMS_TO_TICKS(responseTime));
                if(message.responseQueue == NULL) {
                    logMsgSend(logQueue, LOG_ERROR, pcTaskGetName(NULL), "Response queue is NULL");
                }
                else if(xQueueSend(message.responseQueue, &message, 0) != pdPASS) {
                    logMsgSend(logQueue, LOG_ERROR, pcTaskGetName(NULL), "Failed to send response");
                }
                else
                {
                    logMsgSend(logQueue, LOG_INFO, pcTaskGetName(NULL), "Completed the event");
                }
            }
        }
        logMsgSend(logQueue, LOG_INFO, pcTaskGetName(NULL), "Unit is available");
        xSemaphoreGive(params->xUnitsCountingSemaphore);
    }
}

void vEventDaemonTask(void *pvParameters)
{
    EventDaemonParams_t *params = (EventDaemonParams_t *)pvParameters;
    
    char logMsg[256];

    if(xSemaphoreTake(params->xUnitsCountingSemaphore, 0) == pdPASS)
    {
        if(xQueueSend(params->unitQueue, &params->message, 0) != pdPASS) {
            logMsgSend(logQueue, LOG_ERROR, pcTaskGetName(NULL), "Failed to send the message to the original unit");
            xSemaphoreGive(params->xUnitsCountingSemaphore);
        }
    }
    else if(xSemaphoreTake(params->xAuxUnitsCountingSemaphore[0], 0) == pdPASS)
    {
        if(xQueueSend(params->auxUnitQueue[0], &params->message, 0) != pdPASS) {
            sprintf(logMsg, "Failed to send the message to the spare unit %s", params->auxUnitName[0]);
            logMsgSend(logQueue, LOG_ERROR, pcTaskGetName(NULL), logMsg);
            xSemaphoreGive(params->xAuxUnitsCountingSemaphore[0]);
        }
        else
        {
            sprintf(logMsg, "Sent the message to the spare unit %s", params->auxUnitName[0]);
            logMsgSend(logQueue, LOG_INFO, pcTaskGetName(NULL), logMsg);
        }
    }
    else if(xSemaphoreTake(params->xAuxUnitsCountingSemaphore[1], 0) == pdPASS)
    {
        if(xQueueSend(params->auxUnitQueue[1], &params->message, 0) != pdPASS) {
            sprintf(logMsg, "Failed to send the message to the spare unit %s", params->auxUnitName[1]);
            logMsgSend(logQueue, LOG_ERROR, pcTaskGetName(NULL), logMsg);
            xSemaphoreGive(params->xAuxUnitsCountingSemaphore[1]);
        }
        else
        {
            sprintf(logMsg, "Sent the message to the spare unit %s", params->auxUnitName[1]);
            logMsgSend(logQueue, LOG_INFO, pcTaskGetName(NULL), logMsg);
        }
    }
    else if(params->message.timeLimit > xTaskGetTickCount()
        && xSemaphoreTake(params->xUnitsCountingSemaphore, params->message.timeLimit - xTaskGetTickCount()) == pdPASS) {
        if(xQueueSend(params->unitQueue, &params->message, 0) != pdPASS) {
            logMsgSend(logQueue, LOG_ERROR, pcTaskGetName(NULL), "Failed to send the message to the original unit");
            xSemaphoreGive(params->xUnitsCountingSemaphore);
        }
    } else {
        sprintf(logMsg, "No available units for %s event", EmergencyType2string(params->message.emergencyType));
        logMsgSend(logQueue, LOG_WARNING, pcTaskGetName(NULL), logMsg);
    }
    vPortFree(params);
    vTaskDelete(NULL);
}

/* Timer callback function */
void vRandomEventTimerCallback(TimerHandle_t xRandomEventTimer) {
    (void)xRandomEventTimer;

    static uint32_t emergencyEventNumber = 0;
    char emergencyEventTaskName[32];

    /* Perform the timer's task */
    sprintf(emergencyEventTaskName, "EmergencyEventTask%d", emergencyEventNumber++);
    xTaskCreate(vEmergencyEventTask, emergencyEventTaskName, configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 4, NULL);

    /* Update the timer period */
    if (xTimerChangePeriod(xRandomEventTimer, pdMS_TO_TICKS(1000 + rand() % 3000), 0) != pdPASS) {
        printf("Failed to update timer period.\n");
    }

    /* Restart the timer */
    if (xTimerStart(xRandomEventTimer, 0) != pdPASS) {
        printf("Failed to restart timer.\n");
    }
}

char* LogMessageType2string(enum LogMessageType type) {
    switch (type) {
    case LOG_INFO:
        return "Info";
    case LOG_WARNING:
        return "Warning";
    case LOG_ERROR:
        return "Error";
    default:
        return "Unknown";
    }
}

char* EmergencyType2string(enum EmergencyType type) {
    switch (type) {
    case POLICE:
        return "Police";
    case AMBULANCE:
        return "Ambulance";
    case FIRE:
        return "Fire";
    default:
        return "Unknown";
    }
}

char* PriorityLevel2string(enum PriorityLevel level) {
    switch (level) {
    case LOW:
        return "Low";
    case MEDIUM:
        return "Medium";
    case HIGH:
        return "High";
    default:
        return "Unknown";
    }
}