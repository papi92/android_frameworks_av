/*
**
** Copyright (C) 2008, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define LOG_TAG "CameraService"
//#define LOG_NDEBUG 0

#include <stdio.h>
#include <sys/types.h>
#include <pthread.h>

#include <binder/AppOpsManager.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/MemoryBase.h>
#include <binder/MemoryHeapBase.h>
#include <cutils/atomic.h>
#include <cutils/properties.h>
#include <gui/Surface.h>
#include <hardware/hardware.h>
#include <media/AudioSystem.h>
#include <media/mediaplayer.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/String16.h>

#include "CameraService.h"
#include "CameraClient.h"
#include "Camera2Client.h"
#include "ProCamera2Client.h"

namespace android {

// ----------------------------------------------------------------------------
// Logging support -- this is for debugging only
// Use "adb shell dumpsys media.camera -v 1" to change it.
volatile int32_t gLogLevel = 0;

#define LOG1(...) ALOGD_IF(gLogLevel >= 1, __VA_ARGS__);
#define LOG2(...) ALOGD_IF(gLogLevel >= 2, __VA_ARGS__);

static void setLogLevel(int level) {
    android_atomic_write(level, &gLogLevel);
}

// ----------------------------------------------------------------------------

static int getCallingPid() {
    return IPCThreadState::self()->getCallingPid();
}

static int getCallingUid() {
    return IPCThreadState::self()->getCallingUid();
}

// ----------------------------------------------------------------------------

// This is ugly and only safe if we never re-create the CameraService, but
// should be ok for now.
static CameraService *gCameraService;

CameraService::CameraService()
    :mSoundRef(0), mModule(0)
{
    ALOGI("CameraService started (pid=%d)", getpid());
    gCameraService = this;
}

void CameraService::onFirstRef()
{
    LOG1("CameraService::onFirstRef");

    BnCameraService::onFirstRef();

    if (hw_get_module(CAMERA_HARDWARE_MODULE_ID,
                (const hw_module_t **)&mModule) < 0) {
        ALOGE("Could not load camera HAL module");
        mNumberOfCameras = 0;
    }
    else {
        ALOGI("Loaded \"%s\" camera module", mModule->common.name);
        mNumberOfCameras = mModule->get_number_of_cameras();
        if (mNumberOfCameras > MAX_CAMERAS) {
            ALOGE("Number of cameras(%d) > MAX_CAMERAS(%d).",
                    mNumberOfCameras, MAX_CAMERAS);
            mNumberOfCameras = MAX_CAMERAS;
        }
        for (int i = 0; i < mNumberOfCameras; i++) {
            setCameraFree(i);
        }
    }
}

CameraService::~CameraService() {
    for (int i = 0; i < mNumberOfCameras; i++) {
        if (mBusy[i]) {
            ALOGE("camera %d is still in use in destructor!", i);
        }
    }

    gCameraService = NULL;
}

int32_t CameraService::getNumberOfCameras() {
    return mNumberOfCameras;
}

status_t CameraService::getCameraInfo(int cameraId,
                                      struct CameraInfo* cameraInfo) {
    if (!mModule) {
        return NO_INIT;
    }

    if (cameraId < 0 || cameraId >= mNumberOfCameras) {
        return BAD_VALUE;
    }

    struct camera_info info;
    status_t rc = mModule->get_camera_info(cameraId, &info);
    cameraInfo->facing = info.facing;
    cameraInfo->orientation = info.orientation;
    return rc;
}

int CameraService::getDeviceVersion(int cameraId, int* facing) {
    struct camera_info info;
    if (mModule->get_camera_info(cameraId, &info) != OK) {
        return -1;
    }

    int deviceVersion;
    if (mModule->common.module_api_version >= CAMERA_MODULE_API_VERSION_2_0) {
        deviceVersion = info.device_version;
    } else {
        deviceVersion = CAMERA_DEVICE_API_VERSION_1_0;
    }

    if (facing) {
        *facing = info.facing;
    }

    return deviceVersion;
}

sp<ICamera> CameraService::connect(
        const sp<ICameraClient>& cameraClient,
        int cameraId,
        const String16& clientPackageName,
        int clientUid) {

    String8 clientName8(clientPackageName);
    int callingPid = getCallingPid();

    LOG1("CameraService::connect E (pid %d \"%s\", id %d)", callingPid,
            clientName8.string(), cameraId);

    if (clientUid == USE_CALLING_UID) {
        clientUid = getCallingUid();
    } else {
        // We only trust our own process to forward client UIDs
        if (callingPid != getpid()) {
            ALOGE("CameraService::connect X (pid %d) rejected (don't trust clientUid)",
                    callingPid);
            return NULL;
        }
    }

    if (!mModule) {
        ALOGE("Camera HAL module not loaded");
        return NULL;
    }

    sp<Client> client;
    if (cameraId < 0 || cameraId >= mNumberOfCameras) {
        ALOGE("CameraService::connect X (pid %d) rejected (invalid cameraId %d).",
            callingPid, cameraId);
        return NULL;
    }

    char value[PROPERTY_VALUE_MAX];
    property_get("sys.secpolicy.camera.disabled", value, "0");
    if (strcmp(value, "1") == 0) {
        // Camera is disabled by DevicePolicyManager.
        ALOGI("Camera is disabled. connect X (pid %d) rejected", callingPid);
        return NULL;
    }

    Mutex::Autolock lock(mServiceLock);
    if (mClient[cameraId] != 0) {
        client = mClient[cameraId].promote();
        if (client != 0) {
            if (cameraClient->asBinder() == client->getCameraClient()->asBinder()) {
                LOG1("CameraService::connect X (pid %d) (the same client)",
                     callingPid);
                return client;
            } else {
                // TODOSC: need to support 1 regular client, multiple shared clients here
                ALOGW("CameraService::connect X (pid %d) rejected (existing client).",
                      callingPid);
                return NULL;
            }
        }
        mClient[cameraId].clear();
    }

    /*
    mBusy is set to false as the last step of the Client destructor,
    after which it is guaranteed that the Client destructor has finished (
    including any inherited destructors)

    We only need this for a Client subclasses since we don't allow
    multiple Clents to be opened concurrently, but multiple BasicClient
    would be fine
    */
    if (mBusy[cameraId]) {

        ALOGW("CameraService::connect X (pid %d, \"%s\") rejected"
                " (camera %d is still busy).", callingPid,
                clientName8.string(), cameraId);
        return NULL;
    }

    int facing = -1;
    int deviceVersion = getDeviceVersion(cameraId, &facing);

    switch(deviceVersion) {
      case CAMERA_DEVICE_API_VERSION_1_0:
        client = new CameraClient(this, cameraClient,
                clientPackageName, cameraId,
                facing, callingPid, clientUid, getpid());
        break;
      case CAMERA_DEVICE_API_VERSION_2_0:
      case CAMERA_DEVICE_API_VERSION_2_1:
        client = new Camera2Client(this, cameraClient,
                clientPackageName, cameraId,
                facing, callingPid, clientUid, getpid());
        break;
      case -1:
        ALOGE("Invalid camera id %d", cameraId);
        return NULL;
      default:
        ALOGE("Unknown camera device HAL version: %d", deviceVersion);
        return NULL;
    }

    if (client->initialize(mModule) != OK) {
        return NULL;
    }

    cameraClient->asBinder()->linkToDeath(this);

    mClient[cameraId] = client;
    LOG1("CameraService::connect X (id %d, this pid is %d)", cameraId, getpid());
    return client;
}

sp<IProCameraUser> CameraService::connect(
                                        const sp<IProCameraCallbacks>& cameraCb,
                                        int cameraId)
{
    int callingPid = getCallingPid();

    LOG1("CameraService::connectPro E (pid %d, id %d)", callingPid, cameraId);

    if (!mModule) {
        ALOGE("Camera HAL module not loaded");
        return NULL;
    }

    sp<ProClient> client;
    if (cameraId < 0 || cameraId >= mNumberOfCameras) {
        ALOGE("CameraService::connectPro X (pid %d) rejected (invalid cameraId %d).",
            callingPid, cameraId);
        return NULL;
    }

    char value[PROPERTY_VALUE_MAX];
    property_get("sys.secpolicy.camera.disabled", value, "0");
    if (strcmp(value, "1") == 0) {
        // Camera is disabled by DevicePolicyManager.
        ALOGI("Camera is disabled. connect X (pid %d) rejected", callingPid);
        return NULL;
    }

    int facing = -1;
    int deviceVersion = getDeviceVersion(cameraId, &facing);

    switch(deviceVersion) {
      case CAMERA_DEVICE_API_VERSION_1_0:
        ALOGE("Camera id %d uses HALv1, doesn't support ProCamera", cameraId);
        return NULL;
        break;
      case CAMERA_DEVICE_API_VERSION_2_0:
      case CAMERA_DEVICE_API_VERSION_2_1:
        client = new ProCamera2Client(this, cameraCb, String16(),
                cameraId, facing, callingPid, USE_CALLING_UID, getpid());
        break;
      case -1:
        ALOGE("Invalid camera id %d", cameraId);
        return NULL;
      default:
        ALOGE("Unknown camera device HAL version: %d", deviceVersion);
        return NULL;
    }

    if (client->initialize(mModule) != OK) {
        return NULL;
    }

    mProClientList[cameraId].push(client);

    cameraCb->asBinder()->linkToDeath(this);

    LOG1("CameraService::connectPro X (id %d, this pid is %d)", cameraId,
            getpid());
    return client;


    return NULL;
}

void CameraService::removeClientByRemote(const wp<IBinder>& remoteBinder) {
    int callingPid = getCallingPid();
    LOG1("CameraService::removeClientByRemote E (pid %d)", callingPid);

    // Declare this before the lock to make absolutely sure the
    // destructor won't be called with the lock held.
    Mutex::Autolock lock(mServiceLock);

    int outIndex;
    sp<Client> client = findClientUnsafe(remoteBinder, outIndex);

    if (client != 0) {
        // Found our camera, clear and leave.
        LOG1("removeClient: clear camera %d", outIndex);
        mClient[outIndex].clear();

        client->unlinkToDeath(this);
    } else {

        sp<ProClient> clientPro = findProClientUnsafe(remoteBinder);

        if (clientPro != NULL) {
            // Found our camera, clear and leave.
            LOG1("removeClient: clear pro %p", clientPro.get());

            clientPro->getRemoteCallback()->asBinder()->unlinkToDeath(this);
        }
    }

    LOG1("CameraService::removeClientByRemote X (pid %d)", callingPid);
}

sp<CameraService::ProClient> CameraService::findProClientUnsafe(
                        const wp<IBinder>& cameraCallbacksRemote)
{
    sp<ProClient> clientPro;

    for (int i = 0; i < mNumberOfCameras; ++i) {
        Vector<size_t> removeIdx;

        for (size_t j = 0; j < mProClientList[i].size(); ++j) {
            wp<ProClient> cl = mProClientList[i][j];

            sp<ProClient> clStrong = cl.promote();
            if (clStrong != NULL && clStrong->getRemote() == cameraCallbacksRemote) {
                clientPro = clStrong;
                break;
            } else if (clStrong == NULL) {
                // mark to clean up dead ptr
                removeIdx.push(j);
            }
        }

        // remove stale ptrs (in reverse so the indices dont change)
        for (ssize_t j = (ssize_t)removeIdx.size() - 1; j >= 0; --j) {
            mProClientList[i].removeAt(removeIdx[j]);
        }

    }

    return clientPro;
}

sp<CameraService::Client> CameraService::findClientUnsafe(
                        const wp<IBinder>& cameraClient, int& outIndex) {
    sp<Client> client;

    for (int i = 0; i < mNumberOfCameras; i++) {

        // This happens when we have already disconnected (or this is
        // just another unused camera).
        if (mClient[i] == 0) continue;

        // Promote mClient. It can fail if we are called from this path:
        // Client::~Client() -> disconnect() -> removeClientByRemote().
        client = mClient[i].promote();

        // Clean up stale client entry
        if (client == NULL) {
            mClient[i].clear();
            continue;
        }

        if (cameraClient == client->getCameraClient()->asBinder()) {
            // Found our camera
            outIndex = i;
            return client;
        }
    }

    outIndex = -1;
    return NULL;
}

CameraService::Client* CameraService::getClientByIdUnsafe(int cameraId) {
    if (cameraId < 0 || cameraId >= mNumberOfCameras) return NULL;
    return mClient[cameraId].unsafe_get();
}

Mutex* CameraService::getClientLockById(int cameraId) {
    if (cameraId < 0 || cameraId >= mNumberOfCameras) return NULL;
    return &mClientLock[cameraId];
}

sp<CameraService::BasicClient> CameraService::getClientByRemote(
                                const wp<IBinder>& cameraClient) {

    // Declare this before the lock to make absolutely sure the
    // destructor won't be called with the lock held.
    sp<BasicClient> client;

    Mutex::Autolock lock(mServiceLock);

    int outIndex;
    client = findClientUnsafe(cameraClient, outIndex);

    return client;
}

status_t CameraService::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) {
    // Permission checks
    switch (code) {
        case BnCameraService::CONNECT:
        case BnCameraService::CONNECT_PRO:
            const int pid = getCallingPid();
            const int self_pid = getpid();
            if (pid != self_pid) {
                // we're called from a different process, do the real check
                if (!checkCallingPermission(
                        String16("android.permission.CAMERA"))) {
                    const int uid = getCallingUid();
                    ALOGE("Permission Denial: "
                         "can't use the camera pid=%d, uid=%d", pid, uid);
                    return PERMISSION_DENIED;
                }
            }
            break;
    }

    return BnCameraService::onTransact(code, data, reply, flags);
}

// The reason we need this busy bit is a new CameraService::connect() request
// may come in while the previous Client's destructor has not been run or is
// still running. If the last strong reference of the previous Client is gone
// but the destructor has not been finished, we should not allow the new Client
// to be created because we need to wait for the previous Client to tear down
// the hardware first.
void CameraService::setCameraBusy(int cameraId) {
    android_atomic_write(1, &mBusy[cameraId]);

    ALOGV("setCameraBusy cameraId=%d", cameraId);
}

void CameraService::setCameraFree(int cameraId) {
    android_atomic_write(0, &mBusy[cameraId]);

    ALOGV("setCameraFree cameraId=%d", cameraId);
}

// We share the media players for shutter and recording sound for all clients.
// A reference count is kept to determine when we will actually release the
// media players.

MediaPlayer* CameraService::newMediaPlayer(const char *file) {
    MediaPlayer* mp = new MediaPlayer();
    if (mp->setDataSource(file, NULL) == NO_ERROR) {
        mp->setAudioStreamType(AUDIO_STREAM_ENFORCED_AUDIBLE);
        mp->prepare();
    } else {
        ALOGE("Failed to load CameraService sounds: %s", file);
        return NULL;
    }
    return mp;
}

void CameraService::loadSound() {
    Mutex::Autolock lock(mSoundLock);
    LOG1("CameraService::loadSound ref=%d", mSoundRef);
    if (mSoundRef++) return;

    mSoundPlayer[SOUND_SHUTTER] = newMediaPlayer("/system/media/audio/ui/camera_click.ogg");
    mSoundPlayer[SOUND_RECORDING] = newMediaPlayer("/system/media/audio/ui/VideoRecord.ogg");
}

void CameraService::releaseSound() {
    Mutex::Autolock lock(mSoundLock);
    LOG1("CameraService::releaseSound ref=%d", mSoundRef);
    if (--mSoundRef) return;

    for (int i = 0; i < NUM_SOUNDS; i++) {
        if (mSoundPlayer[i] != 0) {
            mSoundPlayer[i]->disconnect();
            mSoundPlayer[i].clear();
        }
    }
}

void CameraService::playSound(sound_kind kind) {
    LOG1("playSound(%d)", kind);
    Mutex::Autolock lock(mSoundLock);
    sp<MediaPlayer> player = mSoundPlayer[kind];
    if (player != 0) {
        player->seekTo(0);
        player->start();
    }
}

// ----------------------------------------------------------------------------

CameraService::Client::Client(const sp<CameraService>& cameraService,
        const sp<ICameraClient>& cameraClient,
        const String16& clientPackageName,
        int cameraId, int cameraFacing,
        int clientPid, uid_t clientUid,
        int servicePid) :
        CameraService::BasicClient(cameraService, cameraClient->asBinder(),
                clientPackageName,
                cameraId, cameraFacing,
                clientPid, clientUid,
                servicePid)
{
    int callingPid = getCallingPid();
    LOG1("Client::Client E (pid %d, id %d)", callingPid, cameraId);

    mCameraClient = cameraClient;

    cameraService->setCameraBusy(cameraId);
    cameraService->loadSound();

    LOG1("Client::Client X (pid %d, id %d)", callingPid, cameraId);
}

// tear down the client
CameraService::Client::~Client() {
    mDestructionStarted = true;

    mCameraService->releaseSound();
    finishCameraOps();
    // unconditionally disconnect. function is idempotent
    Client::disconnect();
}

CameraService::BasicClient::BasicClient(const sp<CameraService>& cameraService,
        const sp<IBinder>& remoteCallback,
        const String16& clientPackageName,
        int cameraId, int cameraFacing,
        int clientPid, uid_t clientUid,
        int servicePid):
        mClientPackageName(clientPackageName)
{
    mCameraService = cameraService;
    mRemoteCallback = remoteCallback;
    mCameraId = cameraId;
    mCameraFacing = cameraFacing;
    mClientPid = clientPid;
    mClientUid = clientUid;
    mServicePid = servicePid;
    mOpsActive = false;
    mDestructionStarted = false;
}

CameraService::BasicClient::~BasicClient() {
    mDestructionStarted = true;
}

void CameraService::BasicClient::disconnect() {
    mCameraService->removeClientByRemote(mRemoteCallback);
}

status_t CameraService::BasicClient::startCameraOps() {
    int32_t res;

    mOpsCallback = new OpsCallback(this);

    mAppOpsManager.startWatchingMode(AppOpsManager::OP_CAMERA,
            mClientPackageName, mOpsCallback);
    res = mAppOpsManager.startOp(AppOpsManager::OP_CAMERA,
            mClientUid, mClientPackageName);

    if (res != AppOpsManager::MODE_ALLOWED) {
        ALOGI("Camera %d: Access for \"%s\" has been revoked",
                mCameraId, String8(mClientPackageName).string());
        return PERMISSION_DENIED;
    }
    mOpsActive = true;
    return OK;
}

status_t CameraService::BasicClient::finishCameraOps() {
    if (mOpsActive) {
        mAppOpsManager.finishOp(AppOpsManager::OP_CAMERA, mClientUid,
                mClientPackageName);
        mOpsActive = false;
    }
    mAppOpsManager.stopWatchingMode(mOpsCallback);
    mOpsCallback.clear();

    return OK;
}

void CameraService::BasicClient::opChanged(int32_t op, const String16& packageName) {
    String8 name(packageName);
    String8 myName(mClientPackageName);

    if (op != AppOpsManager::OP_CAMERA) {
        ALOGW("Unexpected app ops notification received: %d", op);
        return;
    }

    int32_t res;
    res = mAppOpsManager.checkOp(AppOpsManager::OP_CAMERA,
            mClientUid, mClientPackageName);
    ALOGV("checkOp returns: %d, %s ", res,
            res == AppOpsManager::MODE_ALLOWED ? "ALLOWED" :
            res == AppOpsManager::MODE_IGNORED ? "IGNORED" :
            res == AppOpsManager::MODE_ERRORED ? "ERRORED" :
            "UNKNOWN");

    if (res != AppOpsManager::MODE_ALLOWED) {
        ALOGI("Camera %d: Access for \"%s\" revoked", mCameraId,
                myName.string());
        // Reset the client PID to allow server-initiated disconnect,
        // and to prevent further calls by client.
        mClientPid = getCallingPid();
        notifyError();
        disconnect();
    }
}

// ----------------------------------------------------------------------------

Mutex* CameraService::Client::getClientLockFromCookie(void* user) {
    return gCameraService->getClientLockById((int) user);
}

// Provide client pointer for callbacks. Client lock returned from getClientLockFromCookie should
// be acquired for this to be safe
CameraService::Client* CameraService::Client::getClientFromCookie(void* user) {
    Client* client = gCameraService->getClientByIdUnsafe((int) user);

    // This could happen if the Client is in the process of shutting down (the
    // last strong reference is gone, but the destructor hasn't finished
    // stopping the hardware).
    if (client == NULL) return NULL;

    // destruction already started, so should not be accessed
    if (client->mDestructionStarted) return NULL;

    return client;
}

void CameraService::Client::notifyError() {
    mCameraClient->notifyCallback(CAMERA_MSG_ERROR, CAMERA_ERROR_RELEASED, 0);
}

// NOTE: function is idempotent
void CameraService::Client::disconnect() {
    BasicClient::disconnect();
    mCameraService->setCameraFree(mCameraId);
}

CameraService::Client::OpsCallback::OpsCallback(wp<BasicClient> client):
        mClient(client) {
}

void CameraService::Client::OpsCallback::opChanged(int32_t op,
        const String16& packageName) {
    sp<BasicClient> client = mClient.promote();
    if (client != NULL) {
        client->opChanged(op, packageName);
    }
}

// ----------------------------------------------------------------------------
//                  IProCamera
// ----------------------------------------------------------------------------

CameraService::ProClient::ProClient(const sp<CameraService>& cameraService,
        const sp<IProCameraCallbacks>& remoteCallback,
        const String16& clientPackageName,
        int cameraId,
        int cameraFacing,
        int clientPid,
        uid_t clientUid,
        int servicePid)
        : CameraService::BasicClient(cameraService, remoteCallback->asBinder(),
                clientPackageName, cameraId, cameraFacing,
                clientPid,  clientUid, servicePid)
{
    mRemoteCallback = remoteCallback;
}

CameraService::ProClient::~ProClient() {
    mDestructionStarted = true;

    ProClient::disconnect();
}

status_t CameraService::ProClient::connect(const sp<IProCameraCallbacks>& callbacks) {
    ALOGE("%s: not implemented yet", __FUNCTION__);

    return INVALID_OPERATION;
}

void CameraService::ProClient::disconnect() {
    BasicClient::disconnect();
}

status_t CameraService::ProClient::initialize(camera_module_t* module)
{
    ALOGW("%s: not implemented yet", __FUNCTION__);
    return OK;
}

status_t CameraService::ProClient::exclusiveTryLock() {
    ALOGE("%s: not implemented yet", __FUNCTION__);
    return INVALID_OPERATION;
}

status_t CameraService::ProClient::exclusiveLock() {
    ALOGE("%s: not implemented yet", __FUNCTION__);
    return INVALID_OPERATION;
}

status_t CameraService::ProClient::exclusiveUnlock() {
    ALOGE("%s: not implemented yet", __FUNCTION__);
    return INVALID_OPERATION;
}

bool CameraService::ProClient::hasExclusiveLock() {
    ALOGE("%s: not implemented yet", __FUNCTION__);
    return false;
}

status_t CameraService::ProClient::submitRequest(camera_metadata_t* request, bool streaming) {
    ALOGE("%s: not implemented yet", __FUNCTION__);

    free_camera_metadata(request);

    return INVALID_OPERATION;
}

status_t CameraService::ProClient::cancelRequest(int requestId) {
    ALOGE("%s: not implemented yet", __FUNCTION__);

    return INVALID_OPERATION;
}

status_t CameraService::ProClient::requestStream(int streamId) {
    ALOGE("%s: not implemented yet", __FUNCTION__);

    return INVALID_OPERATION;
}

status_t CameraService::ProClient::cancelStream(int streamId) {
    ALOGE("%s: not implemented yet", __FUNCTION__);

    return INVALID_OPERATION;
}

void CameraService::ProClient::notifyError() {
    ALOGE("%s: not implemented yet", __FUNCTION__);
}

// ----------------------------------------------------------------------------

static const int kDumpLockRetries = 50;
static const int kDumpLockSleep = 60000;

static bool tryLock(Mutex& mutex)
{
    bool locked = false;
    for (int i = 0; i < kDumpLockRetries; ++i) {
        if (mutex.tryLock() == NO_ERROR) {
            locked = true;
            break;
        }
        usleep(kDumpLockSleep);
    }
    return locked;
}

status_t CameraService::dump(int fd, const Vector<String16>& args) {
    String8 result;
    if (checkCallingPermission(String16("android.permission.DUMP")) == false) {
        result.appendFormat("Permission Denial: "
                "can't dump CameraService from pid=%d, uid=%d\n",
                getCallingPid(),
                getCallingUid());
        write(fd, result.string(), result.size());
    } else {
        bool locked = tryLock(mServiceLock);
        // failed to lock - CameraService is probably deadlocked
        if (!locked) {
            result.append("CameraService may be deadlocked\n");
            write(fd, result.string(), result.size());
        }

        bool hasClient = false;
        if (!mModule) {
            result = String8::format("No camera module available!\n");
            write(fd, result.string(), result.size());
            return NO_ERROR;
        }

        result = String8::format("Camera module HAL API version: 0x%x\n",
                mModule->common.hal_api_version);
        result.appendFormat("Camera module API version: 0x%x\n",
                mModule->common.module_api_version);
        result.appendFormat("Camera module name: %s\n",
                mModule->common.name);
        result.appendFormat("Camera module author: %s\n",
                mModule->common.author);
        result.appendFormat("Number of camera devices: %d\n\n", mNumberOfCameras);
        write(fd, result.string(), result.size());
        for (int i = 0; i < mNumberOfCameras; i++) {
            result = String8::format("Camera %d static information:\n", i);
            camera_info info;

            status_t rc = mModule->get_camera_info(i, &info);
            if (rc != OK) {
                result.appendFormat("  Error reading static information!\n");
                write(fd, result.string(), result.size());
            } else {
                result.appendFormat("  Facing: %s\n",
                        info.facing == CAMERA_FACING_BACK ? "BACK" : "FRONT");
                result.appendFormat("  Orientation: %d\n", info.orientation);
                int deviceVersion;
                if (mModule->common.module_api_version <
                        CAMERA_MODULE_API_VERSION_2_0) {
                    deviceVersion = CAMERA_DEVICE_API_VERSION_1_0;
                } else {
                    deviceVersion = info.device_version;
                }
                result.appendFormat("  Device version: 0x%x\n", deviceVersion);
                if (deviceVersion >= CAMERA_DEVICE_API_VERSION_2_0) {
                    result.appendFormat("  Device static metadata:\n");
                    write(fd, result.string(), result.size());
                    dump_indented_camera_metadata(info.static_camera_characteristics,
                            fd, 2, 4);
                } else {
                    write(fd, result.string(), result.size());
                }
            }

            sp<Client> client = mClient[i].promote();
            if (client == 0) {
                result = String8::format("  Device is closed, no client instance\n");
                write(fd, result.string(), result.size());
                continue;
            }
            hasClient = true;
            result = String8::format("  Device is open. Client instance dump:\n");
            write(fd, result.string(), result.size());
            client->dump(fd, args);
        }
        if (!hasClient) {
            result = String8::format("\nNo active camera clients yet.\n");
            write(fd, result.string(), result.size());
        }

        if (locked) mServiceLock.unlock();

        // change logging level
        int n = args.size();
        for (int i = 0; i + 1 < n; i++) {
            String16 verboseOption("-v");
            if (args[i] == verboseOption) {
                String8 levelStr(args[i+1]);
                int level = atoi(levelStr.string());
                result = String8::format("\nSetting log level to %d.\n", level);
                setLogLevel(level);
                write(fd, result.string(), result.size());
            }
        }

    }
    return NO_ERROR;
}

/*virtual*/void CameraService::binderDied(
    const wp<IBinder> &who) {

    /**
      * While tempting to promote the wp<IBinder> into a sp,
      * it's actually not supported by the binder driver
      */

    ALOGV("java clients' binder died");

    sp<BasicClient> cameraClient = getClientByRemote(who);

    if (cameraClient == 0) {
        ALOGV("java clients' binder death already cleaned up (normal case)");
        return;
    }

    ALOGW("Disconnecting camera client %p since the binder for it "
          "died (this pid %d)", cameraClient.get(), getCallingPid());

    cameraClient->disconnect();

}

}; // namespace android
