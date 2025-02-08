/*
 * City Emergency Dispatch Simulation - FreeRTOS Project
 *
 * This project simulates an emergency dispatch system using FreeRTOS tasks, queues, and semaphores.
 * The system generates emergency calls randomly and categorizes them into three types:
 *   - Police emergencies
 *   - Ambulance emergencies
 *   - Fire emergencies
 *
 * The dispatcher assigns each call to the appropriate department, ensuring optimal resource allocation.
 * Each department manages a limited number of response units and handles calls based on priority levels:
 *   - LOW priority
 *   - MEDIUM priority
 *   - HIGH priority
 *
 * If a department has no available units, the dispatcher attempts to assign the event to auxiliary units.
 * If no units are available within the time limit, the event is escalated to the fault management system.
 *
 * Key Components:
 * - **vEmergencyEventTask**: Generates emergency calls and submits them to the dispatcher.
 * - **vEmergencyDespatcherTask**: Routes calls to the correct department and manages overflow handling.
 * - **vDepartmentTask**: Handles emergency requests and assigns them to available units.
 * - **vUnitTask**: Processes emergency events and reports completion or failure.
 * - **vFaultManagementTask**: Handles uncompleted emergencies and escalates them.
 * - **vLoggingTask**: Records all actions and errors for debugging and monitoring.
 * - **vEventDaemonTask**: Ensures events are handled optimally by checking availability of units.
 * - **vRandomEventTimerCallback**: Generates emergency events at randomized intervals.
 *
 * This project demonstrates real-time scheduling, resource management, and inter-task communication
 * in a time-sensitive emergency dispatch scenario.
 */

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

/* 
 * Structure representing an emergency event/message.
 * This struct is used to store information about an emergency call,
 * including its type, priority, time limit, and communication details.
 */
typedef struct {
    enum EmergencyType emergencyType;  // Type of emergency (Police, Ambulance, Fire)
    enum PriorityLevel priorityLevel;  // Priority of the emergency (Low, Medium, High)
    TickType_t timeLimit;              // Deadline for handling the emergency (in system ticks)
    QueueHandle_t responseQueue;       // Queue to receive response/status updates for this task
    TaskHandle_t taskHandle;           // Task handle of the sender (used for communication)
} Message_t;

/* 
 * Structure representing an emergency response department.
 * Each department (Police, Ambulance, Fire) has a limited number of available units
 * and manages incoming emergency requests using queues and semaphores.
 */
typedef struct {
    char unitName[32];                 // Name of the department (e.g., "Police", "Ambulance", "Fire")
    int unitsAvailable;                 // Number of available units in this department
    QueueHandle_t requestQueue;         // Queue for receiving emergency requests
    QueueHandle_t unitQueue;            // Queue to assign tasks to available units
    SemaphoreHandle_t xUnitsCountingSemaphore; // Semaphore for tracking available units

    char *auxUnitName[2];               // Names of auxiliary (backup) departments for overflow cases
    QueueHandle_t auxUnitQueue[2];      // Queues for auxiliary units (used when primary units are unavailable)
    SemaphoreHandle_t xAuxUnitsCountingSemaphore[2]; // Semaphores for auxiliary unit availability
} DepartmentParams_t;

/* 
 * Structure representing an individual emergency response unit.
 * Each unit operates within a department (Police, Ambulance, Fire) 
 * and processes emergency events while ensuring proper resource management.
 */
typedef struct {
    SemaphoreHandle_t xUnitsCountingSemaphore; // Semaphore to manage the availability of units
    QueueHandle_t eventQueue;                  // Queue for receiving assigned emergency events
    QueueHandle_t faultManagementQueue;        // Queue for reporting unhandled or failed events
} UnitParams_t;

/* 
 * Structure representing the parameters for an event daemon.
 * The event daemon is responsible for managing emergency events,
 * ensuring that they are assigned to available units, and handling overflow cases.
 */
typedef struct {
    Message_t message;                   // The emergency event message containing details of the incident
    SemaphoreHandle_t xUnitsCountingSemaphore; // Semaphore for tracking available primary units

    QueueHandle_t departmentQueue;        // Queue for department-level task management
    QueueHandle_t unitQueue;              // Queue for assigning the emergency event to a specific unit

    char *auxUnitName[2];                 // Names of auxiliary (backup) departments for overflow handling
    QueueHandle_t auxUnitQueue[2];        // Queues for auxiliary units (used if primary units are unavailable)
    SemaphoreHandle_t xAuxUnitsCountingSemaphore[2]; // Semaphores for auxiliary unit availability
} EventDaemonParams_t;

/* 
 * Global Queues and Semaphores for Emergency Dispatch System 
 * These queues and semaphores are used for inter-task communication 
 * and synchronization within the FreeRTOS-based simulation.
 */

/* Logging and Emergency Call Queues */
QueueHandle_t logQueue = NULL;                    // Queue for logging messages
QueueHandle_t emergencyCallQueue = NULL;          // Queue for incoming emergency calls

/* Department Request Queues */
QueueHandle_t policeDepartmentQueue = NULL;       // Queue for police department task assignments
QueueHandle_t ambulanceDepartmentQueue = NULL;    // Queue for ambulance department task assignments
QueueHandle_t fireDepartmentQueue = NULL;         // Queue for fire department task assignments

/* Unit Assignment Queues */
QueueHandle_t policeUnitsQueue = NULL;            // Queue for assigning tasks to police units
QueueHandle_t ambulanceUnitsQueue = NULL;         // Queue for assigning tasks to ambulance units
QueueHandle_t fireUnitsQueue = NULL;              // Queue for assigning tasks to fire department units

/* Fault Management Queue */
QueueHandle_t faultManagementQueue = NULL;        // Queue for handling failed or unassigned emergency events

/* Semaphores for Tracking Available Units */
SemaphoreHandle_t xPoliceUnitsCountingSemaphore = NULL;    // Semaphore for managing police unit availability
SemaphoreHandle_t xAmbulanceUnitsCountingSemaphore = NULL; // Semaphore for managing ambulance unit availability
SemaphoreHandle_t xFireUnitsCountingSemaphore = NULL;      // Semaphore for managing fire unit availability

/* 
 * File pointer for logging system actions.
 * This file is used to store log messages generated during the simulation,
 * allowing for debugging and performance analysis.
 */
FILE *logFile;

/* 
 * Function Prototypes for Emergency Dispatch Simulation
 * These functions are responsible for logging, event handling, 
 * task execution, and managing emergency response units.
 */

/* Logging Functions */
void logMsgSend(QueueHandle_t logQueue, enum LogMessageType type, char* taskName, char* message);  
// Sends a log message to the logging queue.

char* LogMessageType2string(enum LogMessageType type);  
// Converts a log message type (INFO, WARNING, ERROR) to a string.

char* EmergencyType2string(enum EmergencyType type);  
// Converts an emergency type (POLICE, AMBULANCE, FIRE) to a string.

char* PriorityLevel2string(enum PriorityLevel level);  
// Converts a priority level (LOW, MEDIUM, HIGH) to a string.

/* Task Function Prototypes */
void vLoggingTask(void *pvParameters);  
// Task responsible for handling log messages and writing them to a file.

void vEmergencyEventTask(void *pvParameters);  
// Task that generates emergency events and submits them to the dispatcher.

void vEmergencyDespatcherTask(void *pvParameters);  
// Task that processes emergency calls and assigns them to the correct department.

void vFaultManagementTask(void *pvParameters);  
// Task that handles failed or unassigned emergency events.

/* Department and Unit Tasks */
void vDepartmentTask(void *pvParameters);  
// Task representing an emergency department (Police, Ambulance, Fire), handling emergency requests.

void vUnitTask(void *pvParameters);  
// Task representing an individual emergency response unit that handles an event.

void vEventDaemonTask(void *pvParameters);  
// Task that ensures emergency events are assigned to available units, including overflow handling.

/* Timer Callback Prototype */
void vRandomEventTimerCallback(TimerHandle_t xRandomEventTimer);  
// Timer callback function that generates random emergency events.

/* 
 * Signal Handler for Interrupts (SIGINT)
 * This function is called when the program receives a SIGINT signal (e.g., Ctrl+C).
 * It ensures that logs are properly flushed and closed before terminating the program.
 */
void handle_sigint(int sig)
{
    printf("Caught signal %d\n", sig);  // Print the received signal number

    // Ensure the log file is properly closed before exiting
    if (logFile != NULL) {
        fflush(logFile);  // Flush any remaining data in the log buffer
        fclose(logFile);  // Close the log file
    }

    exit(0);  // Terminate the program gracefully
}

/* 
 * Main function for the City Emergency Dispatch Simulation.
 * Initializes the system, sets up logging, starts emergency event handling, 
 * and begins the FreeRTOS scheduler.
 */
int my_proj(void)
{
    // Register signal handler to ensure graceful shutdown on SIGINT (Ctrl+C)
    signal(SIGINT, handle_sigint);

    // Create a queue for logging messages
    logQueue = xQueueCreate(10, sizeof(LogMessage_t));
    if (logQueue == NULL) {
        printf("Failed to create log queue.\n");
    }

    /* Create a task to handle logging */
    xTaskCreate(vLoggingTask, "LoggingTask", configMINIMAL_STACK_SIZE, (void*)logQueue, tskIDLE_PRIORITY + 1, NULL);

    /* Create a software timer to generate emergency events periodically */
    TimerHandle_t xRandomEventTimer = xTimerCreate(
        "Timer1",                   // Timer name (for debugging)
        pdMS_TO_TICKS(2000),        // Timer period (2 seconds)
        pdTRUE,                     // Auto-reload (runs repeatedly)
        (void *)0,                  // Timer ID (optional)
        vRandomEventTimerCallback   // Callback function
    );

    // Check if the timer was successfully created
    if (xRandomEventTimer == NULL) {
        printf("Failed to create timer.\n");
        return 1;
    }

    /* Start the timer */
    if (xTimerStart(xRandomEventTimer, 0) != pdPASS) {
        printf("Failed to start timer.\n");
        return 1;
    }
    
    /* Create a task to handle emergency event dispatching */
    xTaskCreate(vEmergencyDespatcherTask, "EmergencyDespatcherTask", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 5, NULL);

    /* Start the FreeRTOS scheduler, which begins executing tasks */
    vTaskStartScheduler();

    return 1; // This should never be reached unless scheduler fails
}

/* 
 * Function to send a log message to the logging queue.
 * This ensures that logs are processed asynchronously by the logging task.
 *
 * Parameters:
 *  - logQueue: The FreeRTOS queue where log messages will be sent.
 *  - type: The log message type (INFO, WARNING, ERROR).
 *  - taskName: The name of the task that generated the log.
 *  - message: The log message content.
 */
void logMsgSend(QueueHandle_t logQueue, enum LogMessageType type, char* taskName, char* message) {
    LogMessage_t logMessage;

    // Set log message details
    logMessage.messageType = type;
    snprintf(logMessage.taskName, sizeof(logMessage.taskName), "%s", taskName);
    snprintf(logMessage.message, sizeof(logMessage.message), "%s", message);

    // Send the log message to the queue, ensuring non-blocking operation
    if (xQueueSend(logQueue, &logMessage, portMAX_DELAY) != pdPASS) {
        printf("Failed to send log message.\n"); // Fallback in case of queue failure
    }
}

/* 
 * Logging Task
 * This task continuously listens for log messages from the logging queue,
 * formats them with timestamps, writes them to a log file, and prints them to the console.
 *
 * Parameters:
 *  - pvParameters: Pointer to task parameters (unused in this case).
 */
void vLoggingTask(void *pvParameters) {
    LogMessage_t logMessage;
    time_t rawtime;
    struct tm *timeinfo;
    char logLine[320];

    // Open log file in write mode (overwrites previous logs)
    logFile = fopen("log.txt", "w");
    if (logFile == NULL) {
        printf("Failed to open log file.\n");
        vTaskDelete(NULL); // Terminate task if file opening fails
    }

    printf("Logging task is running.\n");

    // Infinite loop to process log messages
    for (;;) {
        // Receive a log message from the queue (blocks indefinitely until a message is available)
        if (xQueueReceive(logQueue, &logMessage, portMAX_DELAY) == pdPASS) {
            // Get the current system time
            time(&rawtime);
            timeinfo = localtime(&rawtime);

            // Format log message with timestamp
            snprintf(logLine, sizeof(logLine), "[Time: %02d:%02d:%02d] [%s] [%s] %s\n",
                timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec, 
                LogMessageType2string(logMessage.messageType), logMessage.taskName, logMessage.message);

            // Write log message to the log file and flush output
            fprintf(logFile, "%s", logLine);
            fflush(logFile); // Ensure the message is immediately written to the file

            // Print log message to the console
            printf("%s", logLine);
        }
        else {
            // Handle failure to receive log messages
            printf("Failed to receive log message.\n");
        }
    }
}

/* 
 * Emergency Event Task
 * This task simulates an incoming emergency event, assigns it random attributes, 
 * sends it to the dispatcher, and waits for a response.
 *
 * Parameters:
 *  - pvParameters: Pointer to task parameters (unused in this case).
 */
void vEmergencyEventTask(void *pvParameters) {
    QueueHandle_t responseQueue;
    Message_t emergencyCall;
    char logMsg[256];

    // Create a private response queue for this task (only 1 message at a time)
    responseQueue = xQueueCreate(1, sizeof(Message_t));
    if (responseQueue == NULL) {
        printf("%s: Failed to create response queue.\n", pcTaskGetName(NULL));
        vTaskDelete(NULL); // Terminate task if queue creation fails
    }

    // Assign response queue and task handle to emergency call structure
    emergencyCall.responseQueue = responseQueue;
    emergencyCall.taskHandle = xTaskGetCurrentTaskHandle();
    
    // Set a random time limit between 10 and 50 seconds
    emergencyCall.timeLimit = xTaskGetTickCount() + pdMS_TO_TICKS((10 + rand() % 60) * 1000);

    // Randomly assign an emergency type (0 = Police, 1 = Ambulance, 2 = Fire)
    emergencyCall.emergencyType = rand() % 3;

    // Randomly assign a priority level (0 = Low, 1 = Medium, 2 = High)
    emergencyCall.priorityLevel = rand() % 3;

    // Log event details
    sprintf(logMsg, "Event: %s, Priority: %s, Time limit: %ld seconds",
        EmergencyType2string(emergencyCall.emergencyType), 
        PriorityLevel2string(emergencyCall.priorityLevel), 
        (emergencyCall.timeLimit - xTaskGetTickCount()) / 1000);
    
    logMsgSend(logQueue, LOG_INFO, pcTaskGetName(NULL), logMsg);

    // Send emergency event to the dispatcher queue
    if (xQueueSend(emergencyCallQueue, &emergencyCall, emergencyCall.timeLimit - xTaskGetTickCount()) == pdPASS) {
        // Wait for a response within the time limit
        if (xQueueReceive(responseQueue, &emergencyCall, emergencyCall.timeLimit - xTaskGetTickCount()) == pdPASS) {
            logMsgSend(logQueue, LOG_INFO, pcTaskGetName(NULL), "The event was successfully completed");
        } else {
            logMsgSend(logQueue, LOG_ERROR, pcTaskGetName(NULL), "The event wasn't completed in time");
        }
    } else {
        logMsgSend(logQueue, LOG_ERROR, pcTaskGetName(NULL), "The call wasn't answered");
    }

    // Cleanup: Set response queue to NULL, delete the queue, and terminate the task
    emergencyCall.responseQueue = NULL;
    vQueueDelete(responseQueue);
    vTaskDelete(NULL);
}

/* 
 * Emergency Dispatcher Task
 * This task is responsible for receiving emergency calls from the queue,
 * determining the appropriate department (Police, Ambulance, Fire),
 * and forwarding the request to the correct department's request queue.
 * It also initializes department structures and manages unit resources.
 *
 * Parameters:
 *  - pvParameters: Pointer to task parameters (unused in this case).
 */
void vEmergencyDespatcherTask(void *pvParameters) {
    Message_t message;
    char logMsg[256];

    // Allocate memory for department parameters
    DepartmentParams_t *policeParams = (DepartmentParams_t *)pvPortMalloc(sizeof(DepartmentParams_t));
    DepartmentParams_t *ambulanceParams = (DepartmentParams_t *)pvPortMalloc(sizeof(DepartmentParams_t));
    DepartmentParams_t *fireParams = (DepartmentParams_t *)pvPortMalloc(sizeof(DepartmentParams_t));

    // Create queues for emergency event handling
    faultManagementQueue = xQueueCreate(10, REQUEST_QUEUE_ITEM_SIZE);
    policeDepartmentQueue = xQueueCreate(1, REQUEST_QUEUE_ITEM_SIZE);
    ambulanceDepartmentQueue = xQueueCreate(1, REQUEST_QUEUE_ITEM_SIZE);
    fireDepartmentQueue = xQueueCreate(1, REQUEST_QUEUE_ITEM_SIZE);
    
    // Create queues for managing available units in each department
    policeUnitsQueue = xQueueCreate(POLICE_UNIT_COUNT, REQUEST_QUEUE_ITEM_SIZE);
    ambulanceUnitsQueue = xQueueCreate(AMBULANCE_UNIT_COUNT, REQUEST_QUEUE_ITEM_SIZE);
    fireUnitsQueue = xQueueCreate(FIRE_UNIT_COUNT, REQUEST_QUEUE_ITEM_SIZE);
    
    // Create semaphores for tracking available resources in each department
    xPoliceUnitsCountingSemaphore = xSemaphoreCreateCounting(POLICE_UNIT_COUNT, POLICE_UNIT_COUNT);
    xAmbulanceUnitsCountingSemaphore = xSemaphoreCreateCounting(AMBULANCE_UNIT_COUNT, AMBULANCE_UNIT_COUNT);
    xFireUnitsCountingSemaphore = xSemaphoreCreateCounting(FIRE_UNIT_COUNT, FIRE_UNIT_COUNT);

    // Create a queue to receive emergency calls
    emergencyCallQueue = xQueueCreate(1, REQUEST_QUEUE_ITEM_SIZE);

    // Log dispatcher startup
    logMsgSend(logQueue, LOG_INFO, pcTaskGetName(NULL), "Dispatcher task is running");

    // Initialize department structures with names and available units
    sprintf(policeParams->unitName, "Police");
    sprintf(ambulanceParams->unitName, "Ambulance");
    sprintf(fireParams->unitName, "Fire");

    policeParams->unitsAvailable = POLICE_UNIT_COUNT;
    ambulanceParams->unitsAvailable = AMBULANCE_UNIT_COUNT;
    fireParams->unitsAvailable = FIRE_UNIT_COUNT;

    // Assign request and unit queues to departments
    policeParams->requestQueue = policeDepartmentQueue;
    ambulanceParams->requestQueue = ambulanceDepartmentQueue;
    fireParams->requestQueue = fireDepartmentQueue;

    policeParams->unitQueue = policeUnitsQueue;
    ambulanceParams->unitQueue = ambulanceUnitsQueue;
    fireParams->unitQueue = fireUnitsQueue;

    // Assign semaphores for unit availability tracking
    policeParams->xUnitsCountingSemaphore = xPoliceUnitsCountingSemaphore;
    ambulanceParams->xUnitsCountingSemaphore = xAmbulanceUnitsCountingSemaphore;
    fireParams->xUnitsCountingSemaphore = xFireUnitsCountingSemaphore;

    // Assign auxiliary department resources for overflow handling
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

    // Create supporting tasks for handling faults and department-level processing
    xTaskCreate(vFaultManagementTask, "FaultManagementTask", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 3, NULL);
    xTaskCreate(vDepartmentTask, "PoliceDepartmentTask", configMINIMAL_STACK_SIZE, (void*)policeParams, tskIDLE_PRIORITY + 2, NULL);
    xTaskCreate(vDepartmentTask, "AmbulanceDepartmentTask", configMINIMAL_STACK_SIZE, (void*)ambulanceParams, tskIDLE_PRIORITY + 2, NULL);
    xTaskCreate(vDepartmentTask, "FireDepartmentTask", configMINIMAL_STACK_SIZE, (void*)fireParams, tskIDLE_PRIORITY + 2, NULL);

    // Infinite loop to process incoming emergency calls
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

    // Cleanup memory (this part is unreachable due to the infinite loop, but for completeness)
    vPortFree(policeParams);
    vPortFree(ambulanceParams);
    vPortFree(fireParams);
}

/* 
 * Department Task
 * This task represents an emergency response department (Police, Ambulance, Fire).
 * It manages its units by spawning unit tasks, receives emergency requests,
 * and delegates them to available units or auxiliary backup units.
 *
 * Parameters:
 *  - pvParameters: Pointer to the DepartmentParams_t structure containing department-specific data.
 */
void vDepartmentTask(void *pvParameters)
{
    DepartmentParams_t *params = (DepartmentParams_t *)pvParameters; // Retrieve department parameters
    UnitParams_t *unitParams = (UnitParams_t *)pvPortMalloc(sizeof(UnitParams_t)); // Allocate memory for unit parameters
    Message_t message;
    char logMsg[256];

    // Log department startup
    sprintf(logMsg, "%s department task is running", params->unitName);
    logMsgSend(logQueue, LOG_INFO, pcTaskGetName(NULL), logMsg);

    // Assign unit queue and semaphore for managing resources
    unitParams->eventQueue = params->unitQueue;
    unitParams->xUnitsCountingSemaphore = params->xUnitsCountingSemaphore;

    // Create tasks for each available unit in this department
    for (int i = 0; i < params->unitsAvailable; i++) {
        char taskName[32], tmp_str[64];
        snprintf(tmp_str, sizeof(tmp_str), "%sUnitTask%d", params->unitName, i);
        snprintf(taskName, sizeof(taskName), "%.31s", tmp_str); // Ensure name fits within buffer
        xTaskCreate(vUnitTask, taskName, configMINIMAL_STACK_SIZE, (void*)unitParams, tskIDLE_PRIORITY + 1 + i, NULL);
    }

    // Infinite loop to handle incoming emergency requests
    for (;;) {
        if(xQueueReceive(params->requestQueue, &message, portMAX_DELAY) == pdPASS) {
            // Allocate memory for event daemon parameters
            EventDaemonParams_t *eventDaemonParams = (EventDaemonParams_t *)pvPortMalloc(sizeof(EventDaemonParams_t));

            // Populate event daemon parameters with relevant department information
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

            // Create an event daemon task to manage this emergency request
            xTaskCreate(vEventDaemonTask, "EventDaemonTask", configMINIMAL_STACK_SIZE, (void*)eventDaemonParams, tskIDLE_PRIORITY + 2, NULL);
        }
    }

    // Cleanup (unreachable due to infinite loop, but for completeness)
    vPortFree(unitParams); 
}

/* 
 * Event Daemon Task
 * This task is responsible for dispatching emergency events to available units.
 * It first tries to assign the event to a primary unit. If no units are available,
 * it attempts to use backup auxiliary units. If no units are available, wait until the deadline
 * for a unit to become free. If all options fail, the event is
 * forwarded to the fault management queue for further handling.
 *
 * Parameters:
 *  - pvParameters: Pointer to EventDaemonParams_t containing event details and resource tracking.
 */
void vEventDaemonTask(void *pvParameters)
{
    EventDaemonParams_t *params = (EventDaemonParams_t *)pvParameters;
    char logMsg[256];

    if (params)
    {
        // Try to allocate a primary unit for the emergency event
        if (xSemaphoreTake(params->xUnitsCountingSemaphore, 0) == pdPASS)
        {
            // Send the emergency message to the unit's queue
            if (xQueueSend(params->unitQueue, &params->message, 0) != pdPASS) {
                logMsgSend(logQueue, LOG_ERROR, pcTaskGetName(NULL), "Failed to send the message to the original unit");
                xSemaphoreGive(params->xUnitsCountingSemaphore); // Release the unit if assignment fails
                xQueueSend(faultManagementQueue, &params->message, 0); // Forward to fault management
            }
        }
        // If primary unit is unavailable, attempt to assign to the first auxiliary unit
        else if (xSemaphoreTake(params->xAuxUnitsCountingSemaphore[0], 0) == pdPASS)
        {
            if (xQueueSend(params->auxUnitQueue[0], &params->message, 0) != pdPASS) {
                sprintf(logMsg, "Failed to send the message to the spare unit %s", params->auxUnitName[0]);
                logMsgSend(logQueue, LOG_ERROR, pcTaskGetName(NULL), logMsg);
                xSemaphoreGive(params->xAuxUnitsCountingSemaphore[0]); // Release auxiliary unit if failed
                xQueueSend(faultManagementQueue, &params->message, 0);
            }
            else
            {
                sprintf(logMsg, "Sent the message to the spare unit %s", params->auxUnitName[0]);
                logMsgSend(logQueue, LOG_INFO, pcTaskGetName(NULL), logMsg);
                xQueueSend(faultManagementQueue, &params->message, 0); // Log that auxiliary unit handled it
            }
        }
        // If the first auxiliary unit is unavailable, try the second auxiliary unit
        else if (xSemaphoreTake(params->xAuxUnitsCountingSemaphore[1], 0) == pdPASS)
        {
            if (xQueueSend(params->auxUnitQueue[1], &params->message, 0) != pdPASS) {
                sprintf(logMsg, "Failed to send the message to the spare unit %s", params->auxUnitName[1]);
                logMsgSend(logQueue, LOG_ERROR, pcTaskGetName(NULL), logMsg);
                xSemaphoreGive(params->xAuxUnitsCountingSemaphore[1]); // Release auxiliary unit if failed
                xQueueSend(faultManagementQueue, &params->message, 0);
            }
            else
            {
                sprintf(logMsg, "Sent the message to the spare unit %s", params->auxUnitName[1]);
                logMsgSend(logQueue, LOG_INFO, pcTaskGetName(NULL), logMsg);
            }
        }
        // If no units are available, wait until the deadline for a unit to become free
        else if (params->message.timeLimit > xTaskGetTickCount()
            && xSemaphoreTake(params->xUnitsCountingSemaphore, params->message.timeLimit - xTaskGetTickCount()) == pdPASS) 
        {
            // Attempt to send the message after waiting for an available unit
            if (xQueueSend(params->unitQueue, &params->message, 0) != pdPASS) {
                logMsgSend(logQueue, LOG_ERROR, pcTaskGetName(NULL), "Failed to send the message to the original unit");
                xSemaphoreGive(params->xUnitsCountingSemaphore);
                xQueueSend(faultManagementQueue, &params->message, 0);
            }
        } 
        // If no units became available, log a failure and send the event to fault management
        else {
            sprintf(logMsg, "No available units for %s event", EmergencyType2string(params->message.emergencyType));
            logMsgSend(logQueue, LOG_WARNING, pcTaskGetName(NULL), logMsg);
            xQueueSend(faultManagementQueue, &params->message, 0);
        }
    }

    // Free allocated memory for this task's parameters
    vPortFree(params);
    // Delete the event daemon task after execution
    vTaskDelete(NULL);
}

/* 
 * Unit Task
 * This task represents an emergency response unit (e.g., a police car, ambulance, or fire truck).
 * It continuously listens for assigned emergency events, processes them, and reports completion or failure.
 *
 * Parameters:
 *  - pvParameters: Pointer to UnitParams_t containing unit-specific data such as queues and semaphores.
 */
void vUnitTask(void *pvParameters)
{
    UnitParams_t *params = (UnitParams_t *)pvParameters; // Retrieve unit parameters
    Message_t message;
    char logMsg[256];

    // Log that the unit task has started
    logMsgSend(logQueue, LOG_INFO, pcTaskGetName(NULL), "Unit task is running");

    // Infinite loop to process assigned emergency events
    for (;;) {
        // Wait to receive an emergency event message
        if (xQueueReceive(params->eventQueue, &message, portMAX_DELAY) == pdPASS) {
            
            // Log received event details
            sprintf(logMsg, "Got message of: %s", pcTaskGetName(message.taskHandle));
            logMsgSend(logQueue, LOG_INFO, pcTaskGetName(NULL), logMsg);

            sprintf(logMsg, "Attending the event of emergency call: %s, Priority: %s, Time limit: %ld seconds",
                pcTaskGetName(message.taskHandle), 
                PriorityLevel2string(message.priorityLevel), 
                (message.timeLimit - xTaskGetTickCount()) / 1000);
            logMsgSend(logQueue, LOG_INFO, pcTaskGetName(NULL), logMsg);

            // Simulate event handling duration with a random response time (5s - 50s)
            int responseTime = 5000 + rand() % 50000;

            // Check if the response can be completed within the allowed time
            if (message.timeLimit < xTaskGetTickCount() + pdMS_TO_TICKS(responseTime)) {
                logMsgSend(logQueue, LOG_WARNING, pcTaskGetName(NULL), "Dropping - the event won't be completed in time");

                // If the event can't be completed in time, send it to fault management
                xQueueSend(faultManagementQueue, &message, 0);
            } else {
                // Simulate event processing delay
                vTaskDelay(pdMS_TO_TICKS(responseTime));

                // Validate that the response queue exists
                if (message.responseQueue == NULL) {
                    logMsgSend(logQueue, LOG_ERROR, pcTaskGetName(NULL), "Response queue is NULL");
                }
                // Attempt to send the response back
                else if (xQueueSend(message.responseQueue, &message, 0) != pdPASS) {
                    logMsgSend(logQueue, LOG_ERROR, pcTaskGetName(NULL), "Failed to send response");
                }
                else {
                    logMsgSend(logQueue, LOG_INFO, pcTaskGetName(NULL), "Completed the event");
                }
            }
        }

        // Mark the unit as available again by releasing its semaphore
        logMsgSend(logQueue, LOG_INFO, pcTaskGetName(NULL), "Unit is available");
        xSemaphoreGive(params->xUnitsCountingSemaphore);
    }
}

/* 
 * Fault Management Task
 * This task is responsible for handling emergency events that were not completed successfully.
 * It logs an appropriate message based on the type of emergency that failed to be resolved.
 *
 * Parameters:
 *  - pvParameters: Pointer to task parameters (unused in this case).
 */
void vFaultManagementTask(void *pvParameters)
{
    char logMsg[256];
    Message_t message;

    // Infinite loop to monitor failed emergency events
    for (;;) {
        // Wait for an event to be received from the fault management queue
        if (xQueueReceive(faultManagementQueue, &message, portMAX_DELAY) == pdPASS) {
            
            // Handle failure based on the type of emergency
            if (message.emergencyType == POLICE) {
                sprintf(logMsg, "The %s event for %s wasn't complete, escalating to higher police authority",
                        EmergencyType2string(message.emergencyType), pcTaskGetName(message.taskHandle));
                logMsgSend(logQueue, LOG_INFO, pcTaskGetName(NULL), logMsg);
            } 
            else if (message.emergencyType == AMBULANCE) {
                sprintf(logMsg, "The %s event for %s wasn't complete, contacting emergency backup medical team",
                        EmergencyType2string(message.emergencyType), pcTaskGetName(message.taskHandle));
                logMsgSend(logQueue, LOG_INFO, pcTaskGetName(NULL), logMsg);
            } 
            else if (message.emergencyType == FIRE) {
                sprintf(logMsg, "The %s event for %s wasn't complete, dispatching additional fire response team",
                        EmergencyType2string(message.emergencyType), pcTaskGetName(message.taskHandle));
                logMsgSend(logQueue, LOG_INFO, pcTaskGetName(NULL), logMsg);
            }
        }
    }
}

/* 
 * Random Event Timer Callback
 * This function is executed each time the software timer expires.
 * It creates a new emergency event task and dynamically updates the timer interval.
 *
 * Parameters:
 *  - xRandomEventTimer: Handle to the timer that triggered the callback.
 */
void vRandomEventTimerCallback(TimerHandle_t xRandomEventTimer) {
    (void)xRandomEventTimer; // Unused parameter, suppress compiler warnings

    static uint32_t emergencyEventNumber = 0;
    char emergencyEventTaskName[32];

    /* Generate a unique task name for the new emergency event */
    sprintf(emergencyEventTaskName, "EmergencyEventTask%d", emergencyEventNumber++);

    /* Create a new emergency event task */
    xTaskCreate(vEmergencyEventTask, emergencyEventTaskName, configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 4, NULL);

    /* Update the timer period randomly between 1 and 4 seconds */
    if (xTimerChangePeriod(xRandomEventTimer, pdMS_TO_TICKS(1000 + rand() % 3000), 0) != pdPASS) {
        printf("Failed to update timer period.\n");
    }

    /* Restart the timer to continue generating emergency events */
    if (xTimerStart(xRandomEventTimer, 0) != pdPASS) {
        printf("Failed to restart timer.\n");
    }
}

/* 
 * Converts a LogMessageType enum value to a human-readable string.
 * 
 * Parameters:
 *  - type: The log message type (LOG_INFO, LOG_WARNING, LOG_ERROR).
 * 
 * Returns:
 *  - A string representation of the log message type.
 */
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

/* 
 * Converts an EmergencyType enum value to a human-readable string.
 * 
 * Parameters:
 *  - type: The type of emergency (POLICE, AMBULANCE, FIRE).
 * 
 * Returns:
 *  - A string representation of the emergency type.
 */
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

/* 
 * Converts a PriorityLevel enum value to a human-readable string.
 * 
 * Parameters:
 *  - level: The priority level (LOW, MEDIUM, HIGH).
 * 
 * Returns:
 *  - A string representation of the priority level.
 */
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
