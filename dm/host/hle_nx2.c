// SDK calls only Switch 2 (Ounce) programs make (Mario Kart World, etc.)
// Not yet implemented or needed elsewhere in the host.
#include "hle.h"
#include <string.h>

// nn::hid: Joy-Con mouse mode (Nmouse)
HLE_FORCE("_ZN2nn3hid14FinalizeNmouseEv", "Switch 2 hid: no mouse mode", void, h_nx2_MouseFini, void) {}
HLE_FORCE("_ZN2nn3hid15GetNmouseStatesEPiPNS0_11NmouseStateEiNS0_12NmouseHandleE", "Switch 2 hid: no mouse mode", void, h_nx2_MouseStates, int *count, void *out, int n, uint32_t h) { (void)out; (void)n; (void)h; if (count) *count = 0; }
HLE_FORCE("_ZN2nn3hid16GetNmouseTypeSetEPNS_4util10BitFlagSetILi32ENS0_10NmouseTypeEEENS0_12NmouseHandleE", "Switch 2 hid: no mouse mode", void, h_nx2_MouseTypes, uint32_t *out, uint32_t h) { (void)h; if (out) *out = 0; }
HLE_FORCE("_ZN2nn3hid19EnableNmousePointerENS0_12NmouseHandleEb", "Switch 2 hid: no mouse mode", void, h_nx2_MouseEnable, uint32_t h, uint32_t on) { (void)h; (void)on; }
HLE_FORCE("_ZN2nn3hid22SetNmousePointerRangesEii", "Switch 2 hid: no mouse mode", void, h_nx2_MouseRanges, int w, int h) { (void)w; (void)h; }
HLE_FORCE("_ZN2nn3hid24SetNmousePointerPositionENS0_12NmouseHandleEii", "Switch 2 hid: no mouse mode", void, h_nx2_MousePos, uint32_t h, int x, int y) { (void)h; (void)x; (void)y; }

// nn::oe
HLE_FORCE("_ZN2nn2oe32BeginPresentPhotographedContentsEv", "Switch 2 oe: no capture restrictions here", void, h_nx2_PhotoBegin, void) {}
HLE_FORCE("_ZN2nn2oe30EndPresentPhotographedContentsEv", "Switch 2 oe: no capture restrictions here", void, h_nx2_PhotoEnd, void) {}

// nn::chat: GameChat is off, nobody is chatting
HLE_FORCE("_ZN2nn4chat10InitializeEv", "GameChat: off", uint32_t, h_nx2_ChatInit, void) { return 0; }
HLE_FORCE("_ZN2nn4chat10IsChattingEv", "GameChat: nobody chatting", uint32_t, h_nx2_ChatIs, void) { return 0; }
HLE_FORCE("_ZN2nn4chat11GetRoomInfoEPNS0_8RoomInfoE", "GameChat: no room", uint32_t, h_nx2_ChatRoom, void *out) { (void)out; return 0; }
HLE_FORCE("_ZN2nn4chat13SetSharingKeyERKNS0_10SharingKeyE", "GameChat: off", void, h_nx2_ChatKeySet, const void *k) { (void)k; }
HLE_FORCE("_ZN2nn4chat15ClearSharingKeyEv", "GameChat: off", void, h_nx2_ChatKeyClr, void) {}
HLE_FORCE("_ZN2nn4chat13ShowChatStartERKNS_7account3UidE", "GameChat: off", void, h_nx2_ChatShow, const void *u) { (void)u; }
HLE_FORCE("_ZN2nn4chat20SetScreenSharingModeENS0_17ScreenSharingModeE", "GameChat: off", void, h_nx2_ChatShare, int m) { (void)m; }
HLE_FORCE("_ZN2nn4chat22ShowCameraSharingGuideEv", "GameChat: off", void, h_nx2_ChatGuide, void) {}
HLE_FORCE("_ZN2nn4chat28AttachToRoomInfoChangedEventEPNS_2os11SystemEventE", "GameChat: bound, never signaled", uint32_t, h_nx2_ChatRoomEv, void *ev) { hle_bind_system_event(ev, 0); return 0; }
HLE_FORCE("_ZN2nn4chat36ShowLocalCameraConfigurationVersion0ENS0_25CameraImageProcessingModeE", "GameChat: no camera", void, h_nx2_ChatCfg, int m) { (void)m; }
HLE_FORCE("_ZN2nn4chat46BeginFullScreenWithLocalCameraAvailableSectionEb", "GameChat: no camera", void, h_nx2_ChatFsBegin, uint32_t b) { (void)b; }
HLE_FORCE("_ZN2nn4chat44EndFullScreenWithLocalCameraAvailableSectionEv", "GameChat: no camera", void, h_nx2_ChatFsEnd, void) {}
HLE_FORCE("_ZN2nn4chat25StartLocalCameraStreamingEv", "GameChat: no camera", void, h_nx2_ChatCamOn, void) {}
HLE_FORCE("_ZN2nn4chat24StopLocalCameraStreamingEv", "GameChat: no camera", void, h_nx2_ChatCamOff, void) {}
HLE_FORCE("_ZN2nn4chat26StartRemoteCameraStreamingERKNS_7account23NetworkServiceAccountIdE", "GameChat: no camera", void, h_nx2_ChatRemOn, const void *i) { (void)i; }
HLE_FORCE("_ZN2nn4chat25StopRemoteCameraStreamingERKNS_7account23NetworkServiceAccountIdE", "GameChat: no camera", void, h_nx2_ChatRemOff, const void *i) { (void)i; }
HLE_FORCE("_ZN2nn4chat35CalculateLocalCameraVideoBufferSizeEiii", "GameChat: no camera, one page", uint64_t, h_nx2_ChatLSize, int w, int h, int n) { (void)w; (void)h; (void)n; return 0x1000; }
HLE_FORCE("_ZN2nn4chat36CalculateRemoteCameraVideoBufferSizeEiii", "GameChat: no camera, one page", uint64_t, h_nx2_ChatRSize, int w, int h, int n) { (void)w; (void)h; (void)n; return 0x1000; }
HLE_FORCE("_ZN2nn4chat33InitializeLocalCameraVideoStorageEPNS0_20LocalCameraVideoTypeEiiiPvm", "GameChat: no camera", void, h_nx2_ChatLInit, void *t, int w, int h, int n, void *p, uint64_t sz) { (void)t; (void)w; (void)h; (void)n; (void)p; (void)sz; }
HLE_FORCE("_ZN2nn4chat34InitializeRemoteCameraVideoStorageEPNS0_21RemoteCameraVideoTypeEiiiPvm", "GameChat: no camera", void, h_nx2_ChatRInit, void *t, int w, int h, int n, void *p, uint64_t sz) { (void)t; (void)w; (void)h; (void)n; (void)p; (void)sz; }
HLE_FORCE("_ZN2nn4chat31FinalizeLocalCameraVideoStorageEPNS0_20LocalCameraVideoTypeE", "GameChat: no camera", void, h_nx2_ChatLFini, void *t) { (void)t; }
HLE_FORCE("_ZN2nn4chat32FinalizeRemoteCameraVideoStorageEPNS0_21RemoteCameraVideoTypeE", "GameChat: no camera", void, h_nx2_ChatRFini, void *t) { (void)t; }
HLE_FORCE("_ZN2nn4chat25GetLocalCameraImageHandleEPKNS0_20LocalCameraVideoTypeEi", "GameChat: no camera", uint64_t, h_nx2_ChatLHandle, const void *t, int i) { (void)t; (void)i; return 0; }
HLE_FORCE("_ZN2nn4chat26GetRemoteCameraImageHandleEPKNS0_21RemoteCameraVideoTypeEi", "GameChat: no camera", uint64_t, h_nx2_ChatRHandle, const void *t, int i) { (void)t; (void)i; return 0; }
HLE_FORCE("_ZN2nn4chat27RequestNextLocalCameraImageEPNS_2gs16NativeSyncHandleEPNS0_20LocalCameraVideoTypeEiS2_", "GameChat: no camera", uint32_t, h_nx2_ChatLNext, void *a, void *t, int i, void *b) { (void)a; (void)t; (void)i; (void)b; return 0; }
HLE_FORCE("_ZN2nn4chat28RequestNextRemoteCameraImageEPNS_2gs16NativeSyncHandleEPNS0_21RemoteCameraVideoTypeEiS2_RKNS_7account23NetworkServiceAccountIdE", "GameChat: no camera", uint32_t, h_nx2_ChatRNext, void *a, void *t, int i, void *b, const void *c) { (void)a; (void)t; (void)i; (void)b; (void)c; return 0; }

// nn::gs
HLE_FORCE("_ZN2nn2gs3sdk30CheckGraphicsSystemInitializedEPKcm", "the graphics servers do not run here", void, h_nx2_GsCheck, const char *s, uint64_t n) { (void)s; (void)n; }
HLE_FORCE("_ZN2nn2gs3sdk18CreateNativeMemoryEPNS0_18NativeMemoryHandleEPvmNS_2os16MemoryPermissionENS0_3cpu18MemoryCacheabilityE", "NVN2 native memory: no server", uint32_t, h_nx2_NmCreate, uint64_t *h, void *p, uint64_t n, int perm, int cache) { (void)p; (void)n; (void)perm; (void)cache; if (h) *h = 1; return 0; }
HLE_FORCE("_ZN2nn2gs3sdk19DestroyNativeMemoryENS0_18NativeMemoryHandleE", "NVN2 native memory: no server", void, h_nx2_NmDestroy, uint64_t h) { (void)h; }

// nn::camera: no camera. The game creates a context at start, and a non-zero Result stops it (main+0xcdec08)
HLE_FORCE("_ZN2nn6camera3sdk22CreateOutputBufferPoolEPNS_3gsv15UserImageHandleERKNS2_7imgdesc4impl4user5ounce25CameraUserImagePreferenceEjNS2_16UserMemoryHandleE", "Switch 2 camera: no server", uint32_t, h_nx2_CamPool, void *h, const void *p, uint32_t n, uint64_t m) { (void)h; (void)p; (void)n; (void)m; return 0; }
HLE_FORCE("_ZN2nn6camera3sdk23CalculateImageArraySizeEPNS_3gsv10MemorySizeES4_RKNS2_7imgdesc4impl4user5ounce25CameraUserImagePreferenceEj", "Switch 2 camera: no server, one page", uint32_t, h_nx2_CamSize, uint64_t *a, uint64_t *b, const void *p, uint32_t n) { (void)p; (void)n; if (a) *a = 0x1000; if (b) *b = 0x1000; return 0; }
HLE_FORCE("_ZN2nn6camera3sdk23DestroyOutputBufferPoolENS_3gsv15UserImageHandleE", "Switch 2 camera: no server", void, h_nx2_CamPoolFree, uint64_t h) { (void)h; }
HLE_FORCE("_ZN2nn6camera3sdk28DestroyAliasOutputBufferPoolENS_3gsv15UserImageHandleE", "Switch 2 camera: no server", void, h_nx2_CamAliasFree, uint64_t h) { (void)h; }

// nn::audio
HLE_FORCE("_ZN2nn5audio29GetDefaultVirtualOutputDeviceEPNS0_15AudioDeviceNameE", "Switch 2 audio device: one output", void, h_nx2_AudDev, char *name) { strlcpy(name, "MainAudioOut", 0x100); }
HLE_FORCE("_ZN2nn5audio32GetVirtualOutputDevicePropertiesEPNS0_23VirtualDevicePropertiesERKNS0_15AudioDeviceNameE", "Switch 2 audio device: no properties", void, h_nx2_AudProps, void *out, const void *name) { (void)out; (void)name; }

// nn::vi
HLE_FORCE("_ZN2nn2vi39SetLayerNextHdrAdjustableBezierMetadataEPNS0_5LayerEfff", "Switch 2 vi: no HDR", uint32_t, h_nx2_HdrBez, void *l, float a, float b, float c) { (void)l; (void)a; (void)b; (void)c; return 0; }
