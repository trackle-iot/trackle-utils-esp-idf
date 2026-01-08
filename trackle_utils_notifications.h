#ifndef TRACKLE_UTILS_NOTIFICATIONS_H
#define TRACKLE_UTILS_NOTIFICATIONS_H

#include <stdbool.h>
#include <esp_types.h>
#include <esp_timer.h>

/**
 *
 * @file trackle_utils_notifications.h
 * @brief Datatypes and functions for working with notifications. Read the full description to learn about notifications
 *
 * Notifications are a construct that allows to let the cloud know that an event has happened.
 *
 * Every notification is characterized by two elements:
 *  - A (human readable) name that identifies the notification;
 *  - An event name that will be displayed by the Trackle platform when the notification is received.
 *
 * Moreover, at runtime, every notification has:
 *  - A level, that is an unsigned int;
 *  - A value, that can be a signed or unsigned int, or a floating point.
 *
 * When the level of the notification changes, the event is published to the cloud, along with the actual value of the notification.
 * The value's purpose is to give some context about the cause that triggered the change of the notification's level.
 *
 * In order to create notifications, one must follow these steps:
 *  1. Declare a variable of type \ref Trackle_NotificationID_t;
 *  2. Assign the result of \ref Trackle_Notification_create to this variable;
 *  3. Repeat the steps from 1 to 2 for all the notifications that must be created;
 *  4. Call \ref Trackle_Notifications_startTask to start the notifications task.
 *
 * Now, one can work with notifications (update, read, etc.) by using the remaining functions exposed by this file.
 *
 */

/**
 * @brief Max number of notifications that can be created.
 */
#define TRACKLE_MAX_NOTIFICATIONS_NUM 20

/**
 * @brief Value returned on error by functions returning \ref Trackle_NotificationID_t
 */
#define Trackle_NotificationID_ERROR -1

/**
 * @brief Type of the ID of an notification.
 */
typedef int Trackle_NotificationID_t;

/**
 * @brief Create a new notification.
 * @param name Name/key to be assigned to the notification.
 * @param eventName Name of the event where to publish the notification (e.g. "machine/speed")
 * @param format Printf style format for the message. It must contain, in order: %s for the notification key, %u for the level, and %s for the value.
 * @param scale Divider to be applied to values used to update the notification (notificationValue = newValue / scale)
 * @param numDecimals Number of decimal digits to be used when publishing the notification value to the cloud. It's used only if \ref scale differs from 1 (otherwise the notification's value is an integer and it doesn't make sense).
 * @param sign If true, the notification's value is signed, otherwise it's unsigned. It's used only if \ref scale equals 1 (otherwise the notification's value is a floating point number and is signed by default).
 * @return ID associated with the new created notification, or \ref Trackle_NotificationID_ERROR on failure.
 */
Trackle_NotificationID_t Trackle_Notification_create(const char *name, const char *eventName, const char *format, uint16_t scale, uint8_t numDecimals, bool sign);

/**
 * @brief Create a new notification with value mapping to strings.
 * @param name Name/key to be assigned to the notification.
 * @param eventName Name of the event where to publish the notification (e.g. "machine/speed")
 * @param format Printf style format for the message. It must contain, in order: %s for the notification key, %u for the level, and %s for the value.
 * @param scale Divider to be applied to values used to update the notification (notificationValue = newValue / scale)
 * @param numDecimals Number of decimal digits to be used when publishing the notification value to the cloud. It's used only if \ref scale differs from 1 (otherwise the notification's value is an integer and it doesn't make sense).
 * @param sign If true, the notification's value is signed, otherwise it's unsigned. It's used only if \ref scale equals 1 (otherwise the notification's value is a floating point number and is signed by default).
 * @param valueMap Optional array of strings to map numeric values to strings. If provided and value is a valid index, the string will be used instead of the numeric value.
 * @param valueMapSize Size of the valueMap array. Set to 0 if valueMap is not used.
 * @return ID associated with the new created notification, or \ref Trackle_NotificationID_ERROR on failure.
 */
Trackle_NotificationID_t Trackle_Notification_createWithValueMap(const char *name, const char *eventName, const char *format, uint16_t scale, uint8_t numDecimals, bool sign, const char **valueMap, uint8_t valueMapSize);

/**
 * @brief Update the value of an notification.
 * @param notificationID ID of the notification to be updated.
 * @param newLevel Unsigned integer representing the level of the notification.
 * @param value New value of the notification.
 * @return true if update was successful, false otherwise.
 */
bool Trackle_Notification_update(Trackle_NotificationID_t notificationID, uint8_t newLevel, int value);

/**
 * @brief Start the task that publishes periodically the notifications created.
 * @return true if task started successfully, false otherwise.
 */
bool Trackle_Notifications_startTask();

/**
 * @brief Get key of an notification.
 * @param notificationID ID of the notification.
 * @return Pointer to the name/key of the notification (empty string if \ref notificationID doesn't identify a valid notification)
 */
const char *Trackle_Notification_getKey(Trackle_NotificationID_t notificationID);

/**
 * @brief Get level of an notification.
 * @param notificationID ID of the notification.
 * @return Actual level of the notification (-1 if \ref notificationID doesn't identify a valid notification)
 */
int32_t Trackle_Notification_getLevel(Trackle_NotificationID_t notificationID);

/**
 * @brief Get value of an notification.
 * @param notificationID ID of the notification.
 * @return Value of the notification (-1 if \ref notificationID doesn't identify a valid notification)
 */
int32_t Trackle_Notification_getValue(Trackle_NotificationID_t notificationID);

#endif