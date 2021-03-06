/*
 *  Copyright (c) 2017, The OpenThread Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

#include "ncp_wpantund.hpp"

#include <vector>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

extern "C" {
#include "spinel.h"
#include "wpan-dbus-v1.h"
#include "wpanctl-utils.h"
}

#include "common/code_utils.hpp"
#include "common/logging.hpp"

namespace ot {

namespace BorderRouter {

namespace Ncp {

/**
 * This string is used to filter the property changed signal from wpantund.
 */
const char *kDBusMatchPropChanged = "type='signal',interface='" WPANTUND_DBUS_APIv1_INTERFACE "',"
                                    "member='" WPANTUND_IF_SIGNAL_PROP_CHANGED "'";

#define OTBR_AGENT_DBUS_NAME_PREFIX "otbr.agent"

static void HandleDBusError(DBusError &aError)
{
    otbrLog(OTBR_LOG_ERR, "NCP DBus error %s: %s!", aError.name, aError.message);
    dbus_error_free(&aError);
}

DBusHandlerResult ControllerWpantund::HandlePropertyChangedSignal(DBusConnection *aConnection,
                                                                  DBusMessage *   aMessage,
                                                                  void *          aContext)
{
    (void)aConnection;
    return static_cast<ControllerWpantund *>(aContext)->HandlePropertyChangedSignal(*aMessage);
}

DBusHandlerResult ControllerWpantund::HandlePropertyChangedSignal(DBusMessage &aMessage)
{
    DBusHandlerResult result = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    DBusMessageIter   iter;
    const char *      key    = NULL;
    const char *      sender = dbus_message_get_sender(&aMessage);
    const char *      path   = dbus_message_get_path(&aMessage);

    if (sender && path && strcmp(sender, mInterfaceDBusName) && strstr(path, mInterfaceName))
    {
        // DBus name of the interface has changed, possibly caused by wpantund restarted,
        // We have to restart the border agent proxy.
        otbrLog(OTBR_LOG_WARNING, "NCP DBus name changed.");

        TmfProxyStart();
    }

    VerifyOrExit(dbus_message_is_signal(&aMessage, WPANTUND_DBUS_APIv1_INTERFACE, WPANTUND_IF_SIGNAL_PROP_CHANGED),
                 result = DBUS_HANDLER_RESULT_NOT_YET_HANDLED);

    VerifyOrExit(dbus_message_iter_init(&aMessage, &iter));
    dbus_message_iter_get_basic(&iter, &key);
    VerifyOrExit(key != NULL, result = DBUS_HANDLER_RESULT_NOT_YET_HANDLED);
    dbus_message_iter_next(&iter);

    otbrLog(OTBR_LOG_INFO, "NCP property %s changed.", key);
    SuccessOrExit(OTBR_ERROR_NONE == ParseEvent(key, &iter));

    result = DBUS_HANDLER_RESULT_HANDLED;

exit:
    return result;
}

otbrError ControllerWpantund::ParseEvent(const char *aKey, DBusMessageIter *aIter)
{
    otbrError ret = OTBR_ERROR_NONE;

    if (!strcmp(aKey, kWPANTUNDProperty_TmfProxyStream))
    {
        const uint8_t *buf     = NULL;
        uint16_t       locator = 0;
        uint16_t       port    = 0;
        uint16_t       len     = 0;
        {
            DBusMessageIter sub_iter;
            int             nelements = 0;

            dbus_message_iter_recurse(aIter, &sub_iter);
            dbus_message_iter_get_fixed_array(&sub_iter, &buf, &nelements);
            len = static_cast<uint16_t>(nelements);
        }

        // both port and locator are encoded in network endian.
        port = buf[--len];
        port |= buf[--len] << 8;
        locator = buf[--len];
        locator |= buf[--len] << 8;

        EventEmitter::Emit(kEventTmfProxyStream, buf, len, locator, port);
    }
    else if (!strcmp(aKey, kWPANTUNDProperty_NCPState))
    {
        const char *state = NULL;
        dbus_message_iter_get_basic(aIter, &state);

        otbrLog(OTBR_LOG_INFO, "state %s", state);

        EventEmitter::Emit(kEventThreadState, 0 == strcmp(state, "associated"));
    }
    else if (!strcmp(aKey, kWPANTUNDProperty_NetworkName))
    {
        const char *networkName = NULL;
        dbus_message_iter_get_basic(aIter, &networkName);

        otbrLog(OTBR_LOG_INFO, "network name %s...", networkName);
        EventEmitter::Emit(kEventNetworkName, networkName);
    }
    else if (!strcmp(aKey, kWPANTUNDProperty_NetworkXPANID))
    {
        uint64_t xpanid = 0;

        if (DBUS_TYPE_UINT64 == dbus_message_iter_get_arg_type(aIter))
        {
            dbus_message_iter_get_basic(aIter, &xpanid);
#if __BYTE_ORDER == __LITTLE_ENDIAN
            // convert to network endian
            for (uint8_t *p = reinterpret_cast<uint8_t *>(&xpanid), *q = p + sizeof(xpanid) - 1; p < q; ++p, --q)
            {
                uint8_t tmp = *p;
                *p          = *q;
                *q          = tmp;
            }
#endif
        }
        else if (DBUS_TYPE_ARRAY == dbus_message_iter_get_arg_type(aIter))
        {
            const uint8_t * bytes = NULL;
            int             count = 0;
            DBusMessageIter subIter;

            dbus_message_iter_recurse(aIter, &subIter);
            dbus_message_iter_get_fixed_array(&subIter, &bytes, &count);
            VerifyOrExit(count == kSizeExtPanId, ret = OTBR_ERROR_DBUS);
            memcpy(&xpanid, bytes, sizeof(xpanid));
        }

        otbrLog(OTBR_LOG_INFO, "xpanid %llu...", xpanid);
        EventEmitter::Emit(kEventExtPanId, reinterpret_cast<uint8_t *>(&xpanid));
    }
    else if (!strcmp(aKey, kWPANTUNDProperty_NetworkPSKc))
    {
        const uint8_t * pskc  = NULL;
        int             count = 0;
        DBusMessageIter subIter;

        dbus_message_iter_recurse(aIter, &subIter);
        dbus_message_iter_get_fixed_array(&subIter, &pskc, &count);
        VerifyOrExit(count == kSizePSKc, ret = OTBR_ERROR_DBUS);

        EventEmitter::Emit(kEventPSKc, pskc);
    }

exit:
    return ret;
}

dbus_bool_t ControllerWpantund::AddDBusWatch(struct DBusWatch *aWatch, void *aContext)
{
    static_cast<ControllerWpantund *>(aContext)->mWatches[aWatch] = true;
    return TRUE;
}

void ControllerWpantund::RemoveDBusWatch(struct DBusWatch *aWatch, void *aContext)
{
    static_cast<ControllerWpantund *>(aContext)->mWatches.erase(aWatch);
}

void ControllerWpantund::ToggleDBusWatch(struct DBusWatch *aWatch, void *aContext)
{
    static_cast<ControllerWpantund *>(aContext)->mWatches[aWatch] = (dbus_watch_get_enabled(aWatch) ? true : false);
}

otbrError ControllerWpantund::TmfProxyEnable(dbus_bool_t aEnable)
{
    otbrError    ret     = OTBR_ERROR_ERRNO;
    DBusMessage *message = NULL;
    const char * key     = kWPANTUNDProperty_TmfProxyEnabled;

    message = dbus_message_new_method_call(mInterfaceDBusName, mInterfaceDBusPath, WPANTUND_DBUS_APIv1_INTERFACE,
                                           WPANTUND_IF_CMD_PROP_SET);

    VerifyOrExit(message != NULL, errno = ENOMEM);

    VerifyOrExit(
        dbus_message_append_args(message, DBUS_TYPE_STRING, &key, DBUS_TYPE_BOOLEAN, &aEnable, DBUS_TYPE_INVALID),
        errno = EINVAL);

    VerifyOrExit(dbus_connection_send(mDBus, message, NULL), errno = ENOMEM);

    ret = OTBR_ERROR_NONE;

exit:
    if (message != NULL)
    {
        dbus_message_unref(message);
    }

    return ret;
}

ControllerWpantund::ControllerWpantund(const char *aInterfaceName)
    : mDBus(NULL)
{
    mInterfaceDBusName[0] = '\0';
    strncpy(mInterfaceName, aInterfaceName, sizeof(mInterfaceName));
}

otbrError ControllerWpantund::Init(void)
{
    otbrError ret = OTBR_ERROR_DBUS;
    DBusError error;
    char      dbusName[DBUS_MAXIMUM_NAME_LENGTH];

    dbus_error_init(&error);
    mDBus = dbus_bus_get(DBUS_BUS_STARTER, &error);
    if (!mDBus)
    {
        dbus_error_free(&error);
        mDBus = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    }
    VerifyOrExit(mDBus != NULL);

    VerifyOrExit(dbus_bus_register(mDBus, &error));

    sprintf(dbusName, "%s.%s", OTBR_AGENT_DBUS_NAME_PREFIX, mInterfaceName);
    otbrLog(OTBR_LOG_INFO, "NCP requesting DBus name %s...", dbusName);
    VerifyOrExit(dbus_bus_request_name(mDBus, dbusName, DBUS_NAME_FLAG_DO_NOT_QUEUE, &error) ==
                 DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER);

    VerifyOrExit(
        dbus_connection_set_watch_functions(mDBus, AddDBusWatch, RemoveDBusWatch, ToggleDBusWatch, this, NULL));

    dbus_bus_add_match(mDBus, kDBusMatchPropChanged, &error);
    VerifyOrExit(!dbus_error_is_set(&error));

    VerifyOrExit(dbus_connection_add_filter(mDBus, HandlePropertyChangedSignal, this, NULL));

    ret = OTBR_ERROR_NONE;

exit:
    if (dbus_error_is_set(&error))
    {
        HandleDBusError(error);
    }

    if (ret)
    {
        if (mDBus)
        {
            dbus_connection_unref(mDBus);
            mDBus = NULL;
        }
        otbrLog(OTBR_LOG_ERR, "NCP failed to initialize!");
    }

    return ret;
}

ControllerWpantund::~ControllerWpantund(void)
{
    TmfProxyStop();

    if (mDBus)
    {
        dbus_connection_unref(mDBus);
        mDBus = NULL;
    }
}

otbrError ControllerWpantund::TmfProxyStart(void)
{
    otbrError ret = OTBR_ERROR_ERRNO;

    VerifyOrExit(lookup_dbus_name_from_interface(mInterfaceDBusName, mInterfaceName) == 0,
                 otbrLog(OTBR_LOG_ERR, "NCP failed to find the interface!"), errno = ENODEV);

    // Populate the path according to source code of wpanctl, better to export a function.
    snprintf(mInterfaceDBusPath, sizeof(mInterfaceDBusPath), "%s/%s", WPANTUND_DBUS_PATH, mInterfaceName);

    ret = TmfProxyEnable(TRUE);

exit:

    return ret;
}

otbrError ControllerWpantund::TmfProxySend(const uint8_t *aBuffer, uint16_t aLength, uint16_t aLocator, uint16_t aPort)
{
    otbrError    ret     = OTBR_ERROR_ERRNO;
    DBusMessage *message = NULL;

    std::vector<uint8_t> data(aLength + sizeof(aLocator) + sizeof(aPort));
    const uint8_t *      value = data.data();
    const char *         key   = kWPANTUNDProperty_TmfProxyStream;

    memcpy(data.data(), aBuffer, aLength);
    data[aLength]     = (aLocator >> 8);
    data[aLength + 1] = (aLocator & 0xff);
    data[aLength + 2] = (aPort >> 8);
    data[aLength + 3] = (aPort & 0xff);

    message = dbus_message_new_method_call(mInterfaceDBusName, mInterfaceDBusPath, WPANTUND_DBUS_APIv1_INTERFACE,
                                           WPANTUND_IF_CMD_PROP_SET);

    VerifyOrExit(message != NULL, errno = ENOMEM);

    VerifyOrExit(dbus_message_append_args(message, DBUS_TYPE_STRING, &key, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &value,
                                          data.size(), DBUS_TYPE_INVALID),
                 errno = EINVAL);

    VerifyOrExit(dbus_connection_send(mDBus, message, NULL), errno = ENOMEM);

    ret = OTBR_ERROR_NONE;

exit:

    if (message)
    {
        dbus_message_unref(message);
    }

    return ret;
}

otbrError ControllerWpantund::TmfProxyStop(void)
{
    return mInterfaceDBusName[0] == '\0' ? OTBR_ERROR_NONE : TmfProxyEnable(FALSE);
}

void ControllerWpantund::UpdateFdSet(fd_set &aReadFdSet, fd_set &aWriteFdSet, fd_set &aErrorFdSet, int &aMaxFd)
{
    for (WatchMap::iterator it = mWatches.begin(); it != mWatches.end(); ++it)
    {
        if (!it->second)
        {
            continue;
        }

        DBusWatch *  watch = it->first;
        unsigned int flags = dbus_watch_get_flags(watch);
        int          fd    = dbus_watch_get_unix_fd(watch);

        if (fd < 0)
        {
            continue;
        }

        if (flags & DBUS_WATCH_READABLE)
        {
            FD_SET(fd, &aReadFdSet);
        }

        if ((flags & DBUS_WATCH_WRITABLE) && dbus_connection_has_messages_to_send(mDBus))
        {
            FD_SET(fd, &aWriteFdSet);
        }

        FD_SET(fd, &aErrorFdSet);

        if (fd > aMaxFd)
        {
            aMaxFd = fd;
        }
    }
}

void ControllerWpantund::Process(const fd_set &aReadFdSet, const fd_set &aWriteFdSet, const fd_set &aErrorFdSet)
{
    for (WatchMap::iterator it = mWatches.begin(); it != mWatches.end(); ++it)
    {
        if (!it->second)
        {
            continue;
        }

        DBusWatch *  watch = it->first;
        unsigned int flags = dbus_watch_get_flags(watch);
        int          fd    = dbus_watch_get_unix_fd(watch);

        if (fd < 0)
        {
            continue;
        }

        if ((flags & DBUS_WATCH_READABLE) && !FD_ISSET(fd, &aReadFdSet))
        {
            flags &= static_cast<unsigned int>(~DBUS_WATCH_READABLE);
        }

        if ((flags & DBUS_WATCH_WRITABLE) && !FD_ISSET(fd, &aWriteFdSet))
        {
            flags &= static_cast<unsigned int>(~DBUS_WATCH_WRITABLE);
        }

        if (FD_ISSET(fd, &aErrorFdSet))
        {
            flags |= DBUS_WATCH_ERROR;
        }

        dbus_watch_handle(watch, flags);
    }

    while (DBUS_DISPATCH_DATA_REMAINS == dbus_connection_get_dispatch_status(mDBus) &&
           dbus_connection_read_write_dispatch(mDBus, 0))
        ;
}

DBusMessage *ControllerWpantund::RequestProperty(const char *aKey)
{
    DBusMessage *message = NULL;
    DBusMessage *reply   = NULL;
    const int    timeout = DEFAULT_TIMEOUT_IN_SECONDS * 1000;
    DBusError    error;

    dbus_error_init(&error);
    VerifyOrExit((message = dbus_message_new_method_call(mInterfaceDBusName, mInterfaceDBusPath,
                                                         WPANTUND_DBUS_APIv1_INTERFACE, WPANTUND_IF_CMD_PROP_GET)) !=
                     NULL,
                 errno = ENOMEM);

    VerifyOrExit(dbus_message_append_args(message, DBUS_TYPE_STRING, &aKey, DBUS_TYPE_INVALID), errno = EINVAL);

    reply = dbus_connection_send_with_reply_and_block(mDBus, message, timeout, &error);

exit:

    if (dbus_error_is_set(&error))
    {
        HandleDBusError(error);
        errno = EREMOTEIO;
    }

    if (message)
    {
        dbus_message_unref(message);
    }

    return reply;
}

otbrError ControllerWpantund::RequestEvent(int aEvent)
{
    otbrError       ret     = OTBR_ERROR_ERRNO;
    DBusMessage *   message = NULL;
    DBusMessage *   reply   = NULL;
    DBusMessageIter iter;
    const char *    key     = NULL;
    const int       timeout = DEFAULT_TIMEOUT_IN_SECONDS * 1000;
    DBusError       error;

    switch (aEvent)
    {
    case kEventExtPanId:
        key = kWPANTUNDProperty_NetworkXPANID;
        break;
    case kEventThreadState:
        key = kWPANTUNDProperty_NCPState;
        break;
    case kEventNetworkName:
        key = kWPANTUNDProperty_NetworkName;
        break;
    case kEventPSKc:
        key = kWPANTUNDProperty_NetworkPSKc;
        break;
    default:
        otbrLog(OTBR_LOG_WARNING, "Unknown event %d", aEvent);
        break;
    }

    VerifyOrExit(key != NULL, errno = EINVAL);
    otbrLog(OTBR_LOG_DEBUG, "Requesting %s...", key);
    VerifyOrExit((message = dbus_message_new_method_call(mInterfaceDBusName, mInterfaceDBusPath,
                                                         WPANTUND_DBUS_APIv1_INTERFACE, WPANTUND_IF_CMD_PROP_GET)) !=
                     NULL,
                 errno = ENOMEM);

    VerifyOrExit(dbus_message_append_args(message, DBUS_TYPE_STRING, &key, DBUS_TYPE_INVALID), errno = EINVAL);

    dbus_error_init(&error);
    reply = dbus_connection_send_with_reply_and_block(mDBus, message, timeout, &error);
    VerifyOrExit(reply != NULL, HandleDBusError(error), ret = OTBR_ERROR_DBUS);
    VerifyOrExit(dbus_message_iter_init(reply, &iter), errno = ENOENT);

    {
        uint32_t status = 0;
        dbus_message_iter_get_basic(&iter, &status);
        VerifyOrExit(status == SPINEL_STATUS_OK, errno = EREMOTEIO);
    }

    dbus_message_iter_next(&iter);
    ret = ParseEvent(key, &iter);

exit:

    if (reply)
    {
        dbus_message_unref(reply);
    }

    if (message)
    {
        dbus_message_unref(message);
    }

    if (ret != OTBR_ERROR_NONE)
    {
        otbrLog(OTBR_LOG_WARNING, "Error requesting %s:%s", key, otbrErrorString(ret));
    }
    return ret;
}

otbrError ControllerWpantund::GetProperty(const char *aKey, uint8_t *aBuffer, size_t &aSize)
{
    otbrError    ret   = OTBR_ERROR_ERRNO;
    DBusMessage *reply = NULL;
    DBusError    error;

    dbus_error_init(&error);
    VerifyOrExit(reply = RequestProperty(aKey));

    {
        uint8_t *buffer = NULL;
        int      count  = 0;
        uint32_t status = 0;

        VerifyOrExit(dbus_message_get_args(reply, &error, DBUS_TYPE_INT32, &status, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
                                           &buffer, &count, DBUS_TYPE_INVALID),
                     errno = EINVAL);

        assert(!dbus_error_is_set(&error));
        VerifyOrExit(status == SPINEL_STATUS_OK, errno = EINVAL);

        aSize = static_cast<size_t>(count);
        memcpy(aBuffer, buffer, aSize);
    }

    ret = OTBR_ERROR_NONE;

exit:

    if (dbus_error_is_set(&error))
    {
        HandleDBusError(error);
    }

    if (reply)
    {
        dbus_message_unref(reply);
    }

    return ret;
}

const uint8_t *ControllerWpantund::GetEui64(void)
{
    const uint8_t *ret  = NULL;
    size_t         size = 0;

    SuccessOrExit(GetProperty(kWPANTUNDProperty_NCPHardwareAddress, mEui64, size));
    assert(size == kSizeEui64);
    ret = mEui64;

exit:
    return ret;
}

Controller *Controller::Create(const char *aInterfaceName)
{
    return new ControllerWpantund(aInterfaceName);
}

void Controller::Destroy(Controller *aController)
{
    delete static_cast<ControllerWpantund *>(aController);
}

} // namespace Ncp

} // namespace BorderRouter

} // namespace ot
