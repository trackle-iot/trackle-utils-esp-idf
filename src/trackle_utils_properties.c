#include <trackle_utils_properties.h>

#include <string.h>
#include <inttypes.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

#include <trackle_esp32.h>

#define JSON_BUFFER_LEN 1024 // Length of the buffer that holds the JSON string of the properties while it's being built.

#define TRACKLE_PROPERTIES_TASK_NAME "trackle_utils_properties"
#define TRACKLE_PROPERTIES_TASK_STACK_SIZE 8192
#define TRACKLE_PROPERTIES_TASK_PRIORITY (tskIDLE_PRIORITY + 10)
#define TRACKLE_PROPERTIES_TASK_CORE_ID 1
#define TRACKLE_PROPERTIES_TASK_PERIOD_MS 100

static const char *TAG = "trackle_utils_properties";
static const char *EMPTY_STRING = "";

// Property data structure
typedef struct
{
    char key[TRACKLE_MAX_PROP_NAME_LENGTH]; // Property name/key
    bool changed;                           // True if read value is changed
    bool sign;                              // True if int32, false if uint32
    int32_t lastPubValue;                   // Latest read value
    int32_t setValue;                       // Latest set value
    uint16_t scale;                         // Scale factor (divides new value when set)
    bool disabled;                          // If disabled, property is ignored from publish
    uint8_t numDecimals;                    // Number of decimal digits (only used if scale is set)
    bool setToPublish;                      // True if added to JSON to publish
    char *lastPubStringValue;               // String value
    char *setStringValue;                   // If this is not NULL, property is a string property and this is its value
    int stringValueMaxLength;               // Max length of the string contained in \ref stringValue field

    // Debounce
    bool debouncing;          // Set to true if a value was set with debouncing
    uint32_t latestSetTimeMs; // Latest time the property was set
    uint32_t debounceDelayMs; // Delay to wait before setting the property to changed

} Prop_t;

// Property group data structure
typedef struct
{
    bool onlyIfChanged;                                   // If true, update the properties within only if their values changed.
    Trackle_PropID_t propsIndexes[TRACKLE_MAX_PROPS_NUM]; // Indexes (different from IDs) of the properties in the group.
    int propsWithin;                                      // Number of properties in the group (number of valid elements in propsIndexes)
    uint32_t periodMs;                                    // Period of publication of the group in milliseconds
    uint32_t latestWakeTimeMs;                            // Latest time the group's properties were published
} PropGroup_t;

static PropGroup_t propGroups[TRACKLE_MAX_PROPGROUPS_NUM] = {0}; // Array holding the properties groups created by the user.
static int numPropGroupsCreated = 0;                             // Number of the property groups created (aka next property group ID available)

static Prop_t props[TRACKLE_MAX_PROPS_NUM] = {0}; // Array holding the properties created by the user.
static int numPropsCreated = 0;                   // Number of the properties created (aka next property ID available)

static int32_t defaultValue = 0;   //  Default value of a new property
static bool defaultChanged = true; // Default changed value of a property

Trackle_PropGroupID_t Trackle_PropGroup_create(uint32_t periodMs, bool onlyIfChanged)
{
    if (numPropGroupsCreated < TRACKLE_MAX_PROPGROUPS_NUM)
    {
        const int newPropGroupIndex = numPropGroupsCreated;
        propGroups[newPropGroupIndex].latestWakeTimeMs = 0; // 0 is not significant here, it must be updated on task start with current time
        propGroups[newPropGroupIndex].onlyIfChanged = onlyIfChanged;
        propGroups[newPropGroupIndex].propsWithin = 0;
        propGroups[newPropGroupIndex].periodMs = periodMs;
        numPropGroupsCreated++;
        return newPropGroupIndex + 1; // Convert internal property group index to property group ID by incrementing it.
    }
    return Trackle_PropGroupID_ERROR;
}

bool Trackle_PropGroup_addProp(Trackle_PropID_t propId, Trackle_PropGroupID_t propGroupId)
{
    const int propIndex = propId - 1;           // Convert property ID to internal property index by decrementing it.
    const int propGroupIndex = propGroupId - 1; // Convert property group ID to internal property group index by decrementing it.
    if (propGroupIndex >= 0 && propGroupIndex < numPropGroupsCreated && propIndex < numPropsCreated && propIndex >= 0 && propGroups[propGroupId].propsWithin < TRACKLE_MAX_PROPS_NUM - 1)
    {
        const int propsWithin = propGroups[propGroupIndex].propsWithin;
        for (int i = 0; i < propsWithin; i++)
        {
            if (propGroups[propGroupIndex].propsIndexes[i] == propIndex)
            {
                return false; // Fail, property already in this group
            }
        }
        propGroups[propGroupIndex].propsIndexes[propsWithin] = propIndex;
        propGroups[propGroupIndex].propsWithin++;
        return true;
    }
    return false;
}

static char *lastCharPtr(char *s)
{
    return &(s[strlen(s)]);
}

static void appendPropertyToJsonString(char *jsonBuffer, int propIndex)
{
    char *jsonBufferTail = lastCharPtr(jsonBuffer);
    if (strlen(jsonBuffer) > 1)
    {
        jsonBufferTail += sprintf(jsonBufferTail, ",");
    }
    if (props[propIndex].setStringValue != NULL)
    { // string
        jsonBufferTail += sprintf(jsonBufferTail, "\"%s\":\"%s\"", props[propIndex].key, props[propIndex].setStringValue);
    }
    else if (props[propIndex].scale == 1)
    { // integer
        if (props[propIndex].sign)
        { // uint, remove sign
            jsonBufferTail += sprintf(jsonBufferTail, "\"%s\":%" PRIu32, props[propIndex].key, (uint32_t)props[propIndex].setValue);
        }
        else
        {
            jsonBufferTail += sprintf(jsonBufferTail, "\"%s\":%" PRIi32, props[propIndex].key, props[propIndex].setValue);
        }
    }
    else
    { // double
        char strFormat[20];
        sprintf(strFormat, "\"%%s\":%%.%df", (int)(props[propIndex].numDecimals));
        jsonBufferTail += sprintf(jsonBufferTail, strFormat, props[propIndex].key, ((double)props[propIndex].setValue) / props[propIndex].scale);
    }
}

static bool isSetValueEqualToLastSent(int propIndex)
{
    if (props[propIndex].setStringValue != NULL)
    {
        // This is a string-property
        return strcmp(props[propIndex].setStringValue, props[propIndex].lastPubStringValue) == 0;
    }
    return props[propIndex].setValue == props[propIndex].lastPubValue;
}

static void updateLastSentToSetValue(int propIndex)
{
    if (props[propIndex].setStringValue != NULL)
        strcpy(props[propIndex].lastPubStringValue, props[propIndex].setStringValue);
    else
        props[propIndex].lastPubValue = props[propIndex].setValue;
}

static bool isMsElapsed(uint32_t now, uint32_t start, uint32_t delay)
{
    if (now < start)
        return (now + UINT32_MAX - start + 1) >= delay;
    return now - start >= delay;
}

static void tracklePropertiesTaskCode(void *arg)
{

    static char jsonBuffer[JSON_BUFFER_LEN];
    jsonBuffer[0] = '\0';

    TickType_t latestWakeTime = xTaskGetTickCount();
    bool first_run = true;

    // Consider this instant as 0 in the time of the properties
    for (int pgIdx = 0; pgIdx < numPropGroupsCreated; pgIdx++)
    {
        propGroups[pgIdx].latestWakeTimeMs = latestWakeTime * portTICK_PERIOD_MS;
    }

    for (;;)
    {
        bool propsToPublish = false;

        vTaskDelayUntil(&latestWakeTime, TRACKLE_PROPERTIES_TASK_PERIOD_MS / portTICK_PERIOD_MS);
        const uint32_t nowMs = xTaskGetTickCount() * portTICK_PERIOD_MS;

        if (trackleConnected(trackle_s))
        {
            // For each group...
            for (int pgIdx = 0; pgIdx < numPropGroupsCreated; pgIdx++)
            {

                const int propsWithin = propGroups[pgIdx].propsWithin;
                const bool onlyIfChanged = propGroups[pgIdx].onlyIfChanged;

                // ... if its period is elapsed ...
                if (isMsElapsed(nowMs, propGroups[pgIdx].latestWakeTimeMs, propGroups[pgIdx].periodMs) || first_run)
                {

                    propGroups[pgIdx].latestWakeTimeMs = nowMs;

                    // ... for each property in the group ...
                    for (int i = 0; i < propsWithin; i++)
                    {
                        const int propIdx = propGroups[pgIdx].propsIndexes[i];

                        if (props[propIdx].debouncing && isMsElapsed(nowMs, props[propIdx].latestSetTimeMs, props[propIdx].debounceDelayMs))
                        {
                            props[propIdx].debouncing = false;
                            props[propIdx].changed = true;
                        }

                        // ... if it's changed or it must be published anyway ...
                        if (!props[propIdx].disabled && ((props[propIdx].changed && !isSetValueEqualToLastSent(propIdx)) || !onlyIfChanged || first_run))
                        {
                            // ... add it to JSON string to publish.
                            if (!propsToPublish)
                            {
                                propsToPublish = true;
                                strcat(jsonBuffer, "{");
                            }
                            appendPropertyToJsonString(jsonBuffer, propIdx);
                            props[propIdx].setToPublish = true;
                            updateLastSentToSetValue(propIdx);
                        }
                    }
                }
            }

            // If there is at least a property in the JSON string to publish, publish it.
            if (propsToPublish)
            {
                strcat(jsonBuffer, "}");
                bool publishedSuccessfully = trackleSyncStateSecure(jsonBuffer);
                if (publishedSuccessfully)
                {
                    for (int pIdx = 0; pIdx < numPropsCreated; pIdx++)
                    {
                        if (props[pIdx].setToPublish)
                        {
                            props[pIdx].changed = false;
                        }
                    }
                    first_run = false;
                }
                for (int pIdx = 0; pIdx < numPropsCreated; pIdx++)
                {
                    props[pIdx].setToPublish = false;
                }
                jsonBuffer[0] = '\0';
            }
        }
    }
}

bool Trackle_Props_startTask()
{

    ESP_LOGI(TAG, "Initializing...");

    // Task creation
    BaseType_t taskCreationRes;

    taskCreationRes = xTaskCreatePinnedToCore(tracklePropertiesTaskCode,
                                              TRACKLE_PROPERTIES_TASK_NAME,
                                              TRACKLE_PROPERTIES_TASK_STACK_SIZE,
                                              NULL,
                                              TRACKLE_PROPERTIES_TASK_PRIORITY,
                                              NULL,
                                              TRACKLE_PROPERTIES_TASK_CORE_ID);

    if (taskCreationRes == pdTRUE)
    {
        ESP_LOGI(TAG, "Task created successfully.");
        return true;
    }
    ESP_LOGE(TAG, "Error in task creation.");
    return false;
}

int Trackle_Props_getNumber()
{
    return numPropsCreated;
}

Trackle_PropID_t Trackle_Prop_create(const char *name, uint16_t scale, uint8_t numDecimals, bool sign)
{
    if (numPropsCreated < TRACKLE_MAX_PROPS_NUM)
    {
        const int newPropIndex = numPropsCreated;
        for (int pIdx = 0; pIdx < numPropsCreated; pIdx++)
        {
            if (strcmp(name, props[pIdx].key) == 0)
            {
                return Trackle_PropID_ERROR;
            }
        }
        if (strlen(name) < TRACKLE_MAX_PROP_NAME_LENGTH)
        {
            strcpy(props[newPropIndex].key, name);
        }
        else
        {
            return Trackle_PropID_ERROR;
        }
        props[newPropIndex].lastPubValue = defaultValue;
        props[newPropIndex].setValue = defaultValue;
        props[newPropIndex].scale = scale;
        props[newPropIndex].sign = sign;
        props[newPropIndex].numDecimals = numDecimals;
        props[newPropIndex].disabled = false;
        props[newPropIndex].changed = defaultChanged;
        props[newPropIndex].setToPublish = false;
        props[newPropIndex].lastPubStringValue = NULL;
        props[newPropIndex].setStringValue = NULL;
        props[newPropIndex].stringValueMaxLength = 0;
        props[newPropIndex].debouncing = false;
        props[newPropIndex].latestSetTimeMs = 0;
        props[newPropIndex].debounceDelayMs = 0;
        numPropsCreated++;
        return newPropIndex + 1; // Convert internal property index to property ID by incrementing it.
    }
    return Trackle_PropID_ERROR;
}

Trackle_PropID_t Trackle_Prop_createString(const char *name, int maxLength)
{
    if (numPropsCreated < TRACKLE_MAX_PROPS_NUM)
    {
        const int newPropIndex = numPropsCreated;
        for (int pIdx = 0; pIdx < numPropsCreated; pIdx++)
        {
            if (strcmp(name, props[pIdx].key) == 0)
            {
                return Trackle_PropID_ERROR;
            }
        }
        if (strlen(name) < TRACKLE_MAX_PROP_NAME_LENGTH)
        {
            strcpy(props[newPropIndex].key, name);
        }
        else
        {
            return Trackle_PropID_ERROR;
        }
        props[newPropIndex].lastPubValue = defaultValue;
        props[newPropIndex].setValue = defaultValue;
        props[newPropIndex].scale = 1;
        props[newPropIndex].sign = 0;
        props[newPropIndex].numDecimals = 0;
        props[newPropIndex].disabled = false;
        props[newPropIndex].changed = defaultChanged;
        props[newPropIndex].setToPublish = false;
        props[newPropIndex].lastPubStringValue = malloc(maxLength * sizeof(char) + 1); // +1 for null character
        if (props[newPropIndex].lastPubStringValue == NULL)
            return Trackle_PropID_ERROR;
        props[newPropIndex].lastPubStringValue[0] = '\0';
        props[newPropIndex].setStringValue = malloc(maxLength * sizeof(char) + 1); // +1 for null character
        if (props[newPropIndex].setStringValue == NULL)
            return Trackle_PropID_ERROR;
        props[newPropIndex].setStringValue[0] = '\0';
        props[newPropIndex].stringValueMaxLength = maxLength;
        props[newPropIndex].debouncing = false;
        props[newPropIndex].latestSetTimeMs = 0;
        props[newPropIndex].debounceDelayMs = 0;
        numPropsCreated++;
        return newPropIndex + 1; // Convert internal property index to property ID by incrementing it.
    }
    return Trackle_PropID_ERROR;
}

bool Trackle_Prop_update(Trackle_PropID_t propID, int newValue)
{
    const int propIndex = propID - 1; // Convert property ID to internal property index by decrementing it.
    if (propIndex >= 0 && propIndex < numPropsCreated)
    {
        if (props[propIndex].setValue != newValue)
        {
            ESP_LOGD(TAG, "PROP CHANGED ---- %s: old: %" PRIi32 ", new: %d", props[propIndex].key, props[propIndex].setValue, newValue);
            props[propIndex].debouncing = true;
            props[propIndex].latestSetTimeMs = xTaskGetTickCount() * portTICK_PERIOD_MS;
            props[propIndex].setValue = newValue;
            return true;
        }
    }
    return false;
}

bool Trackle_Prop_updateString(Trackle_PropID_t propID, const char *newValue)
{
    const int propIndex = propID - 1; // Convert property ID to internal property index by decrementing it.
    if (propIndex >= 0 && propIndex < numPropsCreated)
    {
        if (props[propIndex].setStringValue != NULL && newValue != NULL && strcmp(props[propIndex].setStringValue, newValue) != 0)
        {
            ESP_LOGD(TAG, "PROP CHANGED ---- %s: old: %s, new: %s", props[propIndex].key, props[propIndex].setStringValue, newValue);
            props[propIndex].debouncing = true;
            props[propIndex].latestSetTimeMs = xTaskGetTickCount() * portTICK_PERIOD_MS;
            strncpy(props[propIndex].setStringValue, newValue, props[propIndex].stringValueMaxLength);
            props[propIndex].setStringValue[props[propIndex].stringValueMaxLength] = '\0';
            return true;
        }
    }
    return false;
}

bool Trackle_Prop_setDisabled(Trackle_PropID_t propID, bool isDisabled)
{
    const int propIndex = propID - 1; // Convert property ID to internal property index by decrementing it.
    if (propIndex >= 0 && propIndex < numPropsCreated)
    {
        props[propIndex].disabled = isDisabled;
        return true;
    }
    return false;
}

bool Trackle_Prop_setDebounceDelay(Trackle_PropID_t propID, uint32_t debounceDelayMs)
{
    const int propIndex = propID - 1; // Convert property ID to internal property index by decrementing it.
    if (propIndex >= 0 && propIndex < numPropsCreated)
    {
        props[propIndex].debounceDelayMs = debounceDelayMs;
        return true;
    }
    return false;
}

bool Trackle_Prop_isDisabled(Trackle_PropID_t propID)
{
    const int propIndex = propID - 1; // Convert property ID to internal property index by decrementing it.
    if (propIndex >= 0 && propIndex < numPropsCreated)
    {
        return props[propIndex].disabled;
    }
    return false;
}

const char *Trackle_Prop_getKey(Trackle_PropID_t propID)
{
    const int propIndex = propID - 1; // Convert property ID to internal property index by decrementing it.
    if (propIndex >= 0 && propIndex < numPropsCreated)
    {
        return props[propIndex].key;
    }
    return EMPTY_STRING;
}

int32_t Trackle_Prop_getValue(Trackle_PropID_t propID)
{
    const int propIndex = propID - 1; // Convert property ID to internal property index by decrementing it.
    if (propIndex >= 0 && propIndex < numPropsCreated)
    {
        return props[propIndex].setValue;
    }
    return -1;
}

bool Trackle_Prop_getStringValue(Trackle_PropID_t propID, char *retValue, int retValueMaxLen)
{
    const int propIndex = propID - 1; // Convert property ID to internal property index by decrementing it.
    if (propIndex >= 0 && propIndex < numPropsCreated)
    {
        if (props[propIndex].setStringValue != NULL)
        {
            strncpy(retValue, props[propIndex].setStringValue, retValueMaxLen);
            retValue[retValueMaxLen] = '\0';
            return true;
        }
    }
    return false;
}

uint16_t Trackle_Prop_getScale(Trackle_PropID_t propID)
{
    const int propIndex = propID - 1; // Convert property ID to internal property index by decrementing it.
    if (propIndex >= 0 && propIndex < numPropsCreated)
    {
        return props[propIndex].scale;
    }
    return 0;
}

uint8_t Trackle_Prop_getNumberOfDecimals(Trackle_PropID_t propID)
{
    const int propIndex = propID - 1; // Convert property ID to internal property index by decrementing it.
    if (propIndex >= 0 && propIndex < numPropsCreated)
    {
        return props[propIndex].numDecimals;
    }
    return 0;
}

bool Trackle_Prop_isSigned(Trackle_PropID_t propID)
{
    const int propIndex = propID - 1; // Convert property ID to internal property index by decrementing it.
    if (propIndex >= 0 && propIndex < numPropsCreated)
    {
        return props[propIndex].sign;
    }
    return false;
}

void Trackle_Prop_setDefaults(int32_t value, bool changed)
{
    defaultValue = value;
    defaultChanged = changed;
}
