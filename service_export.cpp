/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "hwservicemanager"

#include <utils/Log.h>

#include <inttypes.h>
#include <base/logging.h>
#include <log/log.h>
#include <linux/MessageLooper.h>
#include <linux/binder.h>

#include <android/hidl/token/1.0/ITokenManager.h>
#include <cutils/properties.h>
#include <hidl/HidlBinderSupport.h>
#include <hidl/HidlTransportSupport.h>
#include <hidl/Status.h>
#include <hwbinder/IPCThreadState.h>
#include <hwbinder/ProcessState.h>
#include <utils/Errors.h>
#include <utils/Looper.h>
#include <utils/StrongPointer.h>

#include "ServiceManager.h"
#include "TokenManager.h"

 // libutils:
using android::sp;
using android::Looper;
using android::LooperCallback;

// libhwbinder:
using android::hardware::BHwBinder;
using android::hardware::IBinder;
using android::hardware::IPCThreadState;
using android::hardware::ProcessState;

// libhidl
using android::hardware::handleTransportPoll;
using android::hardware::setRequestingSid;
using android::hardware::HidlReturnRestriction;
using android::hardware::setProcessHidlReturnRestriction;
using android::hardware::setupTransportPolling;
using android::hardware::toBinder;

// implementations
using android::hidl::manager::implementation::ServiceManager;
using android::hidl::manager::V1_0::IServiceManager;
using android::hidl::token::V1_0::implementation::TokenManager;

static std::string serviceName = "default";

class HwBinderCallback : public LooperCallback
{
public:
    static sp<HwBinderCallback> setupTo( const sp<Looper>& looper )
    {
        sp<HwBinderCallback> cb = new HwBinderCallback;

        int fdHwBinder = setupTransportPolling();
        LOG_ALWAYS_FATAL_IF( fdHwBinder < 0, "Failed to setupTransportPolling: %d", fdHwBinder );

        // Flush after setupPolling(), to make sure the binder driver
        // knows about this thread handling commands.
        IPCThreadState::self()->flushCommands();

        int ret = looper->addFd( fdHwBinder,
                                 Looper::POLL_CALLBACK,
                                 Looper::EVENT_INPUT,
                                 cb,
                                 nullptr /*data*/ );
        LOG_ALWAYS_FATAL_IF( ret != 1, "Failed to add binder FD to Looper" );

        return cb;
    }

    int handleEvent( int fd, int /*events*/, void* /*data*/ ) override
    {
        handleTransportPoll( fd );
        return 1;  // Continue receiving callbacks.
    }
};

class ClientCallbackCallback : public virtual android::RefBase
{
public:

    inline static constexpr int s_handle_interval_ms = 5000;

    ClientCallbackCallback( const sp<ServiceManager>& manager )
    {
        mManager = manager;
    }

    bool handleEvent()
    {
        mManager->handleClientCallbacks();
        return false;
    }

private:

    sp<ServiceManager> mManager;
};

extern "C"
{
__declspec( dllexport ) int hw_service_manager_init()
{
    // If hwservicemanager crashes, the system may be unstable and hard to debug. This is both why
    // we log this and why we care about this at all.
    setProcessHidlReturnRestriction( HidlReturnRestriction::ERROR_IF_UNCHECKED );

    // TODO(b/36424585): make fatal
    ProcessState::self()->setCallRestriction( ProcessState::CallRestriction::ERROR_IF_NOT_ONEWAY );

    sp<ServiceManager> manager = new ServiceManager();
    setRequestingSid( manager, true );

    if( !manager->add( serviceName, manager ).withDefault( false ) )
    {
        ALOGE( "Failed to register hwservicemanager with itself." );
    }

    sp<TokenManager> tokenManager = new TokenManager();
    if( !manager->add( serviceName, tokenManager ).withDefault( false ) )
    {
        ALOGE( "Failed to register ITokenManager with hwservicemanager." );
    }

    // Tell IPCThreadState we're the service manager
    sp<IBinder> binder = toBinder<IServiceManager>( manager );
    sp<BHwBinder> service = static_cast<BHwBinder*>( binder.get() ); // local binder object
    IPCThreadState::self()->setTheContextObject( service );
    // Then tell the kernel
    ProcessState::self()->becomeContextManager();

    int rc = property_set( "hwservicemanager.ready", "true" );
    if( rc )
    {
        ALOGE( "Failed to set \"hwservicemanager.ready\" (error %d). "\
               "HAL services will not start!\n", rc );
    }

    MessageLooper& looper = MessageLooper::GetDefault();
    std::function<bool()> timer_callback = std::bind( &ClientCallbackCallback::handleEvent,
                                                      std::make_shared<ClientCallbackCallback>( manager ) );
    looper.RegisterTimer( ClientCallbackCallback::s_handle_interval_ms, timer_callback );
    auto fun = std::bind( &IPCThreadState::handlePolledCommands, IPCThreadState::self() );
    porting_binder::register_binder_data_handler( fun, false );

    int binder_fd = -1;
    IPCThreadState::self()->setupPolling( &binder_fd );
    LOG_ALWAYS_FATAL_IF( binder_fd < 0, "Failed to setupPolling: %d", binder_fd );
    looper.PostTask( fun );

    ALOGI( "hwservicemanager is ready now." );

    return 0;
}

}
