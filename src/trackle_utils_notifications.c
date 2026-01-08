#include <trackle_utils_notifications.h>

#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

#include <trackle_esp32.h>

#define MESSAGE_BUFFER_LEN 1024 // Length of the buffer that holds the string of the notification while it's being built.

#define TRACKLE_NOTIFICATIONS_TASK_NAME "trackle_utils_notifications"
#define TRACKLE_NOTIFICATIONS_TASK_STACK_SIZE 8192
#define TRACKLE_NOTIFICATIONS_TASK_PRIORITY (tskIDLE_PRIORITY + 10)
#define TRACKLE_NOTIFICATIONS_TASK_CORE_ID 1
#define TRACKLE_NOTIFICATIONS_TASK_PERIOD_MS 1000

static const char *TAG = "trackle_utils_notifications";
static const char *EMPTY_STRING = "";

#define NOTIFICATION_NAME_LENGTH 64
#define NOTIFICATION_EVENT_LENGTH 64
#define NOTIFICATION_FORMAT_LENGTH 128

// Notification data structure
typedef struct
{
    char key[NOTIFICATION_NAME_LENGTH];      // Notification name/key
    char event[NOTIFICATION_EVENT_LENGTH];   // Notification event
    char format[NOTIFICATION_FORMAT_LENGTH]; // Notification format
    bool changed;                            // True if read value is changed
    bool sign;                               // True if int32, false if uint32
    int32_t value;                           // Latest read value
    uint16_t scale;                          // Scale factor (divides new value when set)
    uint8_t numDecimals;                     // Number of decimal digits (only used if scale is set)
    uint8_t level;
    const char **valueMap; // Optional array of strings to map value to string
    uint8_t valueMapSize;  // Size of valueMap array (0 if not used)
} Notification_t;

static Notification_t notifications[TRACKLE_MAX_NOTIFICATIONS_NUM] = {0}; // Array holding the notifications created by the user.
static int numNotificationsCreated = 0;                                   // Number of the notifications created (aka next notification ID available)

static bool makeMessageStringFromNotification(char *messageBuffer, int notificationIndex)
{
    static char valueBuffer[32];
    messageBuffer[0] = '\0';
    valueBuffer[0] = '\0';

    // Check if valueMap is available and value is a valid index
    if (notifications[notificationIndex].valueMap != NULL &&
        notifications[notificationIndex].valueMapSize > 0 &&
        notifications[notificationIndex].value >= 0 &&
        notifications[notificationIndex].value < notifications[notificationIndex].valueMapSize)
    {
        // Use mapped string value
        const char *mappedString = notifications[notificationIndex].valueMap[notifications[notificationIndex].value];
        if (mappedString != NULL)
        {
            // Add quotes around the string value
            snprintf(valueBuffer, sizeof(valueBuffer), "\"%s\"", mappedString);
        }
        else
        {
            // Fallback to numeric value if mapped string is NULL
            if (notifications[notificationIndex].sign)
            {
                sprintf(valueBuffer, "%d", notifications[notificationIndex].value);
            }
            else
            {
                sprintf(valueBuffer, "%u", notifications[notificationIndex].value);
            }
        }
    }
    else if (notifications[notificationIndex].scale == 1)
    { // integer
        if (notifications[notificationIndex].sign)
        { // signed int
            sprintf(valueBuffer, "%d", notifications[notificationIndex].value);
        }
        else
        { // unsigned int
            sprintf(valueBuffer, "%u", notifications[notificationIndex].value);
        }
    }
    else
    { // double
        char doubleFormatString[20];
        sprintf(doubleFormatString, "%%.%df", (int)(notifications[notificationIndex].numDecimals));
        sprintf(valueBuffer, doubleFormatString, ((double)notifications[notificationIndex].value) / notifications[notificationIndex].scale);
    }
    return sprintf(messageBuffer,
                   notifications[notificationIndex].format,
                   notifications[notificationIndex].key,
                   notifications[notificationIndex].level,
                   valueBuffer) >= 0;
}

static void trackleNotificationsTaskCode(void *arg)
{

    static char messageBuffer[MESSAGE_BUFFER_LEN];

    TickType_t latestWakeTime = xTaskGetTickCount();

    for (;;)
    {

        vTaskDelayUntil(&latestWakeTime, TRACKLE_NOTIFICATIONS_TASK_PERIOD_MS / portTICK_PERIOD_MS);

        // For each notification ...
        for (int aIdx = 0; aIdx < numNotificationsCreated; aIdx++)
        {
            // ... if its level changed ...
            if (notifications[aIdx].changed)
            {
                // ... make string representation and publish it.
                makeMessageStringFromNotification(messageBuffer, aIdx);
                const bool success = tracklePublishSecure(notifications[aIdx].event, messageBuffer);
                if (success) // on failure, publishing is retried at next period.
                {
                    notifications[aIdx].changed = false;
                }
            }
        }
    }
}

bool Trackle_Notifications_startTask()
{

    ESP_LOGI(TAG, "Initializing...");

    // Task creation
    BaseType_t taskCreationRes;

    taskCreationRes = xTaskCreatePinnedToCore(trackleNotificationsTaskCode,
                                              TRACKLE_NOTIFICATIONS_TASK_NAME,
                                              TRACKLE_NOTIFICATIONS_TASK_STACK_SIZE,
                                              NULL,
                                              TRACKLE_NOTIFICATIONS_TASK_PRIORITY,
                                              NULL,
                                              TRACKLE_NOTIFICATIONS_TASK_CORE_ID);

    if (taskCreationRes != ESP_OK)
    {
        ESP_LOGI(TAG, "Task created successfully.");
        return true;
    }
    ESP_LOGE(TAG, "Error in task creation.");
    return false;
}

Trackle_NotificationID_t Trackle_Notification_create(const char *name, const char *eventName, const char *format, uint16_t scale, uint8_t numDecimals, bool sign)
{
    return Trackle_Notification_createWithValueMap(name, eventName, format, scale, numDecimals, sign, NULL, 0);
}

Trackle_NotificationID_t Trackle_Notification_createWithValueMap(const char *name, const char *eventName, const char *format, uint16_t scale, uint8_t numDecimals, bool sign, const char **valueMap, uint8_t valueMapSize)
{
    if (numNotificationsCreated < TRACKLE_MAX_NOTIFICATIONS_NUM)
    {
        const int newNotificationIndex = numNotificationsCreated;
        for (int aIdx = 0; aIdx < numNotificationsCreated; aIdx++)
        {
            if (strcmp(name, notifications[aIdx].key) == 0)
            {
                return Trackle_NotificationID_ERROR;
            }
        }
        if (strlen(name) < NOTIFICATION_NAME_LENGTH)
        {
            strcpy(notifications[newNotificationIndex].key, name);
        }
        else
        {
            return Trackle_NotificationID_ERROR;
        }
        if (strlen(eventName) < NOTIFICATION_EVENT_LENGTH)
        {
            strcpy(notifications[newNotificationIndex].event, eventName);
        }
        else
        {
            return Trackle_NotificationID_ERROR;
        }
        if (strlen(format) < NOTIFICATION_FORMAT_LENGTH)
        {
            strcpy(notifications[newNotificationIndex].format, format);
        }
        else
        {
            return Trackle_NotificationID_ERROR;
        }
        notifications[newNotificationIndex].value = -1;
        notifications[newNotificationIndex].scale = scale;
        notifications[newNotificationIndex].sign = sign;
        notifications[newNotificationIndex].numDecimals = numDecimals;
        notifications[newNotificationIndex].changed = false;
        notifications[newNotificationIndex].level = 0;
        notifications[newNotificationIndex].valueMap = valueMap;
        notifications[newNotificationIndex].valueMapSize = valueMapSize;
        numNotificationsCreated++;
        return newNotificationIndex + 1; // Convert internal notification index to notification ID by incrementing it.
    }
    return Trackle_NotificationID_ERROR;
}

bool Trackle_Notification_update(Trackle_NotificationID_t notificationID, uint8_t newLevel, int value)
{
    const int notificationIndex = notificationID - 1; // Convert notification ID to internal notification index by decrementing it.
    if (notificationIndex >= 0 && notificationIndex < numNotificationsCreated)
    {
        if (notifications[notificationIndex].level != newLevel)
        {
            notifications[notificationIndex].changed = true;
            notifications[notificationIndex].value = value;
            notifications[notificationIndex].level = newLevel;
        }
        return true;
    }
    return false;
}

const char *Trackle_Notification_getKey(Trackle_NotificationID_t notificationID)
{
    const int notificationIndex = notificationID - 1; // Convert notification ID to internal notification index by decrementing it.
    if (notificationIndex >= 0 && notificationIndex < numNotificationsCreated)
    {
        return notifications[notificationIndex].key;
    }
    return EMPTY_STRING;
}

int32_t Trackle_Notification_getLevel(Trackle_NotificationID_t notificationID)
{
    const int notificationIndex = notificationID - 1; // Convert notification ID to internal notification index by decrementing it.
    if (notificationIndex >= 0 && notificationIndex < numNotificationsCreated)
    {
        return notifications[notificationIndex].level;
    }
    return -1;
}

int32_t Trackle_Notification_getValue(Trackle_NotificationID_t notificationID)
{
    const int notificationIndex = notificationID - 1; // Convert notification ID to internal notification index by decrementing it.
    if (notificationIndex >= 0 && notificationIndex < numNotificationsCreated)
    {
        return notifications[notificationIndex].value;
    }
    return -1;
}
